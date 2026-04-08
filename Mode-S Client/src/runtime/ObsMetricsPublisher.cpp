#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "runtime/ObsMetricsPublisher.h"

#include <string>

#include "AppState.h"
#include "obs/ObsWsClient.h"

namespace runtime {

void StartObsMetricsPublisher(
    std::thread& metricsThread,
    AppState& state,
    ObsWsClient& obs,
    std::atomic<bool>& running)
{
    metricsThread = std::thread([&state, &obs, &running]() {
        while (running) {
            const auto m = state.get_metrics();
            obs.set_text("TOTAL_VIEWER_COUNT", std::to_string(m.total_viewers()));
            obs.set_text("TOTAL_FOLLOWER_COUNT", std::to_string(m.total_followers()));
            Sleep(5000);
        }
    });
}

} // namespace runtime
