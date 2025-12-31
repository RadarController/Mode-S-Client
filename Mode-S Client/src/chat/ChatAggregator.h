#pragma once

#include <deque>
#include <mutex>
#include <vector>

#include "json.hpp"

// Reuse the existing ChatMessage type already defined in AppState.h
// so we don't create two competing definitions.
#include "AppState.h"

// Thread-safe aggregator / ring buffer for combined live chat.
// Platform adapters call Add(...). UI / overlay can query RecentJson().
class ChatAggregator
{
public:
    explicit ChatAggregator(size_t capacity = 200);

    // Adds a normalized message. Safe to call from any thread.
    void Add(ChatMessage msg);

    // Returns most recent messages (oldest->newest) as a JSON array.
    // Safe to call from any thread.
    nlohmann::json RecentJson(size_t limit = 100) const;

    void Clear();

private:
    size_t capacity_;
    mutable std::mutex mu_;
    std::deque<ChatMessage> ring_;
};
