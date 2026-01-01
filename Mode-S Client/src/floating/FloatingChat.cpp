#include "FloatingChat.h"

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <atomic>
#include <sstream>
#include <vector>
#include <memory>
#include <algorithm>
#include "json.hpp"

#if HAVE_WEBVIEW2
#include <wrl.h>
#include "WebView2.h"
using Microsoft::WRL::ComPtr;
#endif

#pragma comment(lib, "winhttp.lib")

std::atomic<bool> FloatingChat::registered_{ false };

FloatingChat::FloatingChat() = default;
FloatingChat::~FloatingChat()
{
    Close();
}

bool FloatingChat::Open(HWND parent)
{
    if (wnd_) {
        SetForegroundWindow(wnd_);
        return true;
    }

    const wchar_t* kClass = L"StreamHub_FloatingChat";
    if (!registered_.exchange(true)) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProcThunk;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
    }

    RECT pr{}; GetWindowRect(parent, &pr);
    int w = 420, h = 600;

    // Prefer placing the floating chat immediately to the right of the main window.
    int x = pr.right + 10; // 10px gap
    int y = pr.top;

    // Simplified clamping using primary monitor/system metrics to avoid header macro issues.
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // vertical clamp
    if (y + h > screenH) {
        int altY = screenH - h;
        if (altY < 0) altY = 0;
        y = altY;
    }
    if (y < 0) y = 0;

    // horizontal fit: if there's no room on the right, try left of parent, otherwise clamp to screen
    if (x + w > screenW) {
        int alt = pr.left - 10 - w;
        if (alt >= 0) {
            x = alt;
        } else {
            int altX = screenW - w;
            if (altX < 0) altX = 0;
            x = altX;
        }
    }
    if (x < 0) x = 0;

    HWND wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST,
        kClass, L"Floating Chat",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_SYSMENU | WS_THICKFRAME,
        x, y, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!wnd) return false;

    const BYTE alpha = (BYTE)84; // ~33% opaque -> 67% transparent
    SetLayeredWindowAttributes(wnd, 0, alpha, LWA_ALPHA);

    wnd_ = wnd;
    running_ = true;

    // WebView2 initialization will occur in WM_CREATE handler via WndProc.
    return true;
}

void FloatingChat::Close()
{
    if (!wnd_) return;
    // Do NOT tear down WebView2 objects here. Closing a window can generate additional messages
    // (e.g. WM_SIZE) during destruction; releasing the controller here can cause use-after-free
    // if any message handler touches it. We release WebView2 resources in WM_DESTROY.
    running_ = false;
    DestroyWindow(wnd_);
    // wnd_ will be cleared in WM_DESTROY as well, but clear it here too for safety.
    wnd_ = nullptr;
}

bool FloatingChat::IsOpen() const { return wnd_ != nullptr; }

LRESULT CALLBACK FloatingChat::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Associate the window with the FloatingChat instance as early as possible.
    // WM_NCCREATE is the canonical place for GWLP_USERDATA binding.
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        if (cs && cs->lpCreateParams) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        return TRUE;
    }

    auto* self = reinterpret_cast<FloatingChat*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self->WndProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FloatingChat::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
#if HAVE_WEBVIEW2
        // Initialize WebView2 environment and controller
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, hwnd](HRESULT envResult, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(envResult) || !env) return envResult;
                    // Create controller
                    env->CreateCoreWebView2Controller(hwnd,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this, hwnd, env](HRESULT ctlResult, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(ctlResult) || !controller) return ctlResult;
                                // IMPORTANT: do NOT use Attach() here.
                                // The COM callback may Release() its reference after this handler
                                // returns. Assigning to ComPtr AddRef()s the controller so we keep
                                // a valid reference beyond this callback.
                                webview_controller_ = controller;
                                // obtain core webview
                                ICoreWebView2* core = nullptr;
                                webview_controller_->get_CoreWebView2(&core);
                                if (core) {
                                    webview_window_.Attach(core);
                                    // Make background transparent if supported
                                    ICoreWebView2Settings* settings = nullptr;
                                    webview_window_->get_Settings(&settings);
                                    if (settings) {
                                        settings->put_IsStatusBarEnabled(FALSE);
                                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                                        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                                        settings->Release();
                                    }

                                    // Navigate to local overlay chat.html served by local HTTP server
                                    // Navigate to the dedicated floating chat page (separate from /overlay/chat.html)
                                    webview_window_->Navigate(L"http://127.0.0.1:17845/assets/app/chat.html");
                                }

                                // Show controller and set bounds
                                webview_controller_->put_IsVisible(TRUE);
                                RECT rc; GetClientRect(hwnd, &rc);
                                webview_controller_->put_Bounds(rc);
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());
        (void)hr; // ignore; best-effort initialization
#else
        // WebView2 not available: open default browser to the overlay page as fallback
        ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:17845/overlay/chat.html", nullptr, nullptr, SW_SHOW);
#endif
        return 0;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
#if HAVE_WEBVIEW2
        if (webview_controller_) {
            webview_controller_->put_Bounds(rc);
        }
#endif
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
#if HAVE_WEBVIEW2
        if (webview_controller_) {
            webview_controller_->put_IsVisible(FALSE);
            webview_controller_.Reset();
        }
        if (webview_window_) webview_window_.Reset();
#endif
        wnd_ = nullptr;
        running_ = false;
        return 0;
    }
    case WM_NCDESTROY:
        // Critical: prevent any late messages from calling into a stale instance pointer.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Simplified WinHttpRequest kept for compatibility (unused when using WebView2)
FC_HttpResult FloatingChat::WinHttpRequest(const std::wstring& method,
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& headers,
    const std::string& body,
    bool secure)
{
    FC_HttpResult r;
    DWORD status = 0; DWORD statusSize = sizeof(status);
    std::string out;
    HINTERNET hSession = WinHttpOpen(L"Mode-S Client/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { r.winerr = GetLastError(); return r; }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { r.winerr = GetLastError(); WinHttpCloseHandle(hSession); return r; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { r.winerr = GetLastError(); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return r; }

    if (!headers.empty()) WinHttpAddRequestHeaders(hRequest, headers.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0);

    if (!ok) { r.winerr = GetLastError(); goto done; }

    ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) { r.winerr = GetLastError(); goto done; }
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
    {
        r.status = (int)status;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) { r.winerr = GetLastError(); break; }
        if (avail == 0) break;
        size_t cur = out.size();
        out.resize(cur + avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, out.data() + cur, avail, &read)) { r.winerr = GetLastError(); break; }
        out.resize(cur + read);
    }
    r.body = std::move(out);

done:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return r;
}

void FloatingChat::PollLoop()
{
    // PollLoop kept for backward compatibility but not used with WebView2-based UI.
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
