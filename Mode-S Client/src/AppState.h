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

    Metrics get_metrics() const;
    nlohmann::json metrics_json() const;
    nlohmann::json chat_json() const;

private:
    static std::int64_t now_ms();

    mutable std::mutex mtx_;
    Metrics metrics_{};
    std::deque<ChatMessage> chat_; // last 200
};
