#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <windef.h>

class AppState;
class ChatAggregator;
class TikTokSidecar;
class TwitchIrcWsClient;
class TwitchEventSubWsClient;
class TwitchAuth;
class YouTubeLiveChatService;
class HttpServer;
namespace fenixsim { class FenixFailureCoordinator; }

namespace AppShutdown {

struct Dependencies {
    std::unique_ptr<HttpServer>& httpServer;

    std::thread& metricsThread;
    std::thread& twitchHelixThread;
    std::thread& tiktokFollowersThread;

    TwitchEventSubWsClient& twitchEventSub;
    TwitchAuth& twitchAuth;
    TwitchIrcWsClient& twitch;
    YouTubeLiveChatService& youtubeChat;
    TikTokSidecar& youtube;
    TikTokSidecar& tiktok;
    fenixsim::FenixFailureCoordinator& fenixFailureCoordinator;

    std::atomic<bool>& running;
    std::atomic<bool>& twitchHelixRunning;
};

void BeginShutdown(Dependencies& deps, HWND hwndToDestroy);

} // namespace AppShutdown