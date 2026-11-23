#include "controller.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Pipe {
public:
    Pipe() { require(::pipe(fds_) == 0, "pipe failed"); }
    ~Pipe() {
        close_writer();
        ::close(fds_[0]);
    }
    int reader() const { return fds_[0]; }

    void close_writer() {
        if (fds_[1] >= 0) {
            ::close(fds_[1]);
            fds_[1] = -1;
        }
    }

    void write(std::string_view bytes) const {
        while (!bytes.empty()) {
            const auto written = ::write(fds_[1], bytes.data(), bytes.size());
            if (written < 0 && errno == EINTR) continue;
            require(written > 0, "pipe write failed");
            bytes.remove_prefix(static_cast<std::size_t>(written));
        }
    }

private:
    int fds_[2];
};

struct Sent {
    mea::Command command;
    Clock::time_point at;
};

class FakeBackend final : public mea::Backend {
public:
    explicit FakeBackend(std::size_t fail_at) : fail_at_(fail_at) {}

    int send(const mea::Command& command) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_.push_back({command, Clock::now()});
        changed_.notify_all();
        return sent_.size() == fail_at_ ? 42 : 0;
    }

    void skip(const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++duplicates_;
        changed_.notify_all();
    }

    Sent wait_for_send(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, 2s, [&] { return sent_.size() >= count; }),
                "timed out waiting for backend command");
        return sent_[count - 1];
    }

    void wait_for_duplicate() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, 2s, [&] { return duplicates_ != 0; }),
                "duplicate was not reported");
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Sent> sent_;
    std::size_t fail_at_;
    std::size_t duplicates_ = 0;
};

class Session {
public:
    explicit Session(std::size_t fail_at = 0, mea::Speeds speeds = {}, int watchdog_ms = 500)
        : backend(fail_at), worker_([this, speeds, watchdog_ms] {
              result_ = mea::run_controller(input.reader(), stop.reader(), true, speeds, backend,
                                            watchdog_ms);
          }) {}

    ~Session() {
        input.close_writer();
        if (worker_.joinable()) worker_.join();
    }

    int join() {
        worker_.join();
        return result_;
    }

    Pipe input;
    Pipe stop;
    FakeBackend backend;

private:
    int result_ = -1;
    std::thread worker_;
};

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string valid_timing(std::chrono::milliseconds remaining) {
    const auto issued = now_unix_ms();
    const auto expires = issued + remaining.count();
    return "{\"issued_at_unix_ms\":" + std::to_string(issued) +
           ",\"expires_at_unix_ms\":" + std::to_string(expires) + "}";
}

std::string make_event(std::string_view id, std::string_view intent_name,
                       std::string_view direction, const std::string& timing_json) {
    const std::string params = direction.empty()
        ? "{}" : "{\"direction\":\"" + std::string(direction) + "\"}";
    return "{\"schema_version\":1,\"event_id\":\"" + std::string(id) +
        "\",\"decision\":{\"status\":\"accepted\",\"reason\":null},"
        "\"intent\":{\"name\":\"" + std::string(intent_name) + "\",\"parameters\":" + params +
        "},\"live_timing\":" + timing_json + "}\n";
}

std::string forward(std::string_view id, const std::string& timing_json = valid_timing(5s)) {
    return make_event(id, "locomotion.translate", "forward", timing_json);
}

void require_stop(const Sent& sent, std::string_view reason, std::string_view event_id = {}) {
    const auto& command = sent.command;
    require(command.action == std::string_view("stop"), "expected stop action");
    require(command.velocity.vx == 0 && command.velocity.vy == 0 && command.velocity.vyaw == 0,
            "stop must send all zero velocities");
    require(command.reason == reason, "unexpected stop reason");
    require(command.event_id == event_id, "unexpected stop event ID");
}

void strafe_and_turn_velocities() {
    Session session(0, mea::Speeds{0.25, 0.15, 0.4}, 500);
    session.input.write(
        make_event("fwd", "locomotion.translate", "forward", valid_timing(5s)) +
        make_event("sl", "locomotion.translate", "left", valid_timing(5s)) +
        make_event("sr", "locomotion.translate", "right", valid_timing(5s)) +
        make_event("tl", "locomotion.turn", "left", valid_timing(5s)) +
        make_event("tr", "locomotion.turn", "right", valid_timing(5s)) +
        make_event("st", "locomotion.stop", "", valid_timing(5s))
    );

    const auto c1 = session.backend.wait_for_send(1).command;
    require(c1.action == std::string_view("forward") && c1.velocity.vx == 0.25 && c1.velocity.vy == 0 && c1.velocity.vyaw == 0,
            "unexpected forward velocity");
    const auto c2 = session.backend.wait_for_send(2).command;
    require(c2.action == std::string_view("strafe_left") && c2.velocity.vx == 0 && c2.velocity.vy == 0.15 && c2.velocity.vyaw == 0,
            "unexpected strafe_left velocity");
    const auto c3 = session.backend.wait_for_send(3).command;
    require(c3.action == std::string_view("strafe_right") && c3.velocity.vx == 0 && c3.velocity.vy == -0.15 && c3.velocity.vyaw == 0,
            "unexpected strafe_right velocity");
    const auto c4 = session.backend.wait_for_send(4).command;
    require(c4.action == std::string_view("turn_left") && c4.velocity.vx == 0 && c4.velocity.vy == 0 && c4.velocity.vyaw == 0.4,
            "unexpected turn_left velocity");
    const auto c5 = session.backend.wait_for_send(5).command;
    require(c5.action == std::string_view("turn_right") && c5.velocity.vx == 0 && c5.velocity.vy == 0 && c5.velocity.vyaw == -0.4,
            "unexpected turn_right velocity");
    require_stop(session.backend.wait_for_send(6), "accepted", "st");
}

void watchdog_with_partial_input_and_duplicate() {
    Session session;
    session.input.write(forward("watchdog"));
    const auto motion = session.backend.wait_for_send(1);
    require(motion.command.velocity.vx == 0.2, "expected forward movement");

    std::this_thread::sleep_until(motion.at + 300ms);
    session.input.write(forward("watchdog") + "{\"partial\":");
    session.backend.wait_for_duplicate();
    const auto stopped = session.backend.wait_for_send(2);
    require_stop(stopped, "watchdog_timeout", "watchdog");
    require(stopped.at - motion.at >= 450ms && stopped.at - motion.at < 740ms,
            "watchdog did not fire around 500 ms after the original event");

    session.input.close_writer();
    require_stop(session.backend.wait_for_send(3), "eof");
    require(session.join() == 0, "EOF should exit successfully");
}

void expiry_precedes_watchdog() {
    Session session;
    session.input.write(forward("expiry", valid_timing(180ms)));
    const auto motion = session.backend.wait_for_send(1);
    require(motion.command.velocity.vx > 0, "valid timestamp must start motion");
    const auto stopped = session.backend.wait_for_send(2);
    require_stop(stopped, "expired", "expiry");
    require(stopped.at - motion.at < 450ms, "expiry must not wait for the watchdog");
}

void live_requires_valid_expiry() {
    Session session;
    session.input.write(
        forward("missing", "null") +
        forward("expired", "{\"issued_at_unix_ms\":1,\"expires_at_unix_ms\":2}") +
        forward("wrong-type", "{\"issued_at_unix_ms\":1,\"expires_at_unix_ms\":\"123\"}") +
        forward("missing-issued", "{\"expires_at_unix_ms\":9999999999999}") +
        forward("bad-order", "{\"issued_at_unix_ms\":500,\"expires_at_unix_ms\":500}") +
        forward("overflow", "{\"issued_at_unix_ms\":1,\"expires_at_unix_ms\":18446744073709551615}")
    );
    require_stop(session.backend.wait_for_send(1), "missing_expiry", "missing");
    require_stop(session.backend.wait_for_send(2), "expired", "expired");
    require_stop(session.backend.wait_for_send(3), "invalid_expiry", "wrong-type");
    require_stop(session.backend.wait_for_send(4), "invalid_expiry", "missing-issued");
    require_stop(session.backend.wait_for_send(5), "invalid_expiry", "bad-order");
    require_stop(session.backend.wait_for_send(6), "invalid_expiry", "overflow");
}

void shutdown_while_moving(bool signal) {
    Session session;
    session.input.write(forward("moving"));
    require(session.backend.wait_for_send(1).command.velocity.vx > 0, "expected motion");
    if (signal) {
        session.stop.write("x");
    } else {
        session.input.close_writer();
    }
    require_stop(session.backend.wait_for_send(2), signal ? "signal" : "eof");
    require(session.join() == 0, "shutdown should exit successfully");
}

void backend_failure_stops_and_exits() {
    Session session(1);
    session.input.write(forward("failure"));
    require(session.backend.wait_for_send(1).command.velocity.vx > 0, "expected a failed move");
    require_stop(session.backend.wait_for_send(2), "backend_error");
    require(session.join() != 0, "backend failure must exit with an error");
}

void failing_shutdown_stop_is_an_error() {
    Session session(2);
    session.input.write(forward("stop-failure"));
    require(session.backend.wait_for_send(1).command.velocity.vx > 0, "expected motion");
    session.stop.write("x");
    require_stop(session.backend.wait_for_send(2), "signal");
    require(session.join() != 0, "shutdown stop failure must exit with an error");
}

}  // namespace

int main() {
    try {
        strafe_and_turn_velocities();
        watchdog_with_partial_input_and_duplicate();
        expiry_precedes_watchdog();
        live_requires_valid_expiry();
        shutdown_while_moving(false);
        shutdown_while_moving(true);
        backend_failure_stops_and_exits();
        failing_shutdown_stop_is_an_error();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
