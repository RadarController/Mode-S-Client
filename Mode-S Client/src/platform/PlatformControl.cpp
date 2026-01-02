#include "PlatformControl.h"

#include <windows.h>
#include <string>
#include <algorithm>

#include "tiktok/TikTokSidecar.h"
#include "twitch/TwitchIrcWsClient.h"
#include "chat/ChatAggregator.h"
#include "AppState.h"

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

} // namespace

namespace PlatformControl {

bool StartOrRestartTikTokSidecar(
    TikTokSidecar& tiktok,
    AppState& state,
    ChatAggregator& chat,
    const std::wstring& exeDir,
    const std::string& tiktokUniqueId,
    HWND hwndMain,
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

    bool ok = tiktok.start(L"python", sidecarPath, [log, hwndMain, &state, &chat](const json& j) {
        std::string type = j.value("type", "");
        std::string msg = j.value("message", "");
        if (type.rfind("tiktok.", 0) == 0 && log) {
            std::string extra;
            if (!msg.empty()) extra = " | " + msg;
            log(ToW("TIKTOK: " + type + extra));
        }

        auto uiPing = [hwndMain](){
            if (hwndMain) PostMessageW(hwndMain, WM_APP + 41, 0, 0);
        };

        if (type == "tiktok.connected") {
            state.set_tiktok_live(true);
            uiPing();
        } else if (type == "tiktok.disconnected" || type == "tiktok.offline" || type == "tiktok.error") {
            state.set_tiktok_live(false);
            state.set_tiktok_viewers(0);
            uiPing();
        } else if (type == "tiktok.chat") {
            ChatMessage c;
            c.platform = "tiktok";
            c.user = j.value("user", "unknown");
            c.message = j.value("message", "");
            double ts = j.value("ts", 0.0);
            c.ts_ms = (std::int64_t)(ts * 1000.0);
            chat.Add(std::move(c));
        } else if (type == "tiktok.stats") {
            bool live = j.value("live", false);
            int viewers = j.value("viewers", 0);

            state.set_tiktok_live(live);
            state.set_tiktok_viewers(viewers);

            if (j.contains("followers")) {
                state.set_tiktok_followers(j.value("followers", 0));
            }
            uiPing();
        } else if (type == "tiktok.viewers") {
            state.set_tiktok_viewers(j.value("viewers", 0));
            uiPing();
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
    HWND hwndMain,
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

    bool ok = youtube.start(L"python", sidecarPath, [log, hwndMain, &state, &chat](const json& j) {
        std::string type = j.value("type", "");
        std::string msg = j.value("message", "");

        if (type.rfind("youtube.", 0) == 0 && log) {
            std::string extra;
            if (!msg.empty()) extra = " | " + msg;
            log(ToW("YOUTUBE: " + type + extra));
        }

        auto uiPing = [hwndMain](){
            if (hwndMain) PostMessageW(hwndMain, WM_APP + 41, 0, 0);
        };

        if (type == "youtube.connected") {
            state.set_youtube_live(true);
            uiPing();
        } else if (type == "youtube.disconnected" || type == "youtube.offline" || type == "youtube.error") {
            state.set_youtube_live(false);
            state.set_youtube_viewers(0);
            uiPing();
        } else if (type == "youtube.chat") {
            ChatMessage c;
            c.platform = "youtube";
            c.user = j.value("user", "unknown");
            c.message = j.value("message", "");
            double ts = j.value("ts", 0.0);
            c.ts_ms = (std::int64_t)(ts * 1000.0);
            chat.Add(std::move(c));
        } else if (type == "youtube.stats") {
            bool live = j.value("live", false);
            int viewers = j.value("viewers", 0);
            state.set_youtube_live(live);
            state.set_youtube_viewers(viewers);
            if (j.contains("followers")) state.set_youtube_followers(j.value("followers", 0));
            uiPing();
        } else if (type == "youtube.viewers") {
            state.set_youtube_viewers(j.value("viewers", 0));
            uiPing();
        }
    });

    if (log) {
        log(ok ? L"YouTube sidecar started/restarted." :
                 L"ERROR: Could not start YouTube sidecar. Check Python + deps.");
    }
    return ok;
}

bool StartOrRestartTwitchIrc(
    TwitchIrcWsClient& twitch,
    AppState& /*state*/,
    ChatAggregator& chat,
    const std::string& twitchLogin,
    LogFn log)
{
    std::string cleaned = SanitizeTwitchLogin(twitchLogin);
    if (cleaned.empty()) {
        if (log) log(L"TWITCH: channel is empty.");
        return false;
    }

    twitch.stop();

    // Anonymous "justinfan" nick: avoids auth for reading public chat.
    std::string nick = "justinfan" + std::to_string(10000 + (GetTickCount() % 50000));

    bool ok = twitch.start(
        "SCHMOOPIIE",
        nick,
        cleaned,
        chat);

    if (log) {
        log(ok ? L"TWITCH: started/restarted IRC client." :
                 L"TWITCH: failed to start IRC client (already running or invalid parameters).");
    }
    return ok;
}

void StopTikTok(TikTokSidecar& tiktok, AppState& state, HWND hwndMain, UINT uiMsg, LogFn log) {
    tiktok.stop();
    state.set_tiktok_live(false);
    state.set_tiktok_viewers(0);
    if (hwndMain) PostMessageW(hwndMain, uiMsg, 0, 0);
    if (log) log(L"TIKTOK: stopped.");
}
void StopYouTube(TikTokSidecar& youtube, AppState& state, HWND hwndMain, UINT uiMsg, LogFn log) {
    youtube.stop();
    state.set_youtube_live(false);
    state.set_youtube_viewers(0);
    if (hwndMain) PostMessageW(hwndMain, uiMsg, 0, 0);
    if (log) log(L"YOUTUBE: stopped.");
}
void StopTwitch(TwitchIrcWsClient& twitch, AppState& state, HWND hwndMain, UINT uiMsg, LogFn log) {
    twitch.stop();
    state.set_twitch_live(false);
    state.set_twitch_viewers(0);
    if (hwndMain) PostMessageW(hwndMain, uiMsg, 0, 0);
    if (log) log(L"TWITCH: stopped.");
}

} // namespace PlatformControl