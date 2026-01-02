#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

// Thread-safe in-memory log buffer for the Web UI.
// Stores UTF-8 messages with monotonically increasing ids.
class LogBuffer {
public:
    struct Entry {
        uint64_t id;
        uint64_t ts_ms;
        std::string msg; // UTF-8
    };

    explicit LogBuffer(size_t capacity = 1000) : capacity_(capacity) {}

    // Add a UTF-8 message.
    void Push(const std::string& msg, uint64_t ts_ms);

    // Convenience: add with current time.
    void PushNow(const std::string& msg);

    // Read entries with id > since_id, up to limit.
    std::vector<Entry> ReadSince(uint64_t since_id, size_t limit) const;

    uint64_t LatestId() const;

private:
    size_t capacity_;
    mutable std::mutex mu_;
    std::deque<Entry> q_;
    uint64_t next_id_ = 1;
};
