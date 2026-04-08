#pragma once

#include <atomic>
#include <thread>

struct AppConfig;
class AppState;

namespace runtime {

void StartTikTokRuntimeServices(
    std::thread& tiktokFollowersThread,
    AppConfig& config,
    AppState& state,
    std::atomic<bool>& running);

} // namespace runtime
