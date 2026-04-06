#include "fenixsim/FenixFailureCoordinator.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include <windows.h>

#include "AppState.h"
#include "fenixsim/FenixSimFailures.h"

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
        next_failure_index_ = 0;
        last_no_trigger_log_ms_ = 0;
        allowed_failures_ = {
            "F_SDCU",
            "F_PNEUMATIC_CPC_1",
            "F_ICE_STAT_STBY_L",
            "F_HYD_LEAK_BLUE",
            "F_HYD_LEAK_GREEN",
            "F_HYD_LEAK_YELLOW"
        };
    }

    running_.store(true);
    worker_ = std::thread(&FenixFailureCoordinator::WorkerLoop, this);
}

void FenixFailureCoordinator::Stop() {
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
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

                {
                    std::lock_guard<std::mutex> lk(mu_);
                    pending_credits_ += credits;
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
                Log(ws.str());
            }

            for (;;) {
                int pending = 0;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    pending = pending_credits_;
                }
                if (pending <= 0) break;
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

bool FenixFailureCoordinator::SpendOnePendingCredit() {
    int pending_before = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_credits_ <= 0) return false;
        pending_before = pending_credits_;
    }

    std::string triggered_id;
    std::string triggered_title;
    std::string detail;
    if (!TriggerOneFailure(triggered_id, triggered_title, detail)) {
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
    ws << L"FENIX: triggered failure " << SafeToW(triggered_id);
    if (!triggered_title.empty()) {
        ws << L" (" << SafeToW(triggered_title) << L")";
    }
    ws << L"; pending credits " << pending_before << L" -> " << pending_after;
    Log(ws.str());
    return true;
}

bool FenixFailureCoordinator::TriggerOneFailure(std::string& triggered_id,
                                                std::string& triggered_title,
                                                std::string& detail) {
    triggered_id.clear();
    triggered_title.clear();
    detail.clear();

    FenixSimFailuresClient* client = nullptr;
    std::vector<std::string> allowlist;
    std::size_t start_index = 0;

    {
        std::lock_guard<std::mutex> lk(mu_);
        client = client_;
        allowlist = allowed_failures_;
        start_index = next_failure_index_;
    }

    if (client == nullptr || allowlist.empty()) {
        detail = "Fenix failure client is not initialized.";
        return false;
    }

    std::vector<Failure> failures;
    std::string fetch_error;
    if (!client->FetchManualFailures(failures, &fetch_error)) {
        detail = fetch_error.empty() ? "Failed to fetch Fenix manual failures." : fetch_error;
        return false;
    }

    for (std::size_t offset = 0; offset < allowlist.size(); ++offset) {
        const std::size_t index = (start_index + offset) % allowlist.size();
        const std::string& candidate_id = allowlist[index];

        auto it = std::find_if(failures.begin(), failures.end(), [&](const Failure& failure) {
            return failure.id == candidate_id;
        });

        if (it == failures.end()) {
            continue;
        }

        if (it->IsActive() || it->IsArmed()) {
            continue;
        }

        SafeWriteResult result = SafeWriteResult::InvalidResponse;
        std::string write_error;
        const bool ok = client->TriggerFailureNowIfInactive(candidate_id, result, &write_error);

        if (ok && result == SafeWriteResult::Success) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                next_failure_index_ = (index + 1) % allowlist.size();
            }
            triggered_id = candidate_id;
            triggered_title = it->title;
            return true;
        }

        if (result == SafeWriteResult::SkippedAlreadyActive || result == SafeWriteResult::SkippedAlreadyArmed) {
            continue;
        }

        std::ostringstream oss;
        oss << "Failed to trigger " << candidate_id << ": " << SafeWriteResultToString(result);
        if (!write_error.empty()) {
            oss << " (" << write_error << ")";
        }
        detail = oss.str();
    }

    if (detail.empty()) {
        detail = "No eligible inactive Fenix failures are available in the current allow-list.";
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
