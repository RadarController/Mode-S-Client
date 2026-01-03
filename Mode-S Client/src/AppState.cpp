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

// --- Twitch EventSub diagnostics ---
void AppState::set_twitch_eventsub_status(const nlohmann::json& status) {
    std::lock_guard<std::mutex> lk(mtx_);
    twitch_eventsub_status_ = status;
}

nlohmann::json AppState::twitch_eventsub_status_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return twitch_eventsub_status_;
}

void AppState::add_twitch_eventsub_event(const nlohmann::json& ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    twitch_eventsub_events_.push_back(ev);
    // Keep small
    while (twitch_eventsub_events_.size() > 200) twitch_eventsub_events_.pop_front();
}

nlohmann::json AppState::twitch_eventsub_events_json(int limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    limit = std::max(1, std::min(limit, 1000));

    nlohmann::json out;
    out["count"] = (int)twitch_eventsub_events_.size();
    nlohmann::json arr = nlohmann::json::array();

    int start = (int)twitch_eventsub_events_.size() - limit;
    if (start < 0) start = 0;
    for (int i = start; i < (int)twitch_eventsub_events_.size(); ++i) {
        arr.push_back(twitch_eventsub_events_[i]);
    }
    out["events"] = std::move(arr);
    return out;
}

void AppState::clear_twitch_eventsub_events() {
    std::lock_guard<std::mutex> lk(mtx_);
    twitch_eventsub_events_.clear();
}

void AppState::push_tiktok_event(const EventItem& e) {
    std::lock_guard<std::mutex> lk(mtx_);
    tiktok_events_.push_back(e);
    while (tiktok_events_.size() > 200) tiktok_events_.pop_front();
}

nlohmann::json AppState::tiktok_events_json(size_t limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json out;
    out["count"] = (int)tiktok_events_.size();
    nlohmann::json arr = nlohmann::json::array();

    size_t n = tiktok_events_.size();
    size_t start = 0;
    if (limit > 0 && n > limit) start = n - limit;

    for (size_t i = start; i < n; ++i) {
        const auto& e = tiktok_events_[i];
        nlohmann::json j;
        j["platform"] = e.platform;
        j["type"] = e.type;
        j["user"] = e.user;
        j["message"] = e.message;
        j["ts_ms"] = e.ts_ms;
        arr.push_back(std::move(j));
    }

    out["events"] = std::move(arr);
    return out;
}
