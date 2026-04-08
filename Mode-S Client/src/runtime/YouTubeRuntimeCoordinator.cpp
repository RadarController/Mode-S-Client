#include "runtime/YouTubeRuntimeCoordinator.h"

#include "log/UiLog.h"
#include "youtube/YouTubeAuth.h"

namespace runtime {

void StartYouTubeRuntimeServices(YouTubeAuth& youtubeAuth)
{
    if (!youtubeAuth.Start()) {
        LogLine(L"YOUTUBE: OAuth token refresh/start failed (check config: youtube.client_id / youtube.client_secret / youtube.refresh_token)");
    }
    else {
        LogLine(L"YOUTUBE: OAuth token refresh/start OK");
    }
}

} // namespace runtime
