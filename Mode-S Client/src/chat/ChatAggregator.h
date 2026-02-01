#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "json.hpp"

// Reuse the project's canonical ChatMessage definition.
#include "AppState.h"

// Thread-safe aggregator / ring buffer for combined live chat.
// Platform adapters call Add(...). UI / overlay can query RecentJson().
class ChatAggregator
{
public:
    explicit ChatAggregator(size_t capacity = 200);

    // Subscribe to newly added messages.
    // Callback will be invoked on the calling thread of Add(). Keep it fast.
    void Subscribe(std::function<void(const ChatMessage&)> cb);

    // Adds a normalized message. Safe to call from any thread.
    void Add(ChatMessage msg);

    // Returns most recent messages (oldest->newest) as a JSON array.
    // Safe to call from any thread.
    nlohmann::json RecentJson(size_t limit = 100) const;

    // Current number of buffered messages.
    size_t Size() const;

    void Clear();

    bool is_event = false;

private:
    size_t capacity_;
    mutable std::mutex mu_;
    std::deque<ChatMessage> ring_;
    std::function<void(const ChatMessage&)> on_add_;
};
