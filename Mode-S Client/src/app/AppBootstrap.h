#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct AppConfig;
class AppState;
class ChatAggregator;
class TikTokSidecar;
class TwitchIrcWsClient;
class TwitchEventSubWsClient;
class TwitchAuth;
class YouTubeAuth;
class YouTubeLiveChatService;
class HttpServer;
class EuroScopeIngestService;
class ObsWsClient;
namespace fenixsim { class FenixSimFailuresClient; class FenixFailureCoordinator; }

namespace AppBootstrap {

struct Dependencies {
    HWND hwnd = nullptr;

    AppConfig& config;
    AppState& state;
    ChatAggregator& chat;
    TikTokSidecar& tiktok;
    TikTokSidecar& youtube;
    TwitchIrcWsClient& twitch;
    TwitchEventSubWsClient& twitchEventSub;
    TwitchAuth& twitchAuth;
    YouTubeAuth& youtubeAuth;
    YouTubeLiveChatService& youtubeChat;
    std::unique_ptr<HttpServer>& httpServer;
    std::thread& metricsThread;
    std::thread& twitchHelixThread;
    std::thread& tiktokFollowersThread;
    EuroScopeIngestService& euroscope;
    ObsWsClient& obs;
    fenixsim::FenixSimFailuresClient& fenixFailures;
    fenixsim::FenixFailureCoordinator& fenixFailureCoordinator;
    std::atomic<bool>& running;
    std::atomic<bool>& twitchHelixRunning;
    std::string& twitchHelixBoundLogin;
};

void InitializeUiAndState(
    Dependencies& deps,
    const wchar_t* modernUiUrl,
    const wchar_t* appVersion,
    const std::function<void(HWND)>& onWebViewOpened);

void StartBackend(
    Dependencies& deps,
    const wchar_t* modernUiUrl,
    const std::function<void(const std::string&)>& restartTwitchHelixPoller);

} // namespace AppBootstrap
