#include "chat/ChatAggregator.h"

#include <algorithm>

ChatAggregator::ChatAggregator(size_t capacity)
    : capacity_(capacity) {}

void ChatAggregator::Subscribe(std::function<void(const ChatMessage&)> cb)
{
    std::lock_guard<std::mutex> lock(mu_);
    on_add_ = std::move(cb);
}

void ChatAggregator::Add(ChatMessage msg)
{
    std::function<void(const ChatMessage&)> cb;
    ChatMessage notify;
    bool do_notify = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (capacity_ == 0) return;

        while (ring_.size() >= capacity_) {
            ring_.pop_front();
        }
        ring_.push_back(std::move(msg));
        cb = on_add_;
        if (cb) {
            notify = ring_.back();
            do_notify = true;
        }
    }

    // Invoke callback outside the lock.
    if (do_notify) {
        // NOTE: callback is invoked on the calling thread of Add(). Keep it fast.
        cb(notify);
    }
}

nlohmann::json ChatAggregator::RecentJson(size_t limit) const
{
    std::lock_guard<std::mutex> lock(mu_);

    const size_t n = std::min(limit, ring_.size());
    const size_t start = ring_.size() - n;

    nlohmann::json out = nlohmann::json::array();
    for (size_t i = start; i < ring_.size(); ++i) {
        const auto& m = ring_[i];
        out.push_back({
            {"platform", m.platform},
            {"user", m.user},
            {"message", m.message},
            {"ts_ms", m.ts_ms},
        });
    }
    return out;
}

void ChatAggregator::Clear()
{
    std::lock_guard<std::mutex> lock(mu_);
    ring_.clear();
}
