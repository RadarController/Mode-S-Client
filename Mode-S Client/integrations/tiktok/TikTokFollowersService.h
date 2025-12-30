#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>

#include <windows.h>

struct AppConfig;
class AppState;

struct TikTokFollowersUiCallbacks {
    std::function<void(const std::wstring&)> log;
    std::function<void(const std::wstring&)> set_status;
    std::function<void(int)> set_followers;
};

// Starts a poller thread that periodically fetches TikTok follower count for config.tiktok_unique_id.
// - Writes follower count into AppState (for /api/metrics).
// - Optionally updates UI via callbacks and posts refresh_msg to hwnd.
std::thread StartTikTokFollowersPoller(
    HWND hwnd,
    AppConfig& config,
    AppState& state,
    std::atomic<bool>& running,
    UINT refresh_msg,
    TikTokFollowersUiCallbacks cb);
