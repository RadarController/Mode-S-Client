#pragma once

#include <atomic>
#include <thread>

class AppState;
class ObsWsClient;

namespace runtime {

void StartObsMetricsPublisher(
    std::thread& metricsThread,
    AppState& state,
    ObsWsClient& obs,
    std::atomic<bool>& running);

} // namespace runtime
