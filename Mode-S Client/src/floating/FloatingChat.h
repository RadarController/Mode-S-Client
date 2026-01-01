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

// Try to include WebView2 if available
#if defined(__has_include)
#  if __has_include("WebView2.h")
#    include <wrl.h>
#    include "WebView2.h"
#    define HAVE_WEBVIEW2 1
#  else
#    define HAVE_WEBVIEW2 0
#  endif
#else
#  define HAVE_WEBVIEW2 0
#endif

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
    // edit_ kept for compatibility but no longer used when WebView2 is active
    HWND edit_ = nullptr;
    std::thread pollThread_;
    std::atomic<bool> running_{ false };
    static std::atomic<bool> registered_;

#if HAVE_WEBVIEW2
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> webview_controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_window_;
#endif
};
