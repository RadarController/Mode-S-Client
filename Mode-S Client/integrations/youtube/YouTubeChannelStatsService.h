#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class AppState;
class YouTubeAuth;

namespace youtube {

class YouTubeChannelStatsService {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    YouTubeChannelStatsService() = default;
    ~YouTubeChannelStatsService();

    bool Start(YouTubeAuth& auth, AppState& state, LogFn log);
    void Stop();

    bool running() const { return running_.load(); }

private:
    void worker();

    YouTubeAuth* auth_ = nullptr;
    AppState* state_ = nullptr;
    LogFn log_;

    std::atomic<bool> running_{ false };
    std::thread thread_;
};

} // namespace youtube
