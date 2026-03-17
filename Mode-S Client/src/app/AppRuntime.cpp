#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "app/AppRuntime.h"

AppBootstrap::Dependencies AppRuntime::BuildBootstrapDeps(HWND hwnd)
{
    return AppBootstrap::Dependencies{
        hwnd,
        config,
        state,
        chat,
        tiktok,
        youtube,
        twitch,
        twitchEventSub,
        twitchAuth,
        youtubeAuth,
        youtubeChat,
        http,
        metricsThread,
        twitchHelixThread,
        tiktokFollowersThread,
        euroscope,
        obs,
        running,
        twitchHelixRunning,
        twitchHelixBoundLogin
    };
}

AppShutdown::Dependencies AppRuntime::BuildShutdownDeps()
{
    return AppShutdown::Dependencies{
        http,
        metricsThread,
        twitchHelixThread,
        tiktokFollowersThread,
        twitchEventSub,
        twitchAuth,
        twitch,
        youtubeChat,
        youtube,
        tiktok,
        running,
        twitchHelixRunning
    };
}
