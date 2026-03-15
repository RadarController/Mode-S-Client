#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "app/AppBootstrap.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "AppConfig.h"
#include "AppState.h"
#include "chat/ChatAggregator.h"
#include "core/StringUtil.h"
#include "euroscope/EuroScopeIngestService.h"
#include "floating/FloatingChat.h"
#include "http/HttpServer.h"
#include "log/UiLog.h"
#include "obs/ObsWsClient.h"
#include "platform/PlatformControl.h"
#include "tiktok/TikTokFollowersService.h"
#include "tiktok/TikTokSidecar.h"
#include "twitch/TwitchAuth.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchHelixService.h"
#include "twitch/TwitchIrcWsClient.h"
#include "ui/WebViewHost.h"
#include "youtube/YouTubeAuth.h"
#include "youtube/YouTubeLiveChatService.h"

namespace {

std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    const auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

void SubscribeBotCommandHandler(AppBootstrap::Dependencies& deps)
{
    static bool botSubscribed = false;
    if (botSubscribed) return;
    botSubscribed = true;

    deps.chat.Subscribe([
        pChat = &deps.chat,
        pState = &deps.state,
        pTwitch = &deps.twitch,
        pTikTok = &deps.tiktok
    ](const ChatMessage& m) {
        if (m.user == "StreamingATC.Bot") return;
        if (m.message.size() < 2 || m.message[0] != '!') return;

        auto to_lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        auto replace_all = [](std::string s, const std::string& from, const std::string& to) {
            if (from.empty()) return s;
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
        };

        size_t start = 1;
        while (start < m.message.size() && std::isspace(static_cast<unsigned char>(m.message[start]))) start++;
        size_t end = start;
        while (end < m.message.size() && !std::isspace(static_cast<unsigned char>(m.message[end]))) end++;
        if (end <= start) return;

        std::string cmd_lc = to_lower(m.message.substr(start, end - start));
        if (cmd_lc.empty()) return;

        const long long now_ms_ll = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

        const AppState::BotSettings bot_settings = pState->bot_settings_snapshot();
        if (bot_settings.silent_mode) {
            return;
        }

        static std::mutex rl_mu;
        static std::unordered_map<std::string, long long> last_by_user;
        static std::unordered_map<std::string, long long> last_by_platform;

        const long long kUserGapMs = static_cast<long long>(bot_settings.per_user_gap_ms);
        const long long kPlatformGapMs = static_cast<long long>(bot_settings.per_platform_gap_ms);

        std::string platform_lc = to_lower(m.platform);
        std::string user_key = platform_lc + "|" + m.user;

        {
            std::lock_guard<std::mutex> lk(rl_mu);
            if (kPlatformGapMs > 0) {
                auto itp = last_by_platform.find(platform_lc);
                if (itp != last_by_platform.end() && (now_ms_ll - itp->second) < kPlatformGapMs) {
                    return;
                }
            }
            if (kUserGapMs > 0) {
                auto itu = last_by_user.find(user_key);
                if (itu != last_by_user.end() && (now_ms_ll - itu->second) < kUserGapMs) {
                    return;
                }
            }
            last_by_platform[platform_lc] = now_ms_ll;
            last_by_user[user_key] = now_ms_ll;
        }

        std::string template_reply = pState->bot_try_get_response(
            cmd_lc,
            m.is_mod,
            m.is_broadcaster,
            now_ms_ll);
        if (template_reply.empty()) return;

        std::string reply = template_reply;
        reply = replace_all(reply, "{user}", m.user);
        reply = replace_all(reply, "{platform}", platform_lc);

        const size_t kMaxReplyLen = bot_settings.max_reply_len;
        if (kMaxReplyLen > 0 && reply.size() > kMaxReplyLen) {
            reply.resize(kMaxReplyLen);
        }
        if (kMaxReplyLen == 0) {
            return;
        }

        ChatMessage bot{};
        bot.platform = m.platform;
        bot.user = "StreamingATC.Bot";
        bot.message = reply;
        bot.ts_ms = static_cast<uint64_t>(now_ms_ll + 1);
        pChat->Add(std::move(bot));

        if (platform_lc == "twitch" && pTwitch) {
            if (!pTwitch->SendPrivMsg(reply)) {
                LogLine(L"BOT: Twitch send failed");
            }
        }
        if (platform_lc == "tiktok" && pTikTok) {
            if (!pTikTok->send_chat(reply)) {
                LogLine(L"BOT: TikTok send failed (sidecar)");
            }
        }
    });
}

} // namespace

namespace AppBootstrap {

void InitializeUiAndState(
    Dependencies& deps,
    const wchar_t* modernUiUrl,
    const wchar_t* appVersion,
    const std::function<void(HWND)>& onWebViewOpened)
{
    (void)deps.config.Load();

    try {
        const std::filesystem::path botPath = std::filesystem::path(GetExeDir()) / "bot_commands.json";
        deps.state.set_bot_commands_storage_path(ToUtf8(botPath.wstring()));
        if (deps.state.load_bot_commands_from_disk()) {
            LogLine(L"BOT: loaded commands from bot_commands.json");
        }
        else {
            LogLine(L"BOT: no bot_commands.json found (or empty/invalid) - starting with in-memory defaults");
        }
    }
    catch (...) {
        LogLine(L"BOT: failed to set/load bot commands storage path");
    }

    try {
        const std::filesystem::path setPath = std::filesystem::path(GetExeDir()) / "bot_settings.json";
        deps.state.set_bot_settings_storage_path(ToUtf8(setPath.wstring()));
        if (deps.state.load_bot_settings_from_disk()) {
            LogLine(L"BOT: loaded settings from bot_settings.json");
        }
        else {
            LogLine(L"BOT: no bot_settings.json found (or empty/invalid) - using defaults");
        }
    }
    catch (...) {
        LogLine(L"BOT: failed to set/load bot settings storage path");
    }

    try {
        const std::filesystem::path hdrPath = std::filesystem::path(GetExeDir()) / "overlay_header.json";
        deps.state.set_overlay_header_storage_path(ToUtf8(hdrPath.wstring()));
        if (deps.state.load_overlay_header_from_disk()) {
            LogLine(L"OVERLAY: loaded header settings from overlay_header.json");
        }
        else {
            LogLine(L"OVERLAY: no overlay_header.json found (or empty/invalid) - using defaults");
        }
    }
    catch (...) {
        LogLine(L"OVERLAY: failed to set/load overlay header settings path");
    }

    UiLog_SetWebLogState(&deps.state);
    SubscribeBotCommandHandler(deps);

    {
        std::wstring snap = L"CONFIG: AppConfig snapshot ";
        snap += L"twitch_login='";
        snap += ToW(deps.config.twitch_login);
        snap += L"' twitch_client_id='";
        snap += ToW(deps.config.twitch_client_id);
        snap += L"' twitch_client_secret_len=";
        snap += std::to_wstring(deps.config.twitch_client_secret.size());
        LogLine(snap.c_str());
    }

    HWND hLog = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
        0, 0, 0, 0,
        deps.hwnd,
        nullptr,
        nullptr,
        nullptr);
    UiLog_SetLogHwnd(hLog);

    WebViewHost::Create(
        deps.hwnd,
        modernUiUrl,
        appVersion,
        onWebViewOpened);

    LogLine(L"Starting Mode-S Client overlay");
    LogLine(L"Overlay: http://localhost:17845/overlay/chat.html");
    LogLine(L"Metrics: http://localhost:17845/api/metrics");

    if (deps.config.tiktok_unique_id.empty() && deps.config.twitch_login.empty() && deps.config.youtube_handle.empty()) {
        LogLine(L"No config.json found yet. Configure your platform details in the Settings page.");
    }
    else {
        LogLine(L"Loaded config.json");
    }
}

void StartBackend(
    Dependencies& deps,
    const wchar_t* modernUiUrl,
    const std::function<void(const std::string&)>& restartTwitchHelixPoller)
{
    // IMPORTANT:
    // deps is only a short-lived wrapper passed in from WndProc.
    // Any async work or stored callbacks must capture the underlying
    // long-lived objects, NOT &deps itself.

    auto& config = deps.config;
    auto& state = deps.state;
    auto& chat = deps.chat;
    auto& tiktok = deps.tiktok;
    auto& youtube = deps.youtube;
    auto& twitch = deps.twitch;
    auto& twitchEventSub = deps.twitchEventSub;
    auto& twitchAuth = deps.twitchAuth;
    auto& youtubeAuth = deps.youtubeAuth;
    auto& youtubeChat = deps.youtubeChat;
    auto& httpServer = deps.httpServer;
    auto& metricsThread = deps.metricsThread;
    auto& tiktokFollowersThread = deps.tiktokFollowersThread;
    auto& euroscope = deps.euroscope;
    auto& obs = deps.obs;
    auto& running = deps.running;
    auto& twitchHelixBoundLogin = deps.twitchHelixBoundLogin;

    HttpServer::Options opt;
    opt.bind_host = "127.0.0.1";
    opt.port = 17845;
    opt.overlay_root = std::filesystem::path(GetExeDir()) / "assets" / "overlay";

    opt.start_tiktok = [&config, &tiktok, &state, &chat]() -> bool {
        config.tiktok_unique_id = SanitizeTikTok(config.tiktok_unique_id);
        if (config.tiktok_unique_id.empty()) {
            LogLine(L"TIKTOK: username is empty - refusing to start");
            return false;
        }

        return PlatformControl::StartOrRestartTikTokSidecar(
            tiktok,
            state,
            chat,
            GetExeDir(),
            config.tiktok_unique_id,
            [](const std::wstring& s) { LogLine(s); });
        };

    opt.stop_tiktok = [&tiktok, &state]() -> bool {
        PlatformControl::StopTikTok(tiktok, state, [](const std::wstring& s) { LogLine(s); });
        return true;
        };

    opt.start_twitch = [&config, &twitchAuth, &twitch, &state, &chat, &twitchEventSub, &restartTwitchHelixPoller, &twitchHelixBoundLogin]() -> bool {
        config.twitch_login = SanitizeTwitchLogin(config.twitch_login);
        if (config.twitch_login.empty()) {
            LogLine(L"TWITCH: channel login is empty - refusing to start");
            return false;
        }

        const std::string token = twitchAuth.GetAccessToken().value_or("");
        if (token.empty()) {
            LogLine(L"TWITCH: token not available yet (auth not ready) - refusing to start");
            return false;
        }

        if (twitchHelixBoundLogin != config.twitch_login) {
            restartTwitchHelixPoller("web dashboard start");
        }

        const bool ok = PlatformControl::StartOrRestartTwitchIrc(
            twitch,
            state,
            chat,
            config.twitch_login,
            token,
            [](const std::wstring& s) { LogLine(s); });

        if (!ok) return false;

        twitchEventSub.Start(
            config.twitch_client_id,
            token,
            config.twitch_login,
            [&](const ChatMessage& msg) {
                ChatMessage c = msg;
                c.is_event = true;
                chat.Add(std::move(c));
            },
            [&](const nlohmann::json& ev) { state.add_twitch_eventsub_event(ev); },
            [&](const nlohmann::json& st) {
                state.set_twitch_eventsub_status(st);

                static std::uint64_t last_seq = 0;
                const std::uint64_t seq = st.value("last_error_seq", static_cast<std::uint64_t>(0));
                if (seq != 0 && seq != last_seq) {
                    last_seq = seq;
                    const std::string msg = st.value("last_error", std::string{});
                    if (!msg.empty()) state.push_twitch_eventsub_error(msg);
                }
            });

        return true;
        };

    opt.stop_twitch = [&twitch, &twitchEventSub, &state]() -> bool {
        PlatformControl::StopTwitch(
            twitch,
            twitchEventSub,
            state,
            [](const std::wstring& s) { LogLine(s); });
        return true;
        };

    opt.start_youtube = [&config, &youtube, &state, &chat, &youtubeChat]() -> bool {
        config.youtube_handle = SanitizeYouTubeHandle(config.youtube_handle);
        if (config.youtube_handle.empty()) {
            LogLine(L"YOUTUBE: handle/channel is empty - refusing to start");
            return false;
        }

        const bool ok = PlatformControl::StartOrRestartYouTubeSidecar(
            youtube,
            state,
            chat,
            GetExeDir(),
            config.youtube_handle,
            [](const std::wstring& s) { LogLine(s); });

        if (!ok) return false;

        youtubeChat.start(
            config.youtube_handle,
            chat,
            [](const std::wstring& s) { LogLine(s); },
            &state);

        return true;
        };

    opt.stop_youtube = [&youtube, &state, &youtubeChat]() -> bool {
        PlatformControl::StopYouTube(youtube, state, [](const std::wstring& s) { LogLine(s); });
        youtubeChat.stop();
        return true;
        };

    opt.twitch_auth_build_authorize_url = [&twitchAuth](const std::string& redirect_uri, std::string* out_error) -> std::string {
        std::string err;
        const std::string url = twitchAuth.BuildAuthorizeUrl(redirect_uri, &err);
        if (url.empty()) {
            if (out_error) *out_error = err;
            LogLine(ToW(std::string("TWITCHAUTH: BuildAuthorizeUrl failed: ") + err));
        }
        return url;
        };

    opt.twitch_auth_handle_callback = [&twitchAuth](
        const std::string& code,
        const std::string& state,
        const std::string& redirect_uri,
        std::string* out_error) -> bool {
            std::string err;
            const bool ok = twitchAuth.HandleOAuthCallback(code, state, redirect_uri, &err);
            if (!ok) {
                if (out_error) *out_error = err;
                LogLine(ToW(std::string("TWITCHAUTH: OAuth callback failed: ") + err));
            }
            return ok;
        };

    opt.youtube_auth_build_authorize_url = [&youtubeAuth](const std::string& redirect_uri, std::string* out_error) -> std::string {
        std::string err;
        const std::string url = youtubeAuth.BuildAuthorizeUrl(redirect_uri, &err);
        if (url.empty()) {
            if (out_error) *out_error = err;
            LogLine(ToW(std::string("YTAUTH: BuildAuthorizeUrl failed: ") + err));
        }
        return url;
        };

    opt.youtube_auth_handle_callback = [&youtubeAuth](
        const std::string& code,
        const std::string& state,
        const std::string& redirect_uri,
        std::string* out_error) -> bool {
            std::string err;
            const bool ok = youtubeAuth.HandleOAuthCallback(code, state, redirect_uri, &err);
            if (!ok) {
                if (out_error) *out_error = err;
                LogLine(ToW(std::string("YTAUTH: OAuth callback failed: ") + err));
            }
            return ok;
        };

    opt.youtube_get_access_token = [&youtubeAuth]() -> std::optional<std::string> {
        return youtubeAuth.GetAccessToken();
        };

    opt.youtube_auth_info_json = [&youtubeAuth]() {
        nlohmann::json j;
        j["ok"] = true;
        j["start_url"] = "/auth/youtube/start";
        j["oauth_routes_wired"] = true;

        const auto snap = youtubeAuth.GetTokenSnapshot();
        j["has_refresh_token"] = snap.has_value() && !snap->refresh_token.empty();
        j["has_access_token"] = snap.has_value() && !snap->access_token.empty();
        j["expires_at_unix"] = snap.has_value() ? snap->expires_at_unix : 0;
        j["scope"] = snap.has_value() ? snap->scope_joined : "";
        j["channel_id"] = youtubeAuth.GetChannelId().value_or("");
        j["scopes_readable"] = std::string(YouTubeAuth::RequiredScopeReadable());
        j["scopes_encoded"] = std::string(YouTubeAuth::RequiredScopeEncoded());
        return j.dump(2);
        };

    httpServer = std::make_unique<HttpServer>(
        state,
        chat,
        euroscope,
        config,
        opt,
        [](const std::wstring& s) { LogLine(s); });
    httpServer->Start();

    WebViewHost::SetHttpReadyAndNavigate(modernUiUrl);

    metricsThread = std::thread([&state, &obs, &running]() {
        while (running) {
            const auto m = state.get_metrics();
            obs.set_text("TOTAL_VIEWER_COUNT", std::to_string(m.total_viewers()));
            obs.set_text("TOTAL_FOLLOWER_COUNT", std::to_string(m.total_followers()));
            Sleep(5000);
        }
        });

    if (!youtubeAuth.Start()) {
        LogLine(L"YOUTUBE: OAuth token refresh/start failed (check config: youtube.client_id / youtube.client_secret / youtube.refresh_token)");
    }
    else {
        LogLine(L"YOUTUBE: OAuth token refresh/start OK");
    }

    LogLine(L"TWITCH: starting Helix poller thread");
    restartTwitchHelixPoller("init");

    LogLine(L"TIKTOK: starting followers poller thread");
    tiktokFollowersThread = StartTikTokFollowersPoller(
        config,
        state,
        running,
        TikTokFollowersUiCallbacks{
            [](const std::wstring& s) { LogLine(s); },
            [](const std::wstring&) {},
            [&](int) {}
        });

    LogLine(L"TWITCH: refreshing OAuth token...");
    twitchAuth.on_tokens_updated = [&config, &twitchEventSub, &twitch, &state, &chat](
        const std::string& access,
        const std::string& /*refresh*/,
        const std::string& login) {
            const std::string effective_login = !login.empty() ? login : config.twitch_login;
            LogLine(L"TWITCH: tokens updated - refreshing EventSub token and restarting IRC");

            config.twitch_login = effective_login;
            twitchEventSub.UpdateAccessToken(access);

            PlatformControl::StartOrRestartTwitchIrc(
                twitch,
                state,
                chat,
                effective_login,
                access,
                [](const std::wstring& s) { LogLine(s); });
        };

    if (!twitchAuth.Start()) {
        LogLine(L"TWITCH: OAuth token refresh/start failed (check config: twitch_client_id / twitch_client_secret / twitch.user_refresh_token)");
    }
    else {
        LogLine(L"TWITCH: OAuth token refresh worker started");
    }
}

} // namespace AppBootstrap
