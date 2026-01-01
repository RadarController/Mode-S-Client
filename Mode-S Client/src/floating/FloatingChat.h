#pragma once

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <atomic>

// Minimal result type for internal HTTP polling
struct FC_HttpResult {
    int status = 0;
    unsigned long winerr = 0;
    std::string body;
};

class FloatingChat {
public:
    FloatingChat();
    ~FloatingChat();

    // Opens or focuses the floating chat window. Parent is used to position the window.
    bool Open(HWND parent);
    void Close();
    bool IsOpen() const;

private:
    // Non-copyable
    FloatingChat(const FloatingChat&) = delete;
    FloatingChat& operator=(const FloatingChat&) = delete;

    // Window proc and helpers
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void PollLoop();
    FC_HttpResult WinHttpRequest(const std::wstring& method,
        const std::wstring& host,
        INTERNET_PORT port,
        const std::wstring& path,
        const std::wstring& headers,
        const std::string& body,
        bool secure);

private:
    HWND wnd_ = nullptr;
    HWND edit_ = nullptr;
    std::thread pollThread_;
    std::atomic<bool> running_{ false };
    static std::atomic<bool> registered_;
};
