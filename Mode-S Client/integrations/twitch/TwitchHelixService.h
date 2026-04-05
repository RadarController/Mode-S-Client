#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>

#include <windows.h>

struct AppConfig;
class AppState;

#include <vector>
#include "json.hpp"

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

// Update Twitch channel title and category (game_id) via Helix.
// Returns true on success; on failure returns false and (optionally) fills out_error.
bool TwitchHelixUpdateChannelInfo(
    AppConfig& config,
    const std::string& title,
    const std::string& game_id,
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


// --- Twitch Channel Points custom rewards ---
// Returns the raw Helix response body (usually {"data": [...]}).
bool TwitchHelixGetCustomRewards(
    AppConfig& config,
    bool only_manageable_rewards,
    nlohmann::json* out,
    std::string* out_error);

// Creates a custom reward from the provided JSON body fields.
// Expected fields mirror Twitch Helix where practical, e.g.
// title, cost, prompt, is_enabled, is_user_input_required, background_color,
// is_max_per_stream_enabled, max_per_stream,
// is_max_per_user_per_stream_enabled, max_per_user_per_stream,
// is_global_cooldown_enabled, global_cooldown_seconds,
// should_redemptions_skip_request_queue.
bool TwitchHelixCreateCustomReward(
    AppConfig& config,
    const nlohmann::json& reward_body,
    nlohmann::json* out,
    std::string* out_error);

// Updates an existing custom reward. reward_id is required.
bool TwitchHelixUpdateCustomReward(
    AppConfig& config,
    const std::string& reward_id,
    const nlohmann::json& reward_body,
    nlohmann::json* out,
    std::string* out_error);

// Deletes an existing custom reward.
bool TwitchHelixDeleteCustomReward(
    AppConfig& config,
    const std::string& reward_id,
    std::string* out_error);

// Returns the raw Helix response body for reward redemptions.
bool TwitchHelixGetCustomRewardRedemptions(
    AppConfig& config,
    const std::string& reward_id,
    const std::string& status,
    int first,
    nlohmann::json* out,
    std::string* out_error);

// Updates a redemption status to FULFILLED or CANCELED.
bool TwitchHelixUpdateRedemptionStatus(
    AppConfig& config,
    const std::string& reward_id,
    const std::string& redemption_id,
    const std::string& status,
    nlohmann::json* out,
    std::string* out_error);
