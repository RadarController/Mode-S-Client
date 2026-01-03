#include "AppState.h"

std::int64_t AppState::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::vector<ChatMessage> AppState::recent_chat() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return std::vector<ChatMessage>(chat_.begin(), chat_.end());
}

void AppState::set_tiktok_viewers(int v) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.tiktok_viewers = v;
}

void AppState::set_tiktok_followers(int f) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.tiktok_followers = f;
}


void AppState::set_twitch_viewers(int v) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.twitch_viewers = v;
}
void AppState::set_twitch_followers(int f) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.twitch_followers = f;
}
void AppState::set_twitch_live(bool live) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.twitch_live = live;
}

void AppState::set_youtube_viewers(int v) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.youtube_viewers = v;
}
void AppState::set_youtube_followers(int f) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.youtube_followers = f;
}
void AppState::set_youtube_live(bool live) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.youtube_live = live;
}
void AppState::set_tiktok_live(bool live) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.tiktok_live = live;
}
Metrics AppState::get_metrics() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return metrics_;
}

nlohmann::json AppState::metrics_json() const {
    auto m = get_metrics();
    return {
        {"ts_ms", m.ts_ms},
        {"twitch_viewers", m.twitch_viewers},
        {"youtube_viewers", m.youtube_viewers},
        {"tiktok_viewers", m.tiktok_viewers},
        {"twitch_followers", m.twitch_followers},
        {"youtube_followers", m.youtube_followers},
        {"tiktok_followers", m.tiktok_followers},
        {"twitch_live", m.twitch_live},
        {"youtube_live", m.youtube_live},
        {"tiktok_live", m.tiktok_live},
        {"total_viewers", m.total_viewers()},
        {"total_followers", m.total_followers()}
    };
}

nlohmann::json AppState::chat_json() const {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& c : recent_chat()) {
        arr.push_back({
            {"platform", c.platform},
            {"user", c.user},
            {"message", c.message},
            {"ts_ms", c.ts_ms}
            });
    }
    return arr;
}

void AppState::push_tiktok_event(const EventItem& ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    tiktok_events_.push_back(ev);
    const size_t kMax = 200;
    while (tiktok_events_.size() > kMax) tiktok_events_.pop_front();
}

nlohmann::json AppState::tiktok_events_json(int limit) const {
    using nlohmann::json;
    if (limit <= 0) limit = 200;

    std::lock_guard<std::mutex> lk(mtx_);
    json events = json::array();
    const int total = (int)tiktok_events_.size();
    const int start = (total > limit) ? (total - limit) : 0;

    for (int i = start; i < total; ++i) {
        const auto& e = tiktok_events_[(size_t)i];
        json o;
        o["platform"] = e.platform;
        o["type"] = e.type;
        o["user"] = e.user;
        o["message"] = e.message;
        o["ts_ms"] = e.ts_ms;
        events.push_back(std::move(o));
    }

    json out;
    out["count"] = (int)events.size();
    out["events"] = std::move(events);
    return out;
}
