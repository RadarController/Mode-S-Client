#pragma once
#include <mutex>
#include <deque>
#include <string>
#include <cstdint>
#include <chrono>
#include <vector>
#include "json.hpp"

struct ChatMessage {
    std::string platform;
    std::string user;
    std::string message;
    std::string color; // optional username color (e.g. "#FF0000")
    std::int64_t ts_ms{};
};

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

private:
    static std::int64_t now_ms();

    mutable std::mutex mtx_;
    Metrics metrics_{};
    std::deque<ChatMessage> chat_; // last 200
    std::deque<EventItem> tiktok_events_; // last 200

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

    std::deque<nlohmann::json> twitch_eventsub_events_; // last 200 by default
};
