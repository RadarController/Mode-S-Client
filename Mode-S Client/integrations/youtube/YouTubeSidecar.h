#pragma once
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <windows.h>
#include "json.hpp"

class YouTubeSidecar {
public:
    using EventHandler = std::function<void(const nlohmann::json&)>;

    YouTubeSidecar() = default;
    ~YouTubeSidecar();

    bool start(const std::wstring& pythonExe,
        const std::wstring& scriptPath,
        EventHandler onEvent);

    // Starts sidecar and passes a config.json path as argv[1]
    bool start(const std::wstring& pythonExe,
        const std::wstring& scriptPath,
        const std::wstring& configPath,
        EventHandler onEvent);

    void stop();

private:
    void reader_loop();

    PROCESS_INFORMATION pi_{};
    HANDLE hStdOutRd_{ nullptr };
    HANDLE hStdOutWr_{ nullptr };
    std::thread reader_;
    std::atomic<bool> running_{ false };
    EventHandler onEvent_;
};
