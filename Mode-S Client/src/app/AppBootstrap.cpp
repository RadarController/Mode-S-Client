#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/AppBootstrap.h"

#include <filesystem>
#include <fstream>

#include "AppConfig.h"
#include "oauth/EmbeddedOAuthConfig.h"
#include "AppState.h"
#include "chat/ChatAggregator.h"
#include "core/StringUtil.h"
#include "euroscope/EuroScopeIngestService.h"
#include "http/HttpServer.h"
#include "http/HttpServerOptionsBuilder.h"
#include "log/UiLog.h"
#include "bot/BotCommandDispatcher.h"
#include "bot/BotStorageBootstrap.h"
#include "obs/ObsWsClient.h"
#include "runtime/ObsMetricsPublisher.h"
#include "runtime/TikTokRuntimeCoordinator.h"
#include "runtime/TwitchRuntimeCoordinator.h"
#include "runtime/YouTubeRuntimeCoordinator.h"
#include "twitch/TwitchAuth.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchIrcWsClient.h"
#include "ui/WebViewHost.h"
#include "youtube/YouTubeAuth.h"
#include "youtube/YouTubeLiveChatService.h"
#include "overlay/OverlayHeaderStorage.h"
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

    bot::InitializeBotStorage(deps.state, GetExeDir());
    overlay::InitializeOverlayHeaderStorage(deps.state, GetExeDir());

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
    auto& httpServer = deps.httpServer;
    auto& euroscope = deps.euroscope;
    auto& fenixFailures = deps.fenixFailures;
    auto& fenixFailureCoordinator = deps.fenixFailureCoordinator;

    HttpServer::Options opt = httpoptions::BuildHttpServerOptions(
        deps,
        GetExeDir(),
        restartTwitchHelixPoller);

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

    runtime::StartObsMetricsPublisher(
        deps.metricsThread,
        deps.state,
        deps.obs,
        deps.running);

    runtime::StartYouTubeRuntimeServices(deps.youtubeAuth, deps.config, deps.state);

    runtime::StartTwitchRuntimeServices(
        restartTwitchHelixPoller,
        deps.twitchAuth,
        deps.config,
        deps.twitchEventSub,
        deps.twitch,
        deps.state,
        deps.chat);

    runtime::StartTikTokRuntimeServices(
        deps.tiktokFollowersThread,
        deps.config,
        deps.state,
        deps.running);
}

} // namespace AppBootstrap
