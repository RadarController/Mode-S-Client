#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/AppBootstrap.h"

#include <filesystem>
#include <fstream>

#include "AppConfig.h"
#include "json.hpp"
#include "oauth/EmbeddedOAuthConfig.h"
#include "AppState.h"
#include "chat/ChatAggregator.h"
#include "core/StringUtil.h"
#include "euroscope/EuroScopeIngestService.h"
#include "floating/FloatingChat.h"
#include "http/HttpServer.h"
#include "log/UiLog.h"
#include "bot/BotCommandDispatcher.h"
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
#include "fenixsim/FenixFailureCoordinator.h"

namespace {

std::wstring GetExeDir()

{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    const auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
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
    deps.youtubeChat.SetReplyAuth(&deps.youtubeAuth);
    bot::SubscribeBotCommandHandler(
        deps.chat,
        deps.state,
        deps.twitch,
        deps.tiktok,
        deps.youtubeChat);

    {
        std::wstring snap = L"CONFIG: AppConfig snapshot ";
        snap += L"twitch_login='";
        snap += ToW(deps.config.twitch_login);
        snap += L"' embedded_twitch_credentials=";
        snap += EmbeddedOAuthConfig::HasTwitchCredentials() ? L"yes" : L"no";
        snap += L" config_twitch_client_id_present=";
        snap += deps.config.twitch_client_id.empty() ? L"no" : L"yes";
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
    auto& fenixFailures = deps.fenixFailures;
    auto& fenixFailureCoordinator = deps.fenixFailureCoordinator;
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
            if (restartTwitchHelixPoller) {
                restartTwitchHelixPoller("web dashboard start");
            }
            else {
                LogLine(L"TWITCH: Helix poller restart callback is not wired.");
            }
        }

        const bool ok = PlatformControl::StartOrRestartTwitchIrc(
            twitch,
            state,
            chat,
            config.twitch_login,
            token,
            [](const std::wstring& s) { LogLine(s); });

        if (!ok) return false;

        std::string eventSubClientId = config.twitch_client_id;
        if (eventSubClientId.empty()) {
            eventSubClientId = EmbeddedOAuthConfig::TwitchClientId();
        }

        if (eventSubClientId.empty()) {
            LogLine(L"TWITCH: EventSub not started - missing embedded Twitch client id");
            return true;
        }

        twitchEventSub.Start(
            eventSubClientId,
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

    opt.twitch_get_access_token = [&twitchAuth]() -> std::optional<std::string> {
        return twitchAuth.GetAccessToken();
        };

    opt.twitch_get_client_id = []() -> std::optional<std::string> {
        const std::string client_id = EmbeddedOAuthConfig::TwitchClientId();
        if (client_id.empty()) return std::nullopt;
        return client_id;
        };

    opt.youtube_get_access_token = [&youtubeAuth]() -> std::optional<std::string> {
        return youtubeAuth.GetAccessToken();
        };

    opt.youtube_get_channel_id = [&youtubeAuth]() -> std::optional<std::string> {
        return youtubeAuth.GetChannelId();
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

    opt.simulator_automation_status_json = [&fenixFailureCoordinator]() {
        return fenixFailureCoordinator.StatusJson();
        };

    opt.simulator_automation_enable = [&fenixFailureCoordinator]() -> bool {
        fenixFailureCoordinator.SetEnabled(true);
        LogLine(L"SIMAUTO: simulator automation enabled");
        return true;
        };

    opt.simulator_automation_disable = [&fenixFailureCoordinator]() -> bool {
        fenixFailureCoordinator.SetEnabled(false);
        LogLine(L"SIMAUTO: simulator automation disabled");
        return true;
        };

    opt.simulator_automation_panic = [&fenixFailureCoordinator]() -> bool {
        fenixFailureCoordinator.PanicStop();
        LogLine(L"SIMAUTO: simulator automation panic stop engaged");
        return true;
        };

    fenixFailureCoordinator.Start(
        state,
        fenixFailures,
        [](const std::wstring& s) { LogLine(s); });

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
        LogLine(L"TWITCH: OAuth token refresh/start failed (check embedded Twitch credentials and twitch.user_refresh_token)");
    }
    else {
        LogLine(L"TWITCH: OAuth token refresh worker started");
    }
}

} // namespace AppBootstrap
