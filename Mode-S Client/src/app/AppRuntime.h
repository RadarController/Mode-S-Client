#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

typedef struct HWND__* HWND;

#include "AppConfig.h"
#include "AppState.h"
#include "app/AppBootstrap.h"
#include "app/AppShutdown.h"
#include "http/HttpServer.h"
#include "chat/ChatAggregator.h"
#include "tiktok/TikTokSidecar.h"
#include "twitch/TwitchIrcWsClient.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchAuth.h"
#include "youtube/YouTubeAuth.h"
#include "youtube/YouTubeLiveChatService.h"
#include "euroscope/EuroScopeIngestService.h"
#include "obs/ObsWsClient.h"

struct AppRuntime {
    AppConfig config;
    AppState state;
    ChatAggregator chat;
    TikTokSidecar tiktok;
    TikTokSidecar youtube;
    TwitchIrcWsClient twitch;
    TwitchEventSubWsClient twitchEventSub;
    TwitchAuth twitchAuth;
    YouTubeAuth youtubeAuth;
    YouTubeLiveChatService youtubeChat;
    std::unique_ptr<HttpServer> http;
    std::thread metricsThread;
    std::thread twitchHelixThread;
    std::thread tiktokFollowersThread;
    EuroScopeIngestService euroscope;
    ObsWsClient obs;
    std::atomic<bool> running{ true };
    std::atomic<bool> twitchHelixRunning{ true };
    std::string twitchHelixBoundLogin;

    AppBootstrap::Dependencies BuildBootstrapDeps(HWND hwnd);
    AppShutdown::Dependencies BuildShutdownDeps();
};