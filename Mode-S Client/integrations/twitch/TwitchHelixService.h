#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>

#include <windows.h>

struct AppConfig;
class AppState;

#include <vector>

struct TwitchHelixUiCallbacks {
    // Optional: if a callback is empty, it will be skipped.
    std::function<void(const std::wstring&)> log;
    std::function<void(const std::wstring&)> set_status;
    std::function<void(bool)> set_live;
    std::function<void(int)> set_viewers;
    std::function<void(int)> set_followers;
};



struct TwitchCategory {
    std::string id;
    std::string name;
};

// Search Twitch categories (games) for typeahead/autocomplete.
// Returns true on success; on failure, returns false and (optionally) fills out_error.
bool TwitchHelixSearchCategories(
    AppConfig& config,
    const std::string& query,
    std::vector<TwitchCategory>& out,
    std::string* out_error);

// Starts the Twitch Helix poller thread.
// - Reads config fields each loop (so Save changes apply without restarting).
// - If AppConfig fields are empty (e.g., JSON key mapping mismatch), the poller will also try reading config.json directly.
// - Writes viewer/follower/live metrics into AppState (for /api/metrics).
// - Also pushes values into the provided UI callbacks for your UI labels.
std::thread StartTwitchHelixPoller(
    HWND hwnd,
    AppConfig& config,
    AppState& state,
    std::atomic<bool>& running,
    UINT refresh_msg,
    TwitchHelixUiCallbacks cb);
