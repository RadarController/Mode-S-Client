#include "runtime/TikTokRuntimeCoordinator.h"

#include "AppConfig.h"
#include "AppState.h"
#include "log/UiLog.h"
#include "tiktok/TikTokFollowersService.h"

namespace runtime {

void StartTikTokRuntimeServices(
    std::thread& tiktokFollowersThread,
    AppConfig& config,
    AppState& state,
    std::atomic<bool>& running)
{
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
}

} // namespace runtime
