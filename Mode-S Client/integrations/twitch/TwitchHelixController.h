#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <windows.h>

struct AppConfig;
class AppState;

namespace TwitchHelixController {

struct Dependencies {
    HWND hwnd;
    AppConfig& config;
    AppState& state;
    std::thread& thread;
    std::atomic<bool>& running;
    std::string& boundLogin;
};

void RestartPoller(Dependencies& deps, const std::string& reason);

} // namespace TwitchHelixController
