#include "fenixsim/FenixFailureCoordinator.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include <windows.h>

#include "AppState.h"
#include "fenixsim/FenixSimFailures.h"
#include "fenixsim/FenixFailureMetadataStore.h"

namespace fenixsim {
namespace {

std::wstring SafeToW(const std::string& s) {
    if (s.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::string SafeToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

std::string SafeWriteResultToString(SafeWriteResult result) {
    switch (result) {
    case SafeWriteResult::Success: return "success";
    case SafeWriteResult::SkippedAlreadyActive: return "skipped_already_active";
    case SafeWriteResult::SkippedAlreadyArmed: return "skipped_already_armed";
    case SafeWriteResult::NotFound: return "not_found";
    case SafeWriteResult::VerificationFailed: return "verification_failed";
    case SafeWriteResult::TransportError: return "transport_error";
    case SafeWriteResult::InvalidResponse: return "invalid_response";
    default: return "unknown";
    }
}

} // namespace

FenixFailureCoordinator::~FenixFailureCoordinator() {
    Stop();
}

void FenixFailureCoordinator::Start(AppState& state, FenixSimFailuresClient& client, LogFn log) {
    Stop();

    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = &state;
        client_ = &client;
        log_ = std::move(log);
        seen_keys_.clear();
        seen_order_.clear();
        pending_credits_ = 0;
        twitch_bits_remainder_ = 0;
        tiktok_gift_remainder_ = 0;
        last_no_trigger_log_ms_ = 0;
        automation_enabled_ = false;
        recent_failure_last_used_ms_.clear();
        triggered_failure_counts_session_.clear();
        merged_catalog_.clear();
        discovered_failure_count_ = 0;
        metadata_entry_count_ = 0;
        stale_metadata_entry_count_ = 0;
        rng_.seed(std::random_device{}());
    }

    RefreshFailureMetadataOnStart();

    running_.store(true);
    worker_ = std::thread(&FenixFailureCoordinator::WorkerLoop, this);
}

void FenixFailureCoordinator::Stop() {
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void FenixFailureCoordinator::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    automation_enabled_ = enabled;
    last_no_trigger_log_ms_ = 0;
}

bool FenixFailureCoordinator::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return automation_enabled_;
}

void FenixFailureCoordinator::PanicStop() {
    FenixSimFailuresClient* client = nullptr;
    int pending_snapshot = 0;

    {
        std::lock_guard<std::mutex> lk(mu_);
        automation_enabled_ = false;
        last_no_trigger_log_ms_ = 0;
        client = client_;
        pending_snapshot = pending_credits_;
    }

    std::wstringstream start_ws;
    start_ws << L"FENIX: panic stop engaged; automation disabled, pending credits preserved at "
             << pending_snapshot << L". Clearing active and armed failures now.";
    Log(start_ws.str());

    if (client == nullptr) {
        Log(L"FENIX: panic stop could not clear failures because the Fenix failure client is not initialized.");
        return;
    }

    std::vector<Failure> failures;
    std::string fetch_error;
    if (!client->FetchManualFailures(failures, &fetch_error)) {
        Log(L"FENIX: panic stop could not read current failures: " +
            SafeToW(fetch_error.empty() ? std::string("unknown_error") : fetch_error));
        return;
    }

    int active_found = 0;
    int armed_found = 0;
    int active_cleared = 0;
    int armed_cleared = 0;
    int clear_failures = 0;

    for (const auto& failure : failures) {
        if (failure.id.empty()) continue;

        if (failure.IsActive()) {
            ++active_found;
            std::string clear_error;
            if (client->ClearFailure(failure.id, &clear_error)) {
                ++active_cleared;
            } else {
                ++clear_failures;
                Log(L"FENIX: panic stop failed to clear active failure " + SafeToW(failure.id) +
                    L": " + SafeToW(clear_error.empty() ? std::string("unknown_error") : clear_error));
            }
            continue;
        }

        if (failure.IsArmed()) {
            ++armed_found;
            std::string clear_error;
            if (client->ClearArmedFailure(failure.id, &clear_error)) {
                ++armed_cleared;
            } else {
                ++clear_failures;
                Log(L"FENIX: panic stop failed to clear armed failure " + SafeToW(failure.id) +
                    L": " + SafeToW(clear_error.empty() ? std::string("unknown_error") : clear_error));
            }
        }
    }

    int remaining_active = -1;
    int remaining_armed = -1;
    std::vector<Failure> verify_failures;
    std::string verify_error;
    if (client->FetchManualFailures(verify_failures, &verify_error)) {
        remaining_active = 0;
        remaining_armed = 0;
        for (const auto& failure : verify_failures) {
            if (failure.IsActive()) ++remaining_active;
            else if (failure.IsArmed()) ++remaining_armed;
        }
    } else {
        Log(L"FENIX: panic stop could not verify cleared failures: " +
            SafeToW(verify_error.empty() ? std::string("unknown_error") : verify_error));
    }

    std::wstringstream done_ws;
    done_ws << L"FENIX: panic stop completed; active cleared " << active_cleared << L"/" << active_found
            << L", armed cleared " << armed_cleared << L"/" << armed_found
            << L", pending credits preserved=" << pending_snapshot;
    if (remaining_active >= 0 && remaining_armed >= 0) {
        done_ws << L", remaining active=" << remaining_active
                << L", remaining armed=" << remaining_armed;
    }
    if (clear_failures > 0) {
        done_ws << L", clear failures=" << clear_failures;
    }
    Log(done_ws.str());
}

nlohmann::json FenixFailureCoordinator::StatusJson() const {
    nlohmann::json out;
    out["ok"] = true;

    FenixSimFailuresClient* client = nullptr;
    bool enabled_now = false;
    int pending_now = 0;

    {
        std::lock_guard<std::mutex> lk(mu_);
        client = client_;
        enabled_now = automation_enabled_;
        pending_now = pending_credits_;
    }

    out["enabled"] = enabled_now;
    out["pending_credits"] = pending_now;
    out["selection_mode"] = "60% immediate / 40% armed";
    out["mode_label"] = "60% immediate / 40% armed";

    if (client == nullptr) {
        out["connected"] = false;
        out["active_failures"] = 0;
        out["armed_failures"] = 0;
        out["status_label"] = "Unavailable";
        out["summary"] = "Simulator automation backend is running, but the Fenix integration is not initialized.";
        out["summary_sub"] = "Start the app normally with the Fenix integration available to populate live status.";
        out["recent_activity"] = nlohmann::json::array();
        return out;
    }

    std::vector<Failure> failures;
    std::string fetch_error;
    if (!client->FetchManualFailures(failures, &fetch_error)) {
        out["connected"] = false;
        out["active_failures"] = 0;
        out["armed_failures"] = 0;
        out["status_label"] = "Unavailable";
        out["summary"] = fetch_error.empty()
            ? "Simulator automation could not read the Fenix manual failures endpoint."
            : ("Simulator automation could not read the Fenix manual failures endpoint: " + fetch_error);
        out["summary_sub"] = "Load the Fenix aircraft and EFB, then retry.";
        out["recent_activity"] = nlohmann::json::array();
        return out;
    }

    int active = 0;
    int armed = 0;
    for (const auto& failure : failures) {
        if (failure.IsActive()) ++active;
        else if (failure.IsArmed()) ++armed;
    }

    std::size_t discovered_failures = 0;
    std::size_t metadata_entries = 0;
    std::size_t stale_metadata_entries = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        discovered_failures = discovered_failure_count_;
        metadata_entries = metadata_entry_count_;
        stale_metadata_entries = stale_metadata_entry_count_;
    }

    out["connected"] = true;
    out["active_failures"] = active;
    out["armed_failures"] = armed;
    out["catalog_discovered_failures"] = discovered_failures;
    out["catalog_metadata_entries"] = metadata_entries;
    out["catalog_stale_entries"] = stale_metadata_entries;
    out["status_label"] = enabled_now ? "Enabled" : "Disabled";
    out["summary"] = enabled_now
        ? "Simulator automation is enabled and ready to react to support events."
        : "Simulator automation is disabled.";
    out["summary_sub"] = "This panel provides light-touch live status and emergency controls; deeper settings can live in the Settings section later.";
    out["recent_activity"] = nlohmann::json::array();
    return out;
}

void FenixFailureCoordinator::RefreshFailureMetadataOnStart() {
    FenixSimFailuresClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        client = client_;
    }

    if (client == nullptr) {
        Log(L"FENIX: failure metadata refresh skipped because the Fenix failure client is not initialized.");
        return;
    }

    std::vector<Failure> failures;
    std::string fetch_error;
    if (!client->FetchManualFailures(failures, &fetch_error)) {
        Log(L"FENIX: failure metadata refresh skipped because the manual failures endpoint could not be read: " +
            SafeToW(fetch_error.empty() ? std::string("unknown_error") : fetch_error));
        return;
    }

    FailureMetadataRefreshSummary summary;
    std::string refresh_error;
    if (!metadata_store_.RefreshFromFailures(failures, summary, &refresh_error)) {
        Log(L"FENIX: failure metadata refresh failed: " +
            SafeToW(refresh_error.empty() ? std::string("unknown_error") : refresh_error));
        return;
    }

    std::vector<MergedFailureCatalogEntry> merged_catalog;
    std::string load_error;
    if (!metadata_store_.LoadMergedCatalog(failures, merged_catalog, &load_error)) {
        Log(L"FENIX: failure metadata refresh saved successfully, but the merged catalog could not be reloaded: " +
            SafeToW(load_error.empty() ? std::string("unknown_error") : load_error));
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        merged_catalog_ = std::move(merged_catalog);
        discovered_failure_count_ = summary.discovered_failures;
        metadata_entry_count_ = summary.metadata_entries_after;
        stale_metadata_entry_count_ = summary.stale_entries;
    }

    std::wstringstream ws;
    ws << L"FENIX: failure metadata refreshed; discovered=" << summary.discovered_failures
       << L", metadata_entries=" << summary.metadata_entries_after
       << L", new=" << summary.new_entries
       << L", stale=" << summary.stale_entries
       << L", file=" << summary.metadata_path;
    Log(ws.str());
}

void FenixFailureCoordinator::WorkerLoop() {
    SeedSeenFromCurrentQueues();
    Log(L"FENIX: failure coordinator started.");

    while (running_.load()) {
        try {
            std::vector<nlohmann::json> new_events;
            CollectNewEvents(new_events);

            for (const auto& event : new_events) {
                const int credits = CreditsFromEvent(event);
                if (credits <= 0) continue;

                bool automation_enabled = false;
                int pending_now = 0;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    automation_enabled = automation_enabled_;
                    pending_credits_ += credits;
                    pending_now = pending_credits_;
                }

                const std::string platform = event.value("platform", "");
                const std::string type = event.value("type", event.value("event_type", ""));
                const std::string user = event.value("user", "");

                std::wstringstream ws;
                ws << L"FENIX: awarded " << credits
                   << L" failure credit" << (credits == 1 ? L"" : L"s")
                   << L" from " << SafeToW(platform)
                   << L" event '" << SafeToW(type) << L"'";
                if (!user.empty()) {
                    ws << L" by " << SafeToW(user);
                }
                ws << L"; pending credits now " << pending_now;
                if (!automation_enabled) {
                    ws << L" (queued while automation disabled)";
                }
                Log(ws.str());
            }

            for (;;) {
                int pending = 0;
                bool automation_enabled = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    pending = pending_credits_;
                    automation_enabled = automation_enabled_;
                }
                if (!automation_enabled || pending <= 0) break;
                if (!SpendOnePendingCredit()) break;
            }
        }
        catch (const std::exception& ex) {
            Log(L"FENIX: failure coordinator exception: " + SafeToW(ex.what()));
        }
        catch (...) {
            Log(L"FENIX: failure coordinator exception: unknown");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    Log(L"FENIX: failure coordinator stopped.");
}

void FenixFailureCoordinator::SeedSeenFromCurrentQueues() {
    AppState* state = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        state = state_;
    }
    if (state == nullptr) return;

    std::vector<nlohmann::json> seed_events;
    CollectNewEvents(seed_events);
    (void)seed_events;
}

void FenixFailureCoordinator::CollectNewEvents(std::vector<nlohmann::json>& out_events) {
    out_events.clear();

    AppState* state = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        state = state_;
    }
    if (state == nullptr) return;

    try {
        const auto twitch = state->twitch_eventsub_events_json(200);
        if (twitch.is_object() && twitch.contains("events") && twitch["events"].is_array()) {
            CollectNewEventsFromArray("twitch", twitch["events"], out_events);
        }
    }
    catch (...) {}

    try {
        const auto tiktok = state->tiktok_events_json(200);
        if (tiktok.is_object() && tiktok.contains("events") && tiktok["events"].is_array()) {
            CollectNewEventsFromArray("tiktok", tiktok["events"], out_events);
        }
    }
    catch (...) {}

    try {
        const auto youtube = state->youtube_events_json(200);
        if (youtube.is_object() && youtube.contains("events") && youtube["events"].is_array()) {
            CollectNewEventsFromArray("youtube", youtube["events"], out_events);
        }
    }
    catch (...) {}
}

void FenixFailureCoordinator::CollectNewEventsFromArray(const char* platform,
                                                        const nlohmann::json& events,
                                                        std::vector<nlohmann::json>& out_events) {
    if (!events.is_array()) return;

    for (const auto& event : events) {
        if (!event.is_object()) continue;

        nlohmann::json normalized = event;
        if (!normalized.contains("platform")) {
            normalized["platform"] = platform;
        }

        const std::string key = MakeEventKey(platform, normalized);
        if (key.empty()) continue;

        if (RememberEventKey(key)) {
            out_events.push_back(std::move(normalized));
        }
    }
}

std::string FenixFailureCoordinator::MakeEventKey(const char* platform, const nlohmann::json& event) const {
    const std::string id = event.value("id", std::string{});
    const std::string redemption_id = event.value("redemption_id", std::string{});
    const std::string type = event.value("type", event.value("event_type", std::string{}));
    const std::string user = event.value("user", std::string{});
    const std::string message = event.value("message", std::string{});
    const std::int64_t ts_ms = event.value("ts_ms", (std::int64_t)0);

    if (!id.empty()) {
        return std::string(platform) + "|" + type + "|id|" + id;
    }
    if (!redemption_id.empty()) {
        return std::string(platform) + "|" + type + "|rid|" + redemption_id;
    }

    return std::string(platform) + "|" + type + "|" + user + "|" + std::to_string(ts_ms) + "|" + message;
}

bool FenixFailureCoordinator::RememberEventKey(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto inserted = seen_keys_.insert(key);
    if (!inserted.second) {
        return false;
    }

    seen_order_.push_back(key);
    while (seen_order_.size() > kSeenMax_) {
        seen_keys_.erase(seen_order_.front());
        seen_order_.pop_front();
    }
    return true;
}

int FenixFailureCoordinator::CreditsFromEvent(const nlohmann::json& event) {
    const std::string platform = ToLower(event.value("platform", ""));
    if (platform == "twitch") {
        return CreditsFromTwitchEvent(event);
    }
    if (platform == "tiktok") {
        return CreditsFromTikTokEvent(event);
    }
    if (platform == "youtube") {
        return CreditsFromYouTubeEvent(event);
    }
    return 0;
}

int FenixFailureCoordinator::CreditsFromTwitchEvent(const nlohmann::json& event) {
    const std::string type = ToLower(event.value("type", ""));
    const nlohmann::json raw = event.value("raw_event", nlohmann::json::object());

    if (type == "channel.subscribe" || type == "channel.subscription.message") {
        std::string tier = event.value("tier", std::string{});
        if (tier.empty() && raw.is_object()) tier = raw.value("tier", std::string{});
        const int credits = TierToCredits(tier);
        return credits > 0 ? credits : 1;
    }

    if (type == "channel.subscription.gift") {
        int total = JsonIntLoose(event, "total", 0);
        if (total <= 0 && raw.is_object()) total = JsonIntLoose(raw, "total", 0);
        return total > 0 ? total : 1;
    }

    if (type == "channel.cheer") {
        int bits = JsonIntLoose(event, "bits", 0);
        if (bits <= 0 && raw.is_object()) bits = JsonIntLoose(raw, "bits", 0);

        std::lock_guard<std::mutex> lk(mu_);
        const int combined = twitch_bits_remainder_ + bits;
        const int credits = combined / 100;
        twitch_bits_remainder_ = combined % 100;
        return credits;
    }

    return 0;
}

int FenixFailureCoordinator::CreditsFromTikTokEvent(const nlohmann::json& event) {
    const std::string type = ToLower(event.value("event_type", event.value("type", "")));

    if (type == "subscribe") {
        return 1;
    }

    if (type == "gift") {
        int value = JsonIntLoose(event, "gift_total_value", 0);
        if (value <= 0) value = JsonIntLoose(event, "gift_value", 0);

        std::lock_guard<std::mutex> lk(mu_);
        const int combined = tiktok_gift_remainder_ + value;
        const int credits = combined / 100;
        tiktok_gift_remainder_ = combined % 100;
        return credits;
    }

    return 0;
}

int FenixFailureCoordinator::CreditsFromYouTubeEvent(const nlohmann::json& event) {
    const std::string type = ToLower(event.value("type", ""));
    if (type == "membership" || type == "subscribe") {
        return 1;
    }
    return 0;
}

int FenixFailureCoordinator::TierToCredits(const std::string& tier) const {
    if (tier == "3000") return 3;
    if (tier == "2000") return 2;
    if (tier == "1000") return 1;
    return 1;
}

bool FenixFailureCoordinator::IsFailureOnRecentCooldown(const std::string& failure_id,
                                                        std::int64_t cooldown_ms,
                                                        std::int64_t now_ms,
                                                        std::int64_t* remaining_ms) const {
    if (remaining_ms != nullptr) {
        *remaining_ms = 0;
    }

    if (cooldown_ms <= 0 || failure_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    const auto it = recent_failure_last_used_ms_.find(failure_id);
    if (it == recent_failure_last_used_ms_.end()) {
        return false;
    }

    const std::int64_t elapsed = now_ms - it->second;
    if (elapsed >= cooldown_ms) {
        return false;
    }

    if (remaining_ms != nullptr) {
        *remaining_ms = cooldown_ms - elapsed;
    }
    return true;
}

void FenixFailureCoordinator::RememberTriggeredFailure(const std::string& failure_id,
                                                       std::int64_t now_ms) {
    if (failure_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lk(mu_);
    recent_failure_last_used_ms_[failure_id] = now_ms;
    ++triggered_failure_counts_session_[failure_id];
}

int FenixFailureCoordinator::TriggerCountThisSession(const std::string& failure_id) const {
    if (failure_id.empty()) {
        return 0;
    }

    std::lock_guard<std::mutex> lk(mu_);
    const auto it = triggered_failure_counts_session_.find(failure_id);
    return (it == triggered_failure_counts_session_.end()) ? 0 : it->second;
}

int FenixFailureCoordinator::WeightedRandomIndex(const std::vector<const MergedFailureCatalogEntry*>& candidates) {
    if (candidates.empty()) {
        return -1;
    }

    long long total_weight = 0;
    for (const auto* candidate : candidates) {
        if (candidate == nullptr) {
            continue;
        }
        const int weight = candidate->metadata.weight > 0 ? candidate->metadata.weight : 1;
        total_weight += static_cast<long long>(weight);
    }

    if (total_weight <= 0) {
        return RandomIntInclusive(0, static_cast<int>(candidates.size()) - 1);
    }

    std::lock_guard<std::mutex> lk(mu_);
    std::uniform_int_distribution<long long> dist(1, total_weight);
    long long roll = dist(rng_);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto* candidate = candidates[i];
        const int weight = (candidate != nullptr && candidate->metadata.weight > 0)
            ? candidate->metadata.weight
            : 1;
        roll -= static_cast<long long>(weight);
        if (roll <= 0) {
            return static_cast<int>(i);
        }
    }

    return static_cast<int>(candidates.size()) - 1;
}

bool FenixFailureCoordinator::ShouldArmRandomly() {
    const int roll = RandomIntInclusive(1, 100);
    return roll <= kArmFailureChancePercent_;
}

int FenixFailureCoordinator::RandomIntInclusive(int min_value, int max_value) {
    if (max_value < min_value) {
        std::swap(min_value, max_value);
    }

    std::lock_guard<std::mutex> lk(mu_);
    std::uniform_int_distribution<int> dist(min_value, max_value);
    return dist(rng_);
}

ArmedFailureCondition FenixFailureCoordinator::MakeRandomArmedCondition() {
    ArmedFailureCondition condition;
    const int template_id = RandomIntInclusive(0, 2);

    switch (template_id) {
    case 0:
        condition.ias = RandomIntInclusive(180, 280);
        break;
    case 1:
        condition.alt_above_amsl = RandomIntInclusive(5000, 37000);
        break;
    case 2:
    default:
        condition.after_event = "V1";
        condition.after_event_seconds = RandomIntInclusive(10, 120);
        break;
    }

    return condition;
}

std::string FenixFailureCoordinator::DescribeArmedCondition(const ArmedFailureCondition& condition) const {
    std::ostringstream oss;

    if (condition.ias.has_value()) {
        oss << "armed via IAS " << *condition.ias << " kt";
        return oss.str();
    }

    if (condition.alt_above_amsl.has_value()) {
        oss << "armed via altitude above " << *condition.alt_above_amsl << " ft";
        return oss.str();
    }

    if (condition.after_event.has_value()) {
        oss << "armed after " << *condition.after_event;
        if (condition.after_event_seconds.has_value()) {
            oss << " +" << *condition.after_event_seconds << "s";
        }
        return oss.str();
    }

    return "armed";
}

bool FenixFailureCoordinator::SpendOnePendingCredit() {
    int pending_before = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_credits_ <= 0) return false;
        pending_before = pending_credits_;
    }

    std::string triggered_id;
    std::string triggered_title;
    std::string action_desc;
    std::string detail;
    if (!TriggerOneFailure(triggered_id, triggered_title, action_desc, detail)) {
        const std::int64_t now = NowMs();
        bool should_log = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if ((now - last_no_trigger_log_ms_) > 10000) {
                last_no_trigger_log_ms_ = now;
                should_log = true;
            }
        }
        if (should_log) {
            Log(L"FENIX: pending failure credit could not be spent yet: " + SafeToW(detail));
        }
        return false;
    }

    int pending_after = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_credits_ > 0) {
            --pending_credits_;
        }
        pending_after = pending_credits_;
        last_no_trigger_log_ms_ = 0;
    }

    std::wstringstream ws;
    ws << L"FENIX: " << SafeToW(action_desc) << L" " << SafeToW(triggered_id);
    if (!triggered_title.empty()) {
        ws << L" (" << SafeToW(triggered_title) << L")";
    }
    ws << L"; pending credits " << pending_before << L" -> " << pending_after;
    Log(ws.str());
    return true;
}

bool FenixFailureCoordinator::TriggerOneFailure(std::string& triggered_id,
                                                std::string& triggered_title,
                                                std::string& action_desc,
                                                std::string& detail) {
    triggered_id.clear();
    triggered_title.clear();
    action_desc.clear();
    detail.clear();

    FenixSimFailuresClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        client = client_;
    }

    if (client == nullptr) {
        detail = "Fenix failure client is not initialized.";
        return false;
    }

    std::vector<Failure> failures;
    std::string fetch_error;
    if (!client->FetchManualFailures(failures, &fetch_error)) {
        detail = fetch_error.empty() ? "Failed to fetch Fenix manual failures." : fetch_error;
        return false;
    }

    std::vector<MergedFailureCatalogEntry> merged_catalog;
    std::string load_error;
    if (!metadata_store_.LoadMergedCatalog(failures, merged_catalog, &load_error)) {
        detail = load_error.empty()
            ? "Failed to load Fenix failure metadata."
            : ("Failed to load Fenix failure metadata: " + load_error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        merged_catalog_ = merged_catalog;
    }

    const std::int64_t now_ms = NowMs();

    std::vector<const MergedFailureCatalogEntry*> candidates;
    candidates.reserve(merged_catalog.size());

    int disabled_skipped = 0;
    int not_stream_safe_skipped = 0;
    int missing_from_catalog_skipped = 0;
    int active_or_armed_skipped = 0;
    int cooldown_skipped = 0;
    int session_cap_skipped = 0;
    std::int64_t shortest_remaining_cooldown_ms = -1;

    for (const auto& entry : merged_catalog) {
        const auto& failure = entry.failure;
        const auto& metadata = entry.metadata;

        if (failure.id.empty()) {
            continue;
        }

        if (!metadata.present_in_current_catalog) {
            ++missing_from_catalog_skipped;
            continue;
        }

        if (!metadata.enabled) {
            ++disabled_skipped;
            continue;
        }

        if (!metadata.stream_safe) {
            ++not_stream_safe_skipped;
            continue;
        }

        if (failure.IsActive() || failure.IsArmed()) {
            ++active_or_armed_skipped;
            continue;
        }

        std::int64_t remaining_ms = 0;
        if (IsFailureOnRecentCooldown(failure.id, metadata.cooldown_ms, now_ms, &remaining_ms)) {
            ++cooldown_skipped;
            if (shortest_remaining_cooldown_ms < 0 || remaining_ms < shortest_remaining_cooldown_ms) {
                shortest_remaining_cooldown_ms = remaining_ms;
            }
            continue;
        }

        const int effective_max_per_session =
            metadata.repeatable
                ? (metadata.max_per_session > 0 ? metadata.max_per_session : 1)
                : 1;

        if (TriggerCountThisSession(failure.id) >= effective_max_per_session) {
            ++session_cap_skipped;
            continue;
        }

        candidates.push_back(&entry);
    }

    if (candidates.empty()) {
        std::ostringstream oss;
        oss << "No eligible Fenix failures are available after metadata filtering. "
            << "Excluded disabled=" << disabled_skipped
            << ", not_stream_safe=" << not_stream_safe_skipped
            << ", missing_from_catalog=" << missing_from_catalog_skipped
            << ", active_or_armed=" << active_or_armed_skipped
            << ", cooldown=" << cooldown_skipped
            << ", session_cap=" << session_cap_skipped;
        if (shortest_remaining_cooldown_ms >= 0) {
            oss << ", shortest_cooldown_remaining_ms=" << shortest_remaining_cooldown_ms;
        }
        detail = oss.str();
        return false;
    }

    while (!candidates.empty()) {
        const int selected_index = WeightedRandomIndex(candidates);
        if (selected_index < 0 || selected_index >= static_cast<int>(candidates.size())) {
            detail = "Failed to choose a weighted Fenix failure candidate.";
            return false;
        }

        const MergedFailureCatalogEntry* candidate = candidates[static_cast<std::size_t>(selected_index)];
        candidates.erase(candidates.begin() + selected_index);

        if (candidate == nullptr) {
            continue;
        }

        const bool arm_this_failure = ShouldArmRandomly();

        if (arm_this_failure) {
            const ArmedFailureCondition armed_condition = MakeRandomArmedCondition();

            SafeWriteResult result = SafeWriteResult::InvalidResponse;
            std::string write_error;
            const bool ok = client->ArmFailureIfInactive(candidate->failure.id, armed_condition, result, &write_error);

            if (ok && result == SafeWriteResult::Success) {
                RememberTriggeredFailure(candidate->failure.id, now_ms);
                triggered_id = candidate->failure.id;
                triggered_title = candidate->metadata.title.empty() ? candidate->failure.title : candidate->metadata.title;
                action_desc = "armed failure";
                action_desc += " (" + DescribeArmedCondition(armed_condition) + ")";
                return true;
            }

            if (result == SafeWriteResult::SkippedAlreadyActive || result == SafeWriteResult::SkippedAlreadyArmed) {
                continue;
            }

            std::ostringstream oss;
            oss << "Failed to arm " << candidate->failure.id << ": " << SafeWriteResultToString(result);
            if (!write_error.empty()) {
                oss << " (" << write_error << ")";
            }
            detail = oss.str();
            continue;
        }

        SafeWriteResult result = SafeWriteResult::InvalidResponse;
        std::string write_error;
        const bool ok = client->TriggerFailureNowIfInactive(candidate->failure.id, result, &write_error);

        if (ok && result == SafeWriteResult::Success) {
            RememberTriggeredFailure(candidate->failure.id, now_ms);
            triggered_id = candidate->failure.id;
            triggered_title = candidate->metadata.title.empty() ? candidate->failure.title : candidate->metadata.title;
            action_desc = "triggered immediate failure";
            return true;
        }

        if (result == SafeWriteResult::SkippedAlreadyActive || result == SafeWriteResult::SkippedAlreadyArmed) {
            continue;
        }

        std::ostringstream oss;
        oss << "Failed to trigger " << candidate->failure.id << ": " << SafeWriteResultToString(result);
        if (!write_error.empty()) {
            oss << " (" << write_error << ")";
        }
        detail = oss.str();
    }

    if (detail.empty()) {
        detail = "Failed to select and write a metadata-eligible Fenix failure.";
    }
    return false;
}

void FenixFailureCoordinator::Log(const std::wstring& msg) const {
    LogFn fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn = log_;
    }
    if (fn) {
        fn(msg);
    }
}

std::wstring FenixFailureCoordinator::ToW(const std::string& s) {
    return SafeToW(s);
}

std::string FenixFailureCoordinator::ToLower(std::string s) {
    return SafeToLower(std::move(s));
}

int FenixFailureCoordinator::JsonIntLoose(const nlohmann::json& obj, const char* key, int fallback) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj.at(key);
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number()) return (int)v.get<double>();
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        }
        catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::int64_t FenixFailureCoordinator::NowMs() {
    return (std::int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace fenixsim
