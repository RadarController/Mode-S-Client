#include "PlatformControl.h"

#define NOMINMAX
#include <windows.h>
#include <string>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

#include "tiktok/TikTokSidecar.h"
#include "twitch/TwitchIrcWsClient.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "chat/ChatAggregator.h"
#include "AppConfig.h"
#include "AppState.h"
#include "youtube/YouTubeSubscriberProvider.h"

using nlohmann::json;

namespace {

// Basic sanitizers (kept minimal and consistent with your existing UI logic)
static std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    auto is_space = [](unsigned char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'; };
    while (a < b && is_space((unsigned char)s[a])) a++;
    while (b > a && is_space((unsigned char)s[b-1])) b--;
    return s.substr(a, b-a);
}

static std::string SanitizeTikTok(std::string s) {
    s = Trim(s);
    if (!s.empty() && s[0] == '@') s.erase(0, 1);
    // TikTok unique_id is generally [A-Za-z0-9._]
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){
        return !(std::isalnum(c) || c=='.' || c=='_');
    }), s.end());
    return s;
}

static std::string SanitizeTwitchLogin(std::string s) {
    s = Trim(s);
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (!s.empty() && s[0] == '@') s.erase(0, 1);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){
        return !(std::isalnum(c) || c=='_');
    }), s.end());
    return s;
}

static std::string SanitizeYouTubeHandle(std::string s) {
    s = Trim(s);
    if (!s.empty() && s[0] == '@') s.erase(0, 1);
    // allow typical handle chars
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){
        return !(std::isalnum(c) || c=='.' || c=='_' || c=='-');
    }), s.end());
    return s;
}

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}


struct YouTubeSubscriberPollerState {
    std::atomic<bool> running{ false };
    std::thread thread;
    std::mutex mu;
    std::unordered_set<std::string> seen_ids;
    bool seeded = false;
};

static YouTubeSubscriberPollerState g_youtubeSubscriberPoller;

static std::optional<std::string> LoadYouTubeAccessTokenFromConfig() {
    const std::wstring path = AppConfig::ConfigPath();

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return std::nullopt;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string data;
    data.resize(sz > 0 ? (size_t)sz : 0);
    if (sz > 0) fread(data.data(), 1, (size_t)sz, f);
    fclose(f);

    try {
        const auto j = nlohmann::json::parse(data, nullptr, false);
        if (!j.is_object()) return std::nullopt;

        const auto yt = j.value("youtube", nlohmann::json::object());
        if (!yt.is_object()) return std::nullopt;

        const std::string token = yt.value("access_token", std::string{});
        if (token.empty()) return std::nullopt;
        return token;
    }
    catch (...) {
        return std::nullopt;
    }
}

static std::int64_t NowMs() {
    return (std::int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static void StopYouTubeSubscriberPoller();

static void StartYouTubeSubscriberPoller(AppState& state, PlatformControl::LogFn log) {
    StopYouTubeSubscriberPoller();

    {
        std::lock_guard<std::mutex> lock(g_youtubeSubscriberPoller.mu);
        g_youtubeSubscriberPoller.seen_ids.clear();
        g_youtubeSubscriberPoller.seeded = false;
    }

    g_youtubeSubscriberPoller.running.store(true);

    g_youtubeSubscriberPoller.thread = std::thread([&state, log]() {
        youtube::YouTubeSubscriberProvider provider(
            []() { return LoadYouTubeAccessTokenFromConfig(); },
            [log](const std::wstring& msg) {
                if (log) log(msg);
            });

        std::string last_status;

        while (g_youtubeSubscriberPoller.running.load()) {
            std::string error;
            auto recent = provider.FetchRecent(25, &error);

            const bool fetch_ok = error.empty();
            if (!fetch_ok) {
                if (error != last_status) {
                    last_status = error;
                    if (log && !error.empty()) {
                        log(L"YOUTUBE: subscriber poller warning: " + ToW(error));
                    }
                }
            }
            else {
                if (!last_status.empty() && log) {
                    log(L"YOUTUBE: subscriber poller recovered.");
                }
                last_status.clear();

                std::vector<youtube::RecentSubscriber> unseen_to_emit;
                bool seeded_now = false;

                {
                    std::lock_guard<std::mutex> lock(g_youtubeSubscriberPoller.mu);

                    if (!g_youtubeSubscriberPoller.seeded) {
                        for (const auto& item : recent) {
                            if (!item.subscription_id.empty()) {
                                g_youtubeSubscriberPoller.seen_ids.insert(item.subscription_id);
                            }
                        }
                        g_youtubeSubscriberPoller.seeded = true;
                        seeded_now = true;
                    }
                    else {
                        for (const auto& item : recent) {
                            if (item.subscription_id.empty()) continue;
                            const auto inserted = g_youtubeSubscriberPoller.seen_ids.insert(item.subscription_id);
                            if (inserted.second) {
                                unseen_to_emit.push_back(item);
                            }
                        }
                    }

                    if (g_youtubeSubscriberPoller.seen_ids.size() > 4000) {
                        g_youtubeSubscriberPoller.seen_ids.clear();
                        for (const auto& item : recent) {
                            if (!item.subscription_id.empty()) {
                                g_youtubeSubscriberPoller.seen_ids.insert(item.subscription_id);
                            }
                        }
                    }
                }

                if (seeded_now && log) {
                    log(L"YOUTUBE: subscriber poller seeded from recent subscriber list.");
                }

                for (auto it = unseen_to_emit.rbegin(); it != unseen_to_emit.rend(); ++it) {
                    if (!g_youtubeSubscriberPoller.running.load()) break;

                    EventItem e;
                    e.platform = "youtube";
                    e.type = "subscribe";
                    e.user = it->subscriber_title.empty() ? "Someone" : it->subscriber_title;
                    e.message = "subscribed";
                    e.ts_ms = it->subscribed_at_ms > 0 ? it->subscribed_at_ms : NowMs();
                    state.push_youtube_event(e);

                    if (log) {
                        log(L"YOUTUBE: new public subscriber: " + ToW(e.user));
                    }
                }
            }

            for (int i = 0; i < 15 && g_youtubeSubscriberPoller.running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

static void StopYouTubeSubscriberPoller() {
    g_youtubeSubscriberPoller.running.store(false);

    if (g_youtubeSubscriberPoller.thread.joinable()) {
        try {
            g_youtubeSubscriberPoller.thread.join();
        }
        catch (...) {
        }
    }

    std::lock_guard<std::mutex> lock(g_youtubeSubscriberPoller.mu);
    g_youtubeSubscriberPoller.seen_ids.clear();
    g_youtubeSubscriberPoller.seeded = false;
}

} // namespace

namespace PlatformControl {

    bool StartOrRestartTikTokSidecar(
        TikTokSidecar& tiktok,
        AppState& state,
        ChatAggregator& chat,
        const std::wstring& exeDir,
        const std::string& tiktokUniqueId,
        LogFn log)
    {
        std::string cleaned = SanitizeTikTok(tiktokUniqueId);
        if (cleaned.empty()) {
            if (log) log(L"TikTok username is empty. Enter it first.");
            return false;
        }

        tiktok.stop();

        std::wstring sidecarPath = exeDir + L"\\sidecar\\tiktok_sidecar.py";
        if (log) log(L"Starting python sidecar: " + sidecarPath);

        _wputenv_s(L"WHITELIST_AUTHENTICATED_SESSION_ID_HOST", L"tiktok.eulerstream.com");

        bool ok = tiktok.start(L"python", sidecarPath, [log, &state, &chat](const json& j) {
            std::string type = j.value("type", "");
            std::string msg = j.value("message", "");
            if (type == "tiktok.send_result" && log) {
                const bool ok = j.value("ok", false);
                const std::string text = j.value("text", "");
                std::string extra = ok ? " | send_chat OK" : " | send_chat FAILED";
                if (!text.empty()) extra += " | " + text;
                log(ToW("TIKTOK: " + type + extra));
            }
            else if (type.rfind("tiktok.", 0) == 0 && log) {
                std::string extra;
                if (!msg.empty()) extra = " | " + msg;
                log(ToW("TIKTOK: " + type + extra));
            }

            if (type == "tiktok.connected") {
                state.set_tiktok_live(true);
            }
            else if (type == "tiktok.disconnected" || type == "tiktok.offline" || type == "tiktok.error") {
                state.set_tiktok_live(false);
                state.set_tiktok_viewers(0);
            }
            else if (type == "tiktok.event") {
                EventItem e;
                e.platform = "tiktok";
                e.type = j.value("event_type", j.value("kind", j.value("event", "")));
                if (e.type.empty()) e.type = "event";
                e.user = j.value("user", "unknown");
                e.message = j.value("message", "");
                if (j.contains("ts_ms")) e.ts_ms = (std::int64_t)j.value("ts_ms", 0LL);
                else {
                    double ts = j.value("ts", 0.0);
                    e.ts_ms = (std::int64_t)(ts * 1000.0);
                }

                // Preserve the full structured sidecar payload so later app logic
                // can use gift_count / gift_total_value / subscription flags without
                // having to parse the human-readable message text.
                e.data = j;

                state.push_tiktok_event(e);
            }
            else if (type == "tiktok.chat") {
                ChatMessage c;
                c.platform = "tiktok";
                c.user = j.value("user", "unknown");
                c.message = j.value("message", "");
                double ts = j.value("ts", 0.0);
                c.ts_ms = (std::int64_t)(ts * 1000.0);
                chat.Add(std::move(c));
            }
            else if (type == "tiktok.stats") {
                bool live = j.value("live", false);
                int viewers = j.value("viewers", 0);

                state.set_tiktok_live(live);
                state.set_tiktok_viewers(viewers);

                if (j.contains("followers")) {
                    state.set_tiktok_followers(j.value("followers", 0));
                }
            }
            else if (type == "tiktok.viewers") {
                state.set_tiktok_viewers(j.value("viewers", 0));
            }
            });

        if (log) {
            log(ok ? L"TikTok sidecar started/restarted." :
                L"ERROR: Could not start TikTok sidecar. Check Python + TikTokLive install.");
        }
        return ok;
    }

    bool StartOrRestartYouTubeSidecar(
        TikTokSidecar& youtube,
        AppState& state,
        ChatAggregator& chat,
        const std::wstring& exeDir,
        const std::string& youtubeHandle,
        LogFn log)
    {
        std::string cleaned = SanitizeYouTubeHandle(youtubeHandle);
        if (cleaned.empty()) {
            if (log) log(L"YouTube handle is empty. Enter it first.");
            return false;
        }

        youtube.stop();

        std::wstring sidecarPath = exeDir + L"\\sidecar\\youtube_sidecar.py";
        if (log) log(L"Starting python sidecar: " + sidecarPath);

        bool ok = youtube.start(L"python", sidecarPath, [log, &state, &chat](const json& j) {
            std::string type = j.value("type", "");
            std::string msg = j.value("message", "");

            if (type.rfind("youtube.", 0) == 0 && log) {
                std::string extra;
                if (!msg.empty()) extra = " | " + msg;
                log(ToW("YOUTUBE: " + type + extra));
            }

            if (type == "youtube.connected") {
                state.set_youtube_live(false);
            }
            else if (type == "youtube.disconnected" || type == "youtube.offline" || type == "youtube.error") {
                state.set_youtube_live(false);
                state.set_youtube_viewers(0);
            }
            else if (type == "youtube.chat") {
                ChatMessage c;
                c.platform = "youtube";
                c.user = j.value("user", "unknown");
                c.message = j.value("message", "");
                double ts = j.value("ts", 0.0);
                c.ts_ms = (std::int64_t)(ts * 1000.0);
                chat.Add(std::move(c));
            }
            else if (type == "youtube.stats") {
                bool live = j.value("live", false);
                int viewers = j.value("viewers", 0);
                state.set_youtube_live(live);
                state.set_youtube_viewers(viewers);
                if (j.contains("followers")) state.set_youtube_followers(j.value("followers", 0));
            }
            else if (type == "youtube.viewers") {
                state.set_youtube_viewers(j.value("viewers", 0));
            }
            });

        if (ok) {
            StartYouTubeSubscriberPoller(state, log);
        }
        else {
            StopYouTubeSubscriberPoller();
        }

        if (log) {
            log(ok ? L"YouTube sidecar started/restarted." :
                L"ERROR: Could not start YouTube sidecar. Check Python + deps.");
        }
        return ok;
    }

bool StartOrRestartYouTubeFeatures(
    AppState& state,
    const std::string& youtubeHandle,
    LogFn log)
{
    const std::string cleaned = SanitizeYouTubeHandle(youtubeHandle);
    if (cleaned.empty()) {
        if (log) log(L"YOUTUBE: handle is empty - refusing to start features");
        return false;
    }

    StartYouTubeSubscriberPoller(state, log);

    if (log) log(L"YOUTUBE: platform features started.");
    return true;
}

void StopYouTubeFeatures(LogFn log)
{
    StopYouTubeSubscriberPoller();

    if (log) log(L"YOUTUBE: platform features stopped.");
}

bool StartOrRestartTwitchIrc(
    TwitchIrcWsClient& twitch,
    AppState& /*state*/,
    ChatAggregator& chat,
    const std::string& twitchLogin,
    const std::string& accessTokenRaw,
    LogFn log)
{
    std::string cleaned = SanitizeTwitchLogin(twitchLogin);
    if (cleaned.empty()) {
        if (log) log(L"TWITCH: channel is empty.");
        return false;
    }

    if (accessTokenRaw.empty()) {
        if (log) log(L"TWITCH: no OAuth user token available; refusing to start IRC (replies require auth).");
        twitch.stop();
        return false;
    }

    twitch.stop();
    twitch.SetChatAggregator(&chat);

    const bool ok = twitch.StartAuthenticated(
        cleaned,
        accessTokenRaw,
        cleaned
    );

    if (log) {
        log(ok ? L"TWITCH: started/restarted IRC client (authenticated)." :
                 L"TWITCH: failed to start IRC client (authenticated).");
    }
    return ok;
}
void StopTikTok(TikTokSidecar& tiktok, AppState& state, LogFn log) {
    tiktok.stop();
    state.set_tiktok_live(false);
    state.set_tiktok_viewers(0);
    if (log) log(L"TIKTOK: stopped.");
}
void StopYouTube(TikTokSidecar& youtube, AppState& state, LogFn log) {
    (void)state;
    youtube.stop();
    StopYouTubeFeatures(log);
}
void StopTwitch(TwitchIrcWsClient& twitch, TwitchEventSubWsClient& twitchEventSub, AppState& state, LogFn log) {
    twitch.stop();
    twitchEventSub.Stop();
    state.set_twitch_live(false);
    state.set_twitch_viewers(0);
    if (log) log(L"TWITCH: stopped IRC + EventSub.");
}

} // namespace PlatformControl