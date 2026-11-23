#include "controller.hpp"

#include <limits>

#include <nlohmann/json.hpp>

namespace mea {
namespace {

using nlohmann::json;

Intent read_intent(const json& intent) {
    if (intent.is_null()) {
        return Intent::none;
    }

    const auto type = intent.at("type").get<std::string>();
    if (type == "locomotion.stop") {
        return Intent::stop;
    }
    if (type != "locomotion.translate" && type != "locomotion.turn") {
        return Intent::unsupported;
    }

    const auto& params = intent.at("params");
    const auto direction = params.at("direction").get<std::string>();
    if (type == "locomotion.translate") {
        if (direction == "forward") return Intent::forward;
        if (direction == "backward") return Intent::backward;
    } else {
        if (direction == "left") return Intent::left;
        if (direction == "right") return Intent::right;
    }
    return Intent::unsupported;
}

bool read_expiry(const json& document, Window& window) {
    const auto timing = document.find("live_timing");
    if (timing == document.end() || timing->is_null()) {
        return true;
    }

    const auto expiry = timing->find("expires_at_unix_ms");
    if (expiry == timing->end() || !expiry->is_number_integer()) {
        return false;
    }
    constexpr auto max_expiry = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (expiry->is_number_unsigned() && expiry->get<std::uint64_t>() > max_expiry) {
        return false;
    }
    const auto expires_at_ms = expiry->get<std::int64_t>();
    if (expires_at_ms <= 0) {
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

    window.error = "invalid_event";
    try {
        window.event_id = document.at("event_id").get<std::string>();
        if (window.event_id.empty()) {
            return window;
        }
        if (document.at("schema_version").get<std::string>() != "mock-v1") {
            window.error = "unsupported_schema";
            return window;
        }
        window.accepted = document.at("decision").at("accepted").get<bool>();
        if (!read_expiry(document, window)) {
            window.error = "invalid_expiry";
            return window;
        }
        if (window.accepted) {
            window.intent = read_intent(document.at("intent"));
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
