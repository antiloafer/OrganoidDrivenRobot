#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mea {

enum class Intent { none, unsupported, forward, backward, left, right, stop };

struct Window {
    std::string event_id;
    bool accepted = false;
    Intent intent = Intent::none;
    std::optional<std::int64_t> expires_at_ms;
    const char* error = nullptr;
};

Window decode_window(std::string_view line);

struct Velocity {
    double vx = 0;
    double vy = 0;
    double vyaw = 0;
};

struct Speeds {
    double linear = 0.2;
    double angular = 0.3;
};

struct Command {
    std::string event_id;
    const char* action;
    Velocity velocity;
    const char* reason;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual int send(const Command& command) = 0;
    virtual void skip(const std::string&) {}
};

std::string format_log(const Command& command, bool applied = true);

// stop_fd becomes readable when shutdown is requested.
int run_controller(int input_fd, int stop_fd, bool live, Speeds speeds, Backend& backend);

}  // namespace mea
