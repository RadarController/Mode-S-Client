#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "json.hpp"

class AppState;

namespace fenixsim {

class FenixSimFailuresClient;

class FenixFailureCoordinator {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    FenixFailureCoordinator() = default;
    ~FenixFailureCoordinator();

    void Start(AppState& state, FenixSimFailuresClient& client, LogFn log);
    void Stop();

    bool running() const { return running_.load(); }

private:
    void WorkerLoop();

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
                           std::string& detail);

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
    std::size_t next_failure_index_ = 0;
    std::int64_t last_no_trigger_log_ms_ = 0;

    std::vector<std::string> allowed_failures_;
};

} // namespace fenixsim
