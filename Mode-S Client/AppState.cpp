#include "AppState.h"

std::int64_t AppState::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void AppState::add_chat(ChatMessage msg) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (msg.ts_ms == 0) msg.ts_ms = now_ms();
    chat_.push_back(std::move(msg));
    while (chat_.size() > 200) chat_.pop_front();
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
