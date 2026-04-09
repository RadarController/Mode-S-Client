#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class AppState;

namespace youtube {

class YouTubeLiveStatusService {
public:
    using HandleFn = std::function<std::string()>;
    using LogFn = std::function<void(const std::wstring&)>;

    YouTubeLiveStatusService();
    ~YouTubeLiveStatusService();

    bool Start(HandleFn getHandle, AppState& state, LogFn log);
    void Stop();

    bool running() const { return running_.load(); }

private:
    void worker(HandleFn getHandle, AppState* state, LogFn log);

    std::atomic<bool> running_{ false };
    std::thread thread_;
};

} // namespace youtube