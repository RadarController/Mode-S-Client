#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

class AppState;
struct AppConfig;

class TwitchHelixService {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    TwitchHelixService() = default;
    ~TwitchHelixService() { Stop(); }

    void Start(HWND notifyHwnd, AppState* state, AppConfig* config, LogFn log);
    void Stop();
    void OnConfigChanged(); // force refresh

private:
    void ThreadMain();

    std::wstring TrimBodyForLog(const std::string& body, size_t maxBytes = 600);

    HWND hwnd_ = nullptr;
    AppState* state_ = nullptr;
    AppConfig* config_ = nullptr;
    LogFn log_;

    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> refresh_{false};

    std::mutex cache_mtx_;
    std::string token_;
    long long token_expiry_ms_ = 0;
    std::string user_id_;
};
