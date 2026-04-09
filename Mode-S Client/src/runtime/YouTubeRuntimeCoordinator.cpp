#include "runtime/YouTubeRuntimeCoordinator.h"

#include "AppConfig.h"
#include "AppState.h"
#include "log/UiLog.h"
#include "youtube/YouTubeAuth.h"
#include "youtube/YouTubeChannelStatsService.h"
#include "youtube/YouTubeLiveStatusService.h"

namespace runtime {
namespace {

YouTubeAuth* g_youtubeAuth = nullptr;
youtube::YouTubeChannelStatsService g_youtubeChannelStatsService;
youtube::YouTubeLiveStatusService g_youtubeLiveStatusService;

void LogRuntime(const std::wstring& msg)
{
    LogLine(msg.c_str());
}

} // namespace

void StartYouTubeRuntimeServices(YouTubeAuth& youtubeAuth, AppConfig& config, AppState& state)
{
    g_youtubeAuth = &youtubeAuth;

    if (!youtubeAuth.Start()) {
        LogLine(L"YOUTUBE: OAuth token refresh/start failed (check config: youtube.client_id / youtube.client_secret / youtube.refresh_token)");
    }
    else {
        LogLine(L"YOUTUBE: OAuth token refresh/start OK");
    }

    if (!g_youtubeChannelStatsService.Start(youtubeAuth, state, LogRuntime)) {
        LogLine(L"YOUTUBE: channel stats service failed to start");
    }
    else {
        LogLine(L"YOUTUBE: channel stats service started");
    }

    if (!g_youtubeLiveStatusService.Start(
        [&config]() { return config.youtube_handle; },
        state,
        LogRuntime)) {
        LogLine(L"YOUTUBE: live status service failed to start");
    }
    else {
        LogLine(L"YOUTUBE: live status service started");
    }
}

void StopYouTubeRuntimeServices()
{
    g_youtubeLiveStatusService.Stop();
    g_youtubeChannelStatsService.Stop();

    if (g_youtubeAuth) {
        try {
            g_youtubeAuth->Stop();
        }
        catch (...) {
        }
        g_youtubeAuth = nullptr;
    }
}

} // namespace runtime
