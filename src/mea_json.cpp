#include "controller.hpp"

#include <limits>

#include <nlohmann/json.hpp>

namespace mea {
namespace {

using nlohmann::json;

bool read_positive_int64(const json& value, std::int64_t& out) {
    if (!value.is_number_integer()) {
        return false;
    }
    constexpr auto max_int64 =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > max_int64) {
        return false;
    }
    const auto parsed = value.get<std::int64_t>();
    if (parsed <= 0) {
        return false;
    }
    out = parsed;
    return true;
}

Intent read_intent(const json& intent) {
    if (intent.is_null()) {
        return Intent::none;
    }
    if (!intent.is_object()) {
        throw json::type_error::create(302, "intent must be an object or null", &intent);
    }

    const auto name = intent.at("name").get<std::string>();
    const auto& params = intent.at("parameters");
    if (!params.is_object()) {
        throw json::type_error::create(302, "intent.parameters must be an object", &params);
    }

    if (name == "locomotion.stop") {
        return Intent::stop;
    }
    if (name != "locomotion.translate" && name != "locomotion.turn") {
        return Intent::unsupported;
    }

    const auto direction = params.at("direction").get<std::string>();
    if (name == "locomotion.translate") {
        if (direction == "forward") return Intent::forward;
        if (direction == "backward") return Intent::backward;
        if (direction == "left") return Intent::strafe_left;
        if (direction == "right") return Intent::strafe_right;
    } else {
        if (direction == "left") return Intent::turn_left;
        if (direction == "right") return Intent::turn_right;
    }
    return Intent::unsupported;
}

bool read_live_timing(const json& document, Window& window) {
    const auto timing = document.find("live_timing");
    if (timing == document.end()) {
        return false;
    }
    if (timing->is_null()) {
        return true;
    }
    if (!timing->is_object()) {
        return false;
    }

    const auto issued = timing->find("issued_at_unix_ms");
    const auto expiry = timing->find("expires_at_unix_ms");
    if (issued == timing->end() || expiry == timing->end()) {
        return false;
    }

    std::int64_t issued_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    if (!read_positive_int64(*issued, issued_at_ms) ||
        !read_positive_int64(*expiry, expires_at_ms)) {
        return false;
    }
    if (expires_at_ms <= issued_at_ms) {
        return false;
    }

    window.expires_at_ms = expires_at_ms;
    return true;
}

}  // namespace

Window decode_window(std::string_view line) {
    Window window;
    window.error = "invalid_json";
    if (line.find('\0') != std::string_view::npos) {
        return window;
    }

    json document;
    try {
        document = json::parse(line.begin(), line.end());
    } catch (const json::exception&) {
        return window;
    }
    if (!document.is_object()) {
        return window;
    }

    window.error = "invalid_event";
    try {
        // Extract event_id first so downstream audit logs preserve it even on field errors.
        if (const auto id = document.find("event_id"); id != document.end() && id->is_string()) {
            window.event_id = id->get<std::string>();
        }
        if (window.event_id.empty()) {
            return window;
        }

        const auto version = document.find("schema_version");
        if (version == document.end()) {
            return window;
        }
        if (!version->is_number_integer() || version->get<std::int64_t>() != 1) {
            window.error = "unsupported_schema";
            return window;
        }

        const auto& decision = document.at("decision");
        if (!decision.is_object()) {
            return window;
        }
        const auto status = decision.at("status").get<std::string>();
        if (status == "accepted") {
            window.accepted = true;
        } else if (status == "rejected") {
            window.accepted = false;
        } else {
            return window;
        }

        if (!document.contains("live_timing")) {
            return window;
        }
        if (!read_live_timing(document, window)) {
            window.error = "invalid_expiry";
            return window;
        }

        const auto& intent = document.at("intent");
        if (window.accepted) {
            window.intent = read_intent(intent);
        } else if (!intent.is_null()) {
            return window;
        }

        window.error = nullptr;
    } catch (const json::exception&) {
        // Keep the event ID if the invalid field followed it.
    }
    return window;
}

std::string format_log(const Command& command, bool applied) {
    return json{
        {"event_id", command.event_id.empty() ? json(nullptr) : json(command.event_id)},
        {"action", command.action},
        {"vx", command.velocity.vx},
        {"vy", command.velocity.vy},
        {"vyaw", command.velocity.vyaw},
        {"reason", command.reason},
        {"applied", applied},
    }.dump();
}

}  // namespace mea
