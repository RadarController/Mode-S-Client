#pragma once
#include <cstddef>
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

    // Optional role flags (populated when available by platform adapters).
    // Used by the chatbot scope rules (all/mods/broadcaster).
    bool is_mod = false;
    bool is_broadcaster = false;
    bool is_event = false;
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

    // Keep backward-compatibility: only include when true.
    if (c.is_mod) j["is_mod"] = true;
    if (c.is_broadcaster) j["is_broadcaster"] = true;
    if (c.is_event) j["is_event"] = true;
}

struct EventItem {
    std::string platform; // "tiktok"
    std::string type;     // e.g. "like", "gift", "share", "follow"
    std::string user;
    std::string message;
    std::int64_t ts_ms{};
};

struct ErrorEntry {
        std::uint64_t id{};
        std::int64_t ts_ms{};
        std::string msg;
    };


struct Metrics {
    std::int64_t ts_ms{};
    int twitch_viewers{};
    int youtube_viewers{};
    int tiktok_viewers{};
    int twitch_followers{};
    int twitch_subscribers{};
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
    void set_twitch_subscribers(int c);
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

    // --- Homepage runtime state (requested start/stop actions from the control surface) ---
    void set_platform_runtime_state(const std::string& platform, const std::string& requested_state);
    nlohmann::json platform_runtime_state_json() const;

    // --- Twitch EventSub diagnostics (separate from chat) ---
    void set_twitch_eventsub_status(const nlohmann::json& status);
    nlohmann::json twitch_eventsub_status_json() const;

    void add_twitch_eventsub_event(const nlohmann::json& ev);
    void request_twitch_subscriber_refresh();
    bool consume_twitch_subscriber_refresh_requested();
    nlohmann::json twitch_eventsub_events_json(int limit = 200) const;
    void clear_twitch_eventsub_events();

    // Ring buffer of recent EventSub errors/warnings (for quick diagnosis).
    void push_twitch_eventsub_error(const std::string& msg);
    nlohmann::json twitch_eventsub_errors_json(int limit = 50) const;

    void push_youtube_event(const EventItem& e);
    nlohmann::json youtube_events_json(size_t limit = 200) const;

    // --- Unified alerts history (across Twitch/TikTok/YouTube) ---
    // Returns a rolling in-memory history for "missed alerts" tooling.
    // Optional platform filter: "twitch" | "tiktok" | "youtube"
    nlohmann::json alerts_history_json(int limit = 200, const std::string& platform = "") const;

    // Re-inject a stored history item back into the live platform queues (with a fresh id/ts)
    // so overlays/chat can replay missed alerts.
    // Returns true on success; on failure, `err` (if provided) is filled.
    bool resend_alert_history(const std::string& history_id, nlohmann::json* replayed = nullptr, std::string* err = nullptr);

    // --- Bot commands (chatbot) ---
    // Storage path should be set once at startup (utf-8 path). If empty, commands are in-memory only.
    void set_bot_commands_storage_path(const std::string& path_utf8);
    // Returns true if commands were loaded.
    bool load_bot_commands_from_disk();

    // Upsert a single command object. Accepts the same fields as set_bot_commands():
    // {"command":"help","response":"...","enabled":true,"cooldown_ms":3000,"scope":"all"}
    // Returns true on success; on failure returns false and (optionally) sets err.
    bool bot_upsert_command(const nlohmann::json& command_obj, std::string* err = nullptr);

    // Delete a single command by name (case-insensitive, with or without leading '!').
    // Returns true if a command was removed.
    bool bot_delete_command(const std::string& command);

    // Replace current command list using a JSON array of objects:
    // {"command":"help","response":"...","enabled":true,"cooldown_ms":3000,"scope":"all"}
    void set_bot_commands(const nlohmann::json& commands);
    nlohmann::json bot_commands_json() const;

    // Legacy: lookup response without consuming cooldown.
    std::string bot_lookup_response(const std::string& command_lc) const;

    // Enforces enabled/cooldown/scope. Returns empty if blocked/no match.
    std::string bot_try_get_response(
        const std::string& command_lc,
        bool is_mod,
        bool is_broadcaster,
        std::int64_t now_ms);

    // Like bot_try_get_response, but does NOT consume cooldown (no last_fire_ms update).
    std::string bot_peek_response(
        const std::string& command_lc,
        bool is_mod,
        bool is_broadcaster,
        std::int64_t now_ms) const;

    // --- Bot safety settings (chatbot) ---
    struct BotSettings {
        // Extra throttles on top of per-command cooldowns.
        std::int64_t per_user_gap_ms = 3000;      // one bot reply per user per 3s
        std::int64_t per_platform_gap_ms = 1000;  // one bot reply per platform per 1s

        // Conservative safety clamp for platform message length.
        std::size_t max_reply_len = 400;

        // If true, bot will not emit replies (commands still match/preview via API).
        bool silent_mode = false;
    };

    // Storage path should be set once at startup (utf-8 path). If empty, settings are in-memory only.
    void set_bot_settings_storage_path(const std::string& path_utf8);
    // Returns true if settings were loaded.
    bool load_bot_settings_from_disk();

    // Replace settings (and persist best-effort).
    // Accepts JSON object: {"per_user_gap_ms":3000,"per_platform_gap_ms":1000,"max_reply_len":400,"silent_mode":false}
    bool set_bot_settings(const nlohmann::json& settings_obj, std::string* err = nullptr);
    nlohmann::json bot_settings_json() const;
    BotSettings bot_settings_snapshot() const;


    // --- Overlay header (stream title/subtitle shown in overlays) ---
    struct OverlayHeader {
        std::string title;
        std::string subtitle;
    };

    // Storage path should be set once at startup (utf-8 path). If empty, header is in-memory only.
    void set_overlay_header_storage_path(const std::string& path_utf8);
    bool load_overlay_header_from_disk();

    // Replace header (and persist best-effort).
    // Accepts JSON object: {"title":"...","subtitle":"..."}
    bool set_overlay_header(const nlohmann::json& header_obj, std::string* err = nullptr);
    nlohmann::json overlay_header_json() const;
    OverlayHeader overlay_header_snapshot() const;

    // -----------------------------------------------------------------
    // Twitch Stream Info draft (used by /app/twitch_stream.html)
    // Persists to config.json under key: twitch_streaminfo
    // -----------------------------------------------------------------
    struct TwitchStreamDraft {
        std::string title;
        std::string category_name; // display text
        std::string category_id;   // Twitch "game_id" for Helix updates
        std::string description;   // stored for YouTube phase 2
    };

    void set_twitch_stream_draft(const TwitchStreamDraft& d);
    TwitchStreamDraft twitch_stream_draft_snapshot();
    nlohmann::json twitch_stream_draft_json();

    // Twitch reward action metadata (local app-side behaviour only).
    nlohmann::json twitch_reward_actions_json();
    nlohmann::json twitch_reward_action_json(const std::string& reward_id);
    bool set_twitch_reward_action(const std::string& reward_id, const nlohmann::json& action_obj, std::string* err = nullptr);
    bool delete_twitch_reward_action(const std::string& reward_id);

    // Twitch Channel Points runtime queues (separate from generic EventSub diagnostics).
    nlohmann::json twitch_channel_points_live_json(int limit = 200) const;
    nlohmann::json twitch_channel_points_pending_json(int limit = 200) const;
    nlohmann::json twitch_channel_points_history_json(int limit = 200) const;
    bool release_twitch_channel_points_pending(const std::string& redemption_id, nlohmann::json* released = nullptr, std::string* err = nullptr);

private:
    void load_twitch_stream_draft_from_config_unlocked();
    void save_twitch_stream_draft_to_config_unlocked();
    void load_twitch_reward_actions_from_config_unlocked();
    void save_twitch_reward_actions_to_config_unlocked();
    void route_twitch_channel_points_event_unlocked(const nlohmann::json& ev);
    void push_twitch_channel_points_live_unlocked(const nlohmann::json& ev);
    void upsert_twitch_channel_points_pending_unlocked(const nlohmann::json& ev);
    bool remove_twitch_channel_points_pending_unlocked(const std::string& redemption_id, nlohmann::json* removed = nullptr);

    void load_metrics_cache_from_config_unlocked();
    void save_metrics_cache_to_config_unlocked();

    static std::int64_t now_ms();

    struct AlertHistoryItem {
        std::string history_id;   // stable id for history lookup (not the original platform id)
        nlohmann::json payload;   // original payload (normalized to include platform/type/user/message/ts_ms where possible)
        std::string platform;     // cached for filtering
        std::int64_t ts_ms{};     // cached for sorting/display
    };

    void record_alert_history_(const nlohmann::json& payload);

    static std::string make_alert_history_id_(std::uint64_t seq);

    static constexpr std::size_t kAlertsHistoryMax_ = 5000;
    mutable std::mutex alerts_history_mu_;
    std::deque<AlertHistoryItem> alerts_history_;
    std::uint64_t alerts_history_seq_ = 0;


    mutable std::mutex mtx_;
    // Twitch stream info draft (loaded lazily from config.json)
    bool twitch_stream_draft_loaded_ = false;
    TwitchStreamDraft twitch_stream_draft_{};

    // Twitch reward action metadata (loaded lazily from config.json)
    bool twitch_reward_actions_loaded_ = false;
    nlohmann::json twitch_reward_actions_ = nlohmann::json::object();

    std::deque<nlohmann::json> twitch_channel_points_live_;
    std::deque<nlohmann::json> twitch_channel_points_pending_;
    std::deque<nlohmann::json> twitch_channel_points_history_;
    static constexpr std::size_t kTwitchChannelPointsQueueMax_ = 500;

    // Metrics cache (loaded lazily from config.json under key: metrics_cache)
    bool metrics_cache_loaded_ = false;
    std::int64_t last_metrics_cache_save_ms_ = 0;
    bool twitch_subscriber_refresh_requested_ = false;

    Metrics metrics_{};
    nlohmann::json platform_runtime_state_ = nlohmann::json{
        {"tiktok", {{"requested_state", "stopped"}, {"ts_ms", 0}}},
        {"twitch", {{"requested_state", "stopped"}, {"ts_ms", 0}}},
        {"youtube", {{"requested_state", "stopped"}, {"ts_ms", 0}}}
    };
    std::deque<ChatMessage> chat_; // last 200
    std::deque<EventItem> tiktok_events_; // last 200
    std::deque<EventItem> youtube_events_; // last 200
    std::deque<nlohmann::json> twitch_eventsub_events_; // last 200 by default
    std::deque<ErrorEntry> twitch_eventsub_errors_; // last 200 (most recent)

    // --- Bot commands ---
    struct BotCmd {
        std::string response;
        bool enabled = true;
        int cooldown_ms = 3000;
        std::string scope = "all"; // all | mods | broadcaster
        std::int64_t last_fire_ms = 0;
    };

    std::unordered_map<std::string, BotCmd> bot_cmds_; // key is lowercase command (no '!')
    std::string bot_commands_path_utf8_;
    // --- Bot safety settings ---
    BotSettings bot_settings_{};
    std::string bot_settings_path_utf8_;


// --- Overlay header ---
OverlayHeader overlay_header_{};
std::string overlay_header_path_utf8_;

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