#include "controller.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <unordered_set>
#include <utility>

#include <poll.h>
#include <unistd.h>

namespace mea {
namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
constexpr milliseconds watchdog_interval{500};
constexpr std::size_t max_line_bytes = 1024 * 1024;

std::int64_t unix_ms() {
    return std::chrono::duration_cast<milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Command stop(const std::string& event_id, const char* reason) {
    return {event_id, "stop", {}, reason};
}

Command map_window(const Window& window, Speeds speeds, bool live, Clock::time_point received) {
    const auto& id = window.event_id;
    if (window.error) return stop(id, window.error);
    if (!window.accepted) return stop(id, "rejected");
    if (window.intent == Intent::none) return stop(id, "empty_intent");
    if (window.intent == Intent::unsupported) return stop(id, "unsupported_intent");
    if (window.expires_at_ms && *window.expires_at_ms <= unix_ms()) {
        return stop(id, "expired");
    }
    if (window.intent == Intent::stop) return stop(id, "accepted");
    if (live && !window.expires_at_ms) return stop(id, "missing_expiry");
    if (live && Clock::now() >= received + watchdog_interval) {
        return stop(id, "watchdog_timeout");
    }

    switch (window.intent) {
        case Intent::forward:  return {id, "forward", {speeds.linear, 0, 0}, "accepted"};
        case Intent::backward: return {id, "backward", {-speeds.linear, 0, 0}, "accepted"};
        case Intent::left:     return {id, "left", {0, 0, speeds.angular}, "accepted"};
        case Intent::right:    return {id, "right", {0, 0, -speeds.angular}, "accepted"};
        default:              return stop(id, "unsupported_intent");
    }
}

bool send(Backend& backend, const Command& command) {
    try {
        const int result = backend.send(command);
        if (result == 0) {
            return true;
        }
        std::cerr << "Backend error " << result << " (" << command.reason << ")\n";
    } catch (const std::exception& error) {
        std::cerr << "Backend error (" << command.reason << "): " << error.what() << '\n';
    } catch (...) {
        std::cerr << "Backend error (" << command.reason << "): unknown exception\n";
    }
    return false;
}

struct Timer {
    int wait_ms = -1;
    const char* reason = nullptr;
};

class Controller {
public:
    Controller(Backend& backend, Speeds speeds, bool live)
        : backend_(backend), speeds_(speeds), live_(live) {}

    Timer timer() const {
        if (!active_) {
            return {};
        }
        const auto watchdog_ms = std::chrono::ceil<milliseconds>(
            active_->watchdog_at - Clock::now()).count();
        const auto now_ms = std::max<std::int64_t>(0, unix_ms());
        const auto expiry_ms = active_->expires_at_ms - now_ms;
        if (expiry_ms <= watchdog_ms) {
            return {static_cast<int>(std::max<std::int64_t>(0, expiry_ms)), "expired"};
        }
        return {static_cast<int>(std::max<std::int64_t>(0, watchdog_ms)), "watchdog_timeout"};
    }

    bool stop_if_due() {
        const auto next = timer();
        if (next.wait_ms != 0) {
            return true;
        }
        auto command = stop(active_->event_id, next.reason);
        active_.reset();
        return send(backend_, command);
    }

    bool handle(const Window& window, Clock::time_point received) {
        if (!window.error && !seen_.insert(window.event_id).second) {
            backend_.skip(window.event_id);
            return true;
        }
        const auto command = map_window(window, speeds_, live_, received);
        if (!send(backend_, command)) {
            return false;
        }
        const auto& velocity = command.velocity;
        if (live_ && (velocity.vx != 0 || velocity.vy != 0 || velocity.vyaw != 0)) {
            active_ = Motion{window.event_id, *window.expires_at_ms, received + watchdog_interval};
        } else {
            active_.reset();
        }
        return true;
    }

private:
    struct Motion {
        std::string event_id;
        std::int64_t expires_at_ms;
        Clock::time_point watchdog_at;
    };

    Backend& backend_;
    Speeds speeds_;
    bool live_;
    std::unordered_set<std::string> seen_;
    std::optional<Motion> active_;
};

struct Exit {
    const char* reason;
    int code = 0;
};

Exit input_error(const char* operation) {
    std::cerr << operation << ": " << std::strerror(errno) << '\n';
    return {"input_error", 1};
}

Exit read_windows(int input_fd, int stop_fd, bool live, Controller& controller) {
    std::array<char, 4096> buffer;
    std::size_t offset = 0;
    std::size_t size = 0;
    std::string line;
    bool discarding = false;
    bool eof = false;
    std::optional<Window> pending;
    Clock::time_point received;

    for (;;) {
        const bool buffered = offset < size;
        const bool local_work = buffered || pending || eof;
        pollfd fds[] = {{stop_fd, POLLIN, 0}, {input_fd, POLLIN, 0}};
        const int ready = ::poll(fds, local_work ? 1 : 2,
                                 local_work ? 0 : controller.timer().wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return input_error("poll");
        }
        if (fds[0].revents & POLLIN) return {"signal"};
        if (fds[0].revents != 0) {
            std::cerr << "Shutdown descriptor failed\n";
            return {"input_error", 1};
        }
        if (!controller.stop_if_due()) return {"backend_error", 1};

        if (pending) {
            if (!controller.handle(*pending, received)) return {"backend_error", 1};
            pending.reset();
            continue;
        }
        if (eof) return {"eof"};

        if (buffered) {
            const std::string_view remaining(buffer.data() + offset, size - offset);
            const auto newline = remaining.find('\n');
            const bool complete = newline != std::string_view::npos;
            const auto part = remaining.substr(0, newline);
            offset += part.size() + (complete ? 1 : 0);

            if (discarding) {
                discarding = !complete;
            } else if (line.size() + part.size() > max_line_bytes) {
                pending.emplace();
                pending->error = "line_too_long";
                line.clear();
                discarding = !complete;
            } else {
                line.append(part);
                if (complete) {
                    pending = decode_window(line);
                    line.clear();
                }
            }
            continue;
        }

        if (!(fds[1].revents & (POLLIN | POLLHUP))) {
            if (fds[1].revents == 0) continue;
            std::cerr << "Input descriptor failed\n";
            return {"input_error", 1};
        }
        const auto count = ::read(input_fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return input_error("read");
        }
        if (count == 0) {
            eof = true;
            // Only replay treats an unterminated final record as a complete line.
            if (!live && !line.empty()) pending = decode_window(line);
        } else {
            received = Clock::now();
            offset = 0;
            size = static_cast<std::size_t>(count);
        }
    }
}

}  // namespace

int run_controller(int input_fd, int stop_fd, bool live, Speeds speeds, Backend& backend) {
    Controller controller(backend, speeds, live);
    Exit result{"controller_error", 1};
    try {
        result = read_windows(input_fd, stop_fd, live, controller);
    } catch (const std::exception& error) {
        std::cerr << "Controller error: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "Controller error: unknown exception\n";
    }
    if (live && !send(backend, stop({}, result.reason))) {
        return 1;
    }
    return result.code;
}

}  // namespace mea
