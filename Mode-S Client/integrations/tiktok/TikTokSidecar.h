#pragma once
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <windows.h>
#include "json.hpp"

class TikTokSidecar {
public:
    using EventHandler = std::function<void(const nlohmann::json&)>;

    TikTokSidecar() = default;
    ~TikTokSidecar();

    bool start(const std::wstring& pythonExe,
        const std::wstring& scriptPath,
        EventHandler onEvent);

    void stop();

    // Step 2: ask the python sidecar to send a message into TikTok LIVE chat.
    // Returns false if the sidecar isn't running or if the write fails.
    bool send_chat(const std::string& text);

private:
    void reader_loop();

    PROCESS_INFORMATION pi_{};

    // Python stdout -> C++ reads (events)
    HANDLE hStdOutRd_{ nullptr };
    HANDLE hStdOutWr_{ nullptr };

    // C++ -> Python stdin (commands, e.g. send_chat)
    HANDLE hStdInRd_{ nullptr };
    HANDLE hStdInWr_{ nullptr };
    std::mutex stdin_mu_;

    std::thread reader_;
    std::atomic<bool> running_{ false };
    EventHandler onEvent_;
};