#pragma once
#include <mutex>
#include <deque>
#include <string>
#include <cstdint>
#include <chrono>
#include <vector>
#include <unordered_map>
#include "json.hpp"

struct ChatMessage {
    std::string platform;
    std::string user;
    std::string message;

    // Optional rich message representation (e.g. YouTube "runs" containing emoji thumbnails).
    // When present, overlays can render emojis as images while retaining `message` as a plain-text fallback.
    nlohmann::json runs; // null or array

    std::string color; // optional username color (e.g. "#FF0000")
    std::int64_t ts_ms{};
};

// JSON serialization for ChatMessage (used by /api/chat and overlays).
// Backward-compatible: `runs` is omitted unless it's a non-empty array.
inline void to_json(nlohmann::json& j, const ChatMessage& c) {
    j = nlohmann::json{
        {"platform", c.platform},
        {"user", c.user},
        {"message", c.message},
        {"ts_ms", c.ts_ms}
    };
    if (!c.color.empty()) j["color"] = c.color;
    if (c.runs.is_array() && !c.runs.empty()) j["runs"] = c.runs;
}

struct EventItem {
    std::string platform; // "tiktok"
    std::string type;     // e.g. "like", "gift", "share", "follow"
    std::string user;
    std::string message;
    std::int64_t ts_ms{};
};

struct Metrics {
    std::int64_t ts_ms{};
    int twitch_viewers{};
    int youtube_viewers{};
    int tiktok_viewers{};
    int twitch_followers{};
    int youtube_followers{};
    int tiktok_followers{};
    bool twitch_live{};
    bool youtube_live{};
    bool tiktok_live{};

    int total_viewers() const { return twitch_viewers + youtube_viewers + tiktok_viewers; }
    int total_followers() const { return twitch_followers + youtube_followers + tiktok_followers; }
};

class AppState {
public:
    void push_log_utf8(const std::string& msg);
    nlohmann::json log_json(std::uint64_t since = 0, int limit = 200) const;

    std::vector<ChatMessage> recent_chat() const;

    void set_tiktok_viewers(int v);
    void set_tiktok_followers(int f);
    void set_twitch_viewers(int v);
    void set_twitch_followers(int f);
    void set_twitch_live(bool live);
    void set_youtube_viewers(int v);
    void set_youtube_followers(int f);
    void set_youtube_live(bool live);
    void set_tiktok_live(bool live);
    void push_tiktok_event(const EventItem& e);

    Metrics get_metrics() const;
    nlohmann::json metrics_json() const;
    nlohmann::json chat_json() const;
    nlohmann::json tiktok_events_json(size_t limit = 200) const;

    // --- Twitch EventSub diagnostics (separate from chat) ---
    void set_twitch_eventsub_status(const nlohmann::json& status);
    nlohmann::json twitch_eventsub_status_json() const;

    void add_twitch_eventsub_event(const nlohmann::json& ev);
    nlohmann::json twitch_eventsub_events_json(int limit = 200) const;
    void clear_twitch_eventsub_events();

    void push_youtube_event(const EventItem& e);
    nlohmann::json youtube_events_json(size_t limit = 200) const;

    // --- Simple chatbot command store (configurable via HTTP + app UI) ---
    // Commands are stored without the leading '!'. Matching is case-insensitive.
    // Persistence: call set_bot_commands_storage_path() once at startup to enable
    // saving/loading between sessions.
    void set_bot_commands_storage_path(const std::string& path_utf8);
    bool load_bot_commands_from_disk();
    void set_bot_commands(const nlohmann::json& commands);
    nlohmann::json bot_commands_json() const;

    // Convenience: get a response for a command. Returns empty string if not found/disabled.
    std::string bot_lookup_response(const std::string& command_lc) const;

    // Checks enabled/scope/cooldown; if allowed returns the response template and updates
    // the command cooldown timer. Returns empty string otherwise.
    // Notes:
    // - 'is_broadcaster' is treated as having mod permissions.
    // - Cooldowns are per-command (global), not per-user.
    std::string bot_try_get_response(
        const std::string& command_lc,
        bool is_mod,
        bool is_broadcaster,
        std::int64_t now_ms);

    // Preview-only: checks enabled/scope/cooldown but does NOT update the cooldown timer.
    // Useful for test UI endpoints so previews don't consume cooldowns.
    std::string bot_peek_response(
        const std::string& command_lc,
        bool is_mod,
        bool is_broadcaster,
        std::int64_t now_ms) const;

private:
    static std::int64_t now_ms();

    mutable std::mutex mtx_;
    Metrics metrics_{};
    std::deque<ChatMessage> chat_; // last 200
    std::deque<EventItem> tiktok_events_; // last 200
    std::deque<EventItem> youtube_events_; // last 200
    std::deque<nlohmann::json> twitch_eventsub_events_; // last 200 by default

    struct BotCmd {
        std::string response;
        bool enabled = true;
        int cooldown_ms = 3000;
        // all | mods | broadcaster
        std::string scope = "all";

        // runtime only
        std::int64_t last_fire_ms = 0;
    };
    std::unordered_map<std::string, BotCmd> bot_cmds_;
    std::string bot_commands_path_utf8_; // empty => no persistence
    std::string bot_commands_path_;
    void save_bot_commands_to_disk_locked() const;

    // EventSub status + events kept small for UI/debugging.
    nlohmann::json twitch_eventsub_status_ = nlohmann::json{
        {"ws_state", "stopped"},
        {"connected", false},
        {"session_id", ""},
        {"subscribed", false},
        {"last_ws_message_ms", 0},
        {"last_keepalive_ms", 0},
        {"last_helix_ok_ms", 0},
        {"last_error", ""},
        {"subscriptions", nlohmann::json::array()}
    };

    struct LogEntry {
        std::uint64_t id{};
        std::int64_t ts_ms{};
        std::string msg;
    };

    std::deque<LogEntry> log_;          // ring buffer
    std::uint64_t log_next_id_ = 0;     // monotonically increasing
};
