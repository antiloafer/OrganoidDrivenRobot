#include "controller.hpp"

#include <cerrno>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#ifdef MEA_ENABLE_G1
namespace mea {
std::unique_ptr<Backend> make_g1_backend(const std::string& interface);
}
#endif

namespace {

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

class LogBackend final : public mea::Backend {
public:
    int send(const mea::Command& command) override {
        std::cout << mea::format_log(command) << std::endl;
        return std::cout ? 0 : 1;
    }

    void skip(const std::string& event_id) override {
        const mea::Command command{event_id, "skip", {}, "duplicate"};
        std::cout << mea::format_log(command, false) << std::endl;
        if (!std::cout) throw std::runtime_error("writing log failed");
    }
};

struct Options {
    std::string input;
    std::string interface;
    mea::Speeds speeds;
    int watchdog_ms = 500;
    bool linear_set = false;
    bool lateral_set = false;
    bool angular_set = false;
    bool live_log = false;
    bool capabilities = false;
    bool help = false;
};

void usage(std::ostream& output) {
    output << "Usage: mea_g1 [--input <file>] [--live] [--linear-speed <m/s>] "
              "[--lateral-speed <m/s>] [--angular-speed <rad/s>] [--watchdog-ms <ms>]\n"
              "       mea_g1 --g1 <interface> --linear-speed <m/s> --angular-speed <rad/s> "
              "[--lateral-speed <m/s>] [--watchdog-ms <ms>]\n"
              "       mea_g1 --capabilities\n";
}

void print_capabilities(std::ostream& output) {
#ifdef MEA_ENABLE_G1
    constexpr const char* g1_flag = "true";
#else
    constexpr const char* g1_flag = "false";
#endif
    output << "{\"schema_version\":1,\"g1_backend\":" << g1_flag
           << ",\"live_log\":true,\"intents\":[\"locomotion.translate\","
              "\"locomotion.turn\",\"locomotion.stop\"]}\n";
}

double parse_speed(const std::string& value, const std::string& flag) {
    std::size_t consumed = 0;
    double speed = 0;
    try {
        speed = std::stod(value, &consumed);
    } catch (const std::invalid_argument&) {
        // Report the same option error for all invalid speed values.
    } catch (const std::out_of_range&) {
    }
    if (consumed != value.size() || !std::isfinite(speed) || speed <= 0 ||
        speed > std::numeric_limits<float>::max() || static_cast<float>(speed) == 0) {
        throw std::invalid_argument(flag + " requires a positive finite speed representable as float");
    }
    return speed;
}

int parse_watchdog_ms(const std::string& value) {
    std::size_t consumed = 0;
    long parsed = 0;
    try {
        parsed = std::stol(value, &consumed, 10);
    } catch (const std::invalid_argument&) {
    } catch (const std::out_of_range&) {
    }
    if (consumed != value.size() || parsed < 50 || parsed > 1000) {
        throw std::invalid_argument("--watchdog-ms requires an integer between 50 and 1000");
    }
    return static_cast<int>(parsed);
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h") {
            options.help = true;
            return options;
        }
        if (flag == "--capabilities") {
            options.capabilities = true;
            return options;
        }
        if (flag == "--live") {
            options.live_log = true;
            continue;
        }
        if (flag != "--input" && flag != "--g1" && flag != "--linear-speed" &&
            flag != "--lateral-speed" && flag != "--angular-speed" && flag != "--watchdog-ms") {
            throw std::invalid_argument("Unknown option: " + flag);
        }
        if (i + 1 == argc || argv[i + 1][0] == '\0' ||
            std::string_view(argv[i + 1]).substr(0, 2) == "--") {
            throw std::invalid_argument("Missing value for " + flag);
        }
        const std::string value = argv[++i];
        if (flag == "--input") {
            options.input = value;
        } else if (flag == "--g1") {
            options.interface = value;
        } else if (flag == "--linear-speed") {
            options.speeds.linear = parse_speed(value, flag);
            options.linear_set = true;
        } else if (flag == "--lateral-speed") {
            options.speeds.lateral = parse_speed(value, flag);
            options.lateral_set = true;
        } else if (flag == "--angular-speed") {
            options.speeds.angular = parse_speed(value, flag);
            options.angular_set = true;
        } else {
            options.watchdog_ms = parse_watchdog_ms(value);
        }
    }
    if (options.linear_set && !options.lateral_set) {
        options.speeds.lateral = options.speeds.linear;
    }
    if (options.live_log && !options.input.empty()) {
        throw std::invalid_argument("--live reads from stdin and disallows --input");
    }
    if (!options.interface.empty()) {
        if (!options.input.empty()) {
            throw std::invalid_argument("--g1 reads from stdin and disallows --input");
        }
        if (!options.linear_set || !options.angular_set) {
            throw std::invalid_argument("--g1 requires --linear-speed and --angular-speed");
        }
#ifndef MEA_ENABLE_G1
        throw std::invalid_argument("--g1 requires a build with MEA_ENABLE_G1=ON");
#endif
    }
    return options;
}

int open_signal_descriptor() {
    sigset_t mask;
    ::sigemptyset(&mask);
    ::sigaddset(&mask, SIGINT);
    ::sigaddset(&mask, SIGTERM);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        throw std::system_error(errno, std::generic_category(), "sigprocmask");
    }
    const int fd = ::signalfd(-1, &mask, SFD_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "signalfd");
    }
    return fd;
}

int open_input(const std::string& path) {
    if (path.empty()) return -1;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), path);
    }
    return fd;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        usage(std::cerr);
        return 2;
    }
    if (options.help) {
        usage(std::cout);
        return 0;
    }
    if (options.capabilities) {
        print_capabilities(std::cout);
        return 0;
    }

    try {
        if (options.input.empty() && ::fcntl(STDIN_FILENO, F_GETFD) < 0) {
            throw std::system_error(errno, std::generic_category(), "stdin");
        }
        const Descriptor signals(open_signal_descriptor());
        const Descriptor file(open_input(options.input));
        const int input_fd = options.input.empty() ? STDIN_FILENO : file.get();
#ifdef MEA_ENABLE_G1
        if (!options.interface.empty()) {
            auto g1 = mea::make_g1_backend(options.interface);
            return mea::run_controller(input_fd, signals.get(), true, options.speeds, *g1,
                                       options.watchdog_ms);
        }
#endif
        LogBackend log;
        return mea::run_controller(input_fd, signals.get(), options.live_log, options.speeds, log,
                                   options.watchdog_ms);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
