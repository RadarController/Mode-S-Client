#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>

#include "app/AppShutdown.h"

#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchAuth.h"
#include "twitch/TwitchIrcWsClient.h"
#include "youtube/YouTubeLiveChatService.h"
#include "tiktok/TikTokSidecar.h"
#include "http/HttpServer.h"
#include "log/UiLog.h"

namespace AppShutdown {

void BeginShutdown(Dependencies& deps, HWND hwndToDestroy)
{
    static std::atomic<bool> shuttingDown{ false };
    if (shuttingDown.exchange(true)) return;

    LogLine(L"SHUTDOWN: BeginShutdown()");

    // 1) Flip flags so loops exit
    deps.running = false;
    deps.twitchHelixRunning = false;
    LogLine(L"SHUTDOWN: flags set");

    // 2) Stop HTTP early
    if (deps.httpServer) {
        LogLine(L"SHUTDOWN: stopping HTTP");
        deps.httpServer->Stop();
        deps.httpServer.reset();
        LogLine(L"SHUTDOWN: HTTP stopped");
    }

    // 3) Join threads next
    if (deps.tiktokFollowersThread.joinable()) {
        LogLine(L"SHUTDOWN: join tiktokFollowersThread...");
        deps.tiktokFollowersThread.join();
        LogLine(L"SHUTDOWN: joined tiktokFollowersThread");
    }

    if (deps.twitchHelixThread.joinable()) {
        LogLine(L"SHUTDOWN: join twitchHelixThread...");
        deps.twitchHelixThread.join();
        LogLine(L"SHUTDOWN: joined twitchHelixThread");
    }

    if (deps.metricsThread.joinable()) {
        LogLine(L"SHUTDOWN: join metricsThread...");
        deps.metricsThread.join();
        LogLine(L"SHUTDOWN: joined metricsThread");
    }

    // 4) Stop services last
    LogLine(L"SHUTDOWN: stopping services...");

    LogLine(L"SHUTDOWN: stopping twitchEventSub...");
    try { deps.twitchEventSub.Stop(); }
    catch (...) {}
    LogLine(L"SHUTDOWN: stopped twitchEventSub");

    LogLine(L"SHUTDOWN: stopping twitchAuth...");
    try { deps.twitchAuth.Stop(); }
    catch (...) {}
    LogLine(L"SHUTDOWN: stopped twitchAuth");

    LogLine(L"SHUTDOWN: stopping twitch...");
    try { deps.twitch.stop(); }
    catch (...) {}
    LogLine(L"SHUTDOWN: stopped twitch");

    LogLine(L"SHUTDOWN: stopping youtubeChat...");
    try { deps.youtubeChat.stop(); }
    catch (...) {}
    LogLine(L"SHUTDOWN: stopped youtubeChat");

    LogLine(L"SHUTDOWN: stopping youtube...");
    try { deps.youtube.stop(); }
    catch (...) {}
    LogLine(L"SHUTDOWN: stopped youtube");

    LogLine(L"SHUTDOWN: stopping tiktok...");
    try { deps.tiktok.stop(); }
    catch (...) {}
    LogLine(L"SHUTDOWN: stopped tiktok");

    LogLine(L"SHUTDOWN: services stopped");

    // 5) Destroy window to reach WM_DESTROY -> PostQuitMessage
    if (hwndToDestroy && IsWindow(hwndToDestroy)) {
        LogLine(L"SHUTDOWN: destroying window");
        DestroyWindow(hwndToDestroy);
    }
}

} // namespace AppShutdown