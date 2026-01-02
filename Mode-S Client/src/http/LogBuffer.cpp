#include "LogBuffer.h"
#include <chrono>

static uint64_t NowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void LogBuffer::Push(const std::string& msg, uint64_t ts_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    q_.push_back(Entry{next_id_++, ts_ms, msg});
    while (q_.size() > capacity_) q_.pop_front();
}

void LogBuffer::PushNow(const std::string& msg) {
    Push(msg, NowMs());
}

std::vector<LogBuffer::Entry> LogBuffer::ReadSince(uint64_t since_id, size_t limit) const {
    std::vector<Entry> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& e : q_) {
        if (e.id > since_id) {
            out.push_back(e);
            if (out.size() >= limit) break;
        }
    }
    return out;
}

uint64_t LogBuffer::LatestId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return next_id_ ? (next_id_ - 1) : 0;
}
