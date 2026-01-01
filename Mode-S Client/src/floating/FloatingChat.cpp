#include "FloatingChat.h"

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <atomic>
#include <sstream>
#include "json.hpp"

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
    pollThread_ = std::thread(&FloatingChat::PollLoop, this);
    return true;
}

void FloatingChat::Close()
{
    if (!wnd_) return;
    running_ = false;
    if (pollThread_.joinable()) pollThread_.join();
    DestroyWindow(wnd_);
    wnd_ = nullptr;
    edit_ = nullptr;
}

bool FloatingChat::IsOpen() const { return wnd_ != nullptr; }

LRESULT CALLBACK FloatingChat::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        if (cs && cs->lpCreateParams) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
    }

    auto* self = reinterpret_cast<FloatingChat*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FloatingChat::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            0, 0, 0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        return 0;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        if (edit_) MoveWindow(edit_, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        return 0;
    }
    case WM_APP + 50: {
        auto p = reinterpret_cast<std::wstring*>(lParam);
        if (p && edit_) {
            bool atBottom = true;
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            if (GetScrollInfo(edit_, SB_VERT, &si)) {
                int pos = si.nPos;
                int page = si.nPage;
                int max = si.nMax;
                if (max > 0) atBottom = (pos + page >= max - 1);
            }
            int prevFirstVisible = (int)SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
            SetWindowTextW(edit_, p->c_str());
            if (atBottom) {
                int len = GetWindowTextLengthW(edit_);
                if (len < 0) len = 0;
                SendMessageW(edit_, EM_SETSEL, (WPARAM)len, (LPARAM)len);
                SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
            } else {
                int newFirstVisible = (int)SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
                int delta = prevFirstVisible - newFirstVisible;
                if (delta != 0) SendMessageW(edit_, EM_LINESCROLL, 0, (LPARAM)delta);
            }
        }
        delete p;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        running_ = false;
        if (pollThread_.joinable()) pollThread_.join();
        wnd_ = nullptr;
        edit_ = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Simplified WinHttpRequest used only by this module.
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
    while (running_ && wnd_) {
        auto r = WinHttpRequest(L"GET", L"127.0.0.1", 17845, L"/api/chat/recent?limit=200", L"", "", false);
        if (r.status == 200 && !r.body.empty()) {
            try {
                auto j = nlohmann::json::parse(r.body);
                std::wstring out;
                if (j.contains("messages") && j["messages"].is_array()) {
                    for (const auto& msg : j["messages"]) {
                        std::string user;
                        std::string text;
                        std::string platform;
                        if (msg.is_object()) {
                            if (msg.contains("user")) user = msg["user"].get<std::string>();
                            if (msg.contains("message")) text = msg["message"].get<std::string>();
                            if (text.empty() && msg.contains("text")) text = msg["text"].get<std::string>();
                            if (msg.contains("platform")) platform = msg["platform"].get<std::string>();
                        }
                        std::wstring wuser = std::wstring();
                        if (!user.empty()) {
                            int len = MultiByteToWideChar(CP_UTF8, 0, user.c_str(), (int)user.size(), nullptr, 0);
                            wuser.resize(len);
                            if (len) MultiByteToWideChar(CP_UTF8, 0, user.c_str(), (int)user.size(), &wuser[0], len);
                        }
                        std::wstring wtext = std::wstring();
                        if (!text.empty()) {
                            int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
                            wtext.resize(len);
                            if (len) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &wtext[0], len);
                        }
                        if (wuser.empty()) {
                            if (!platform.empty()) {
                                int len = MultiByteToWideChar(CP_UTF8, 0, platform.c_str(), (int)platform.size(), nullptr, 0);
                                wuser.resize(len);
                                if (len) MultiByteToWideChar(CP_UTF8, 0, platform.c_str(), (int)platform.size(), &wuser[0], len);
                            }
                        }
                        if (wuser.empty()) wuser = L"(anon)";
                        out += wuser + L": " + wtext + L"\r\n";
                    }
                }
                auto p = new std::wstring(std::move(out));
                PostMessageW(wnd_, WM_APP + 50, 0, (LPARAM)p);
            }
            catch (...) {}
        }
        for (int i = 0; i < 50 && running_; ++i) Sleep(10);
    }
}
