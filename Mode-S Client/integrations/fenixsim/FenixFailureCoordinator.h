#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "json.hpp"

#include "fenixsim/FenixFailureMetadataStore.h"

class AppState;

namespace fenixsim {

class FenixSimFailuresClient;
struct ArmedFailureCondition;

class FenixFailureCoordinator {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    FenixFailureCoordinator() = default;
    ~FenixFailureCoordinator();

    void Start(AppState& state, FenixSimFailuresClient& client, LogFn log);
    void Stop();

    bool running() const { return running_.load(); }

    // Light-touch runtime control/state for the home page simulator automation panel.
    void SetEnabled(bool enabled);
    bool enabled() const;
    void PanicStop();
    nlohmann::json StatusJson() const;

private:
    void WorkerLoop();
    void RefreshFailureMetadataOnStart();

    void SeedSeenFromCurrentQueues();
    void CollectNewEvents(std::vector<nlohmann::json>& out_events);
    void CollectNewEventsFromArray(const char* platform,
                                   const nlohmann::json& events,
                                   std::vector<nlohmann::json>& out_events);

    std::string MakeEventKey(const char* platform, const nlohmann::json& event) const;
    bool RememberEventKey(const std::string& key);

    int CreditsFromEvent(const nlohmann::json& event);
    int CreditsFromTwitchEvent(const nlohmann::json& event);
    int CreditsFromTikTokEvent(const nlohmann::json& event);
    int CreditsFromYouTubeEvent(const nlohmann::json& event);

    int TierToCredits(const std::string& tier) const;

    bool SpendOnePendingCredit();
    bool TriggerOneFailure(std::string& triggered_id,
                           std::string& triggered_title,
                           std::string& action_desc,
                           std::string& detail);
    bool IsFailureOnRecentCooldown(const std::string& failure_id,
                                   std::int64_t now_ms,
                                   std::int64_t* remaining_ms = nullptr) const;
    void RememberTriggeredFailure(const std::string& failure_id,
                                  std::int64_t now_ms);
    void PruneRecentFailureCooldowns(std::int64_t now_ms);

    bool ShouldArmRandomly();
    int RandomIntInclusive(int min_value, int max_value);
    ArmedFailureCondition MakeRandomArmedCondition();
    std::string DescribeArmedCondition(const ArmedFailureCondition& condition) const;

    void Log(const std::wstring& msg) const;

    static std::wstring ToW(const std::string& s);
    static std::string ToLower(std::string s);
    static int JsonIntLoose(const nlohmann::json& obj, const char* key, int fallback = 0);
    static std::int64_t NowMs();

private:
    AppState* state_ = nullptr;
    FenixSimFailuresClient* client_ = nullptr;
    LogFn log_;

    std::thread worker_;
    std::atomic<bool> running_{ false };

    mutable std::mutex mu_;
    std::unordered_set<std::string> seen_keys_;
    std::deque<std::string> seen_order_;
    static constexpr std::size_t kSeenMax_ = 4096;

    int pending_credits_ = 0;
    int twitch_bits_remainder_ = 0;
    int tiktok_gift_remainder_ = 0;
    std::int64_t last_no_trigger_log_ms_ = 0;
    bool automation_enabled_ = true;

    // Runtime-only recent-use cooldown. Set to 0 to disable.
    static constexpr std::int64_t kRecentFailureCooldownMs_ = 15LL * 60LL * 1000LL;
    // First-pass armed/immediate split.
    static constexpr int kArmFailureChancePercent_ = 40;
    std::unordered_map<std::string, std::int64_t> recent_failure_last_used_ms_;

    std::mt19937 rng_{ std::random_device{}() };

    FenixFailureMetadataStore metadata_store_;
    std::vector<MergedFailureCatalogEntry> merged_catalog_;
    std::size_t discovered_failure_count_ = 0;
    std::size_t metadata_entry_count_ = 0;
    std::size_t stale_metadata_entry_count_ = 0;
};

} // namespace fenixsim
