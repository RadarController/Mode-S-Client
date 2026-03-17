// Mode-S Client.cpp
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wininet.h>
#include <winhttp.h>
#include <objbase.h>
#pragma comment(lib, "winhttp.lib")

#include <string>
#include <thread>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <cctype>
#include <algorithm>
#include <ctime>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "httplib.h"
#include "json.hpp"

#include "resource.h"
#include "version.h"
#include "AppConfig.h"
#include "AppState.h"
#include "app/AppBootstrap.h"
#include "app/AppRuntime.h"
#include "app/AppShutdown.h"
#include "core/AppPaths.h"
#include "core/StringUtil.h"
#include "http/HttpServer.h"
#include "chat/ChatAggregator.h"
#include "twitch/TwitchHelixService.h"
#include "twitch/TwitchHelixController.h"
#include "twitch/TwitchIrcWsClient.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchAuth.h"
#include "tiktok/TikTokSidecar.h"
#include "tiktok/TikTokFollowersService.h"
#include "youtube/YouTubeAuth.h"
#include "youtube/YouTubeLiveChatService.h"
#include "euroscope/EuroScopeIngestService.h"
#include "obs/ObsWsClient.h"
#include "floating/FloatingChat.h"
#include "platform/PlatformControl.h"
#include "log/UiLog.h"

#include "ui/WebViewHost.h"
#include "ui/SplashScreen.h"

static const wchar_t* kModernUiUrl = L"http://127.0.0.1:17845/app";

// --------------------------- Globals (UI handles) ---------------------------
static HWND gMainWnd = nullptr;
static constexpr UINT WM_APP_LOG = WM_APP + 100;
static constexpr UINT WM_APP_SPLASH_READY = WM_APP + 201;
static const wchar_t* kAppDisplayName = L"StreamingATC.Live Mode-S Client";

// Floating chat instance
// Moved to FloatingChat class in src/floating/FloatingChat.*
static std::unique_ptr<class FloatingChat> gFloatingChat;

// App lifecycle
static AppRuntime gRuntime;

// --------------------------- Helpers ----------------------------------------
struct HttpResult {
    int status = 0;
    DWORD winerr = 0;
    std::string body;
};

static HttpResult WinHttpRequest(const std::wstring& method,
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& headers,
    const std::string& body,
    bool secure)
{
    HttpResult r;
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

    if (!headers.empty()) {
        WinHttpAddRequestHeaders(hRequest, headers.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

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
    if (WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
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

// --------------------------- Translucent window helper ----------------------
// Prefer Acrylic blur so the background is translucent but text remains readable.
// Falls back to layered alpha if unavailable (Windows versions without the API).
typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_ENABLE_HOSTBACKDROP = 5
} ACCENT_STATE;

typedef struct _ACCENT_POLICY {
    int AccentState;
    int AccentFlags;
    int GradientColor; // ARGB
    int AnimationId;
} ACCENT_POLICY;

typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
    int Attrib;
    PVOID pvData;
    SIZE_T cbData;
} WINDOWCOMPOSITIONATTRIBDATA;

static bool TryEnableAcrylic(HWND hwnd, BYTE bgOpacity /*0..255*/)
{
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (!hUser) return false;

    using Fn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
    auto p = (Fn)GetProcAddress(hUser, "SetWindowCompositionAttribute");
    if (!p) return false;

    const int WCA_ACCENT_POLICY = 19;

    ACCENT_POLICY policy{};
    policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.AccentFlags = 2;

    // Dark tint; bgOpacity controls tint alpha (NOT child controls).
    policy.GradientColor = (bgOpacity << 24) | 0x202020;

    WINDOWCOMPOSITIONATTRIBDATA data{};
    data.Attrib = WCA_ACCENT_POLICY;
    data.pvData = &policy;
    data.cbData = sizeof(policy);

    return p(hwnd, &data) != 0;
}

// --------------------------- Main Window ------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    auto BuildShutdownDeps = [&]() -> AppShutdown::Dependencies {
        return gRuntime.BuildShutdownDeps();
        };

    // Restartable Helix poller (needed when Twitch channel/login changes).
    auto RestartTwitchHelixPoller = [&](const std::string& reason) {
        TwitchHelixController::Dependencies deps{
            hwnd,
            gRuntime.config,
            gRuntime.state,
            gRuntime.twitchHelixThread,
            gRuntime.twitchHelixRunning,
            gRuntime.twitchHelixBoundLogin
        };
        TwitchHelixController::RestartPoller(deps, reason);
    };

    switch (msg) {

    case WM_CREATE:
    {
        auto deps = gRuntime.BuildBootstrapDeps(hwnd);

        if (!gFloatingChat) gFloatingChat = std::make_unique<FloatingChat>();

        AppBootstrap::InitializeUiAndState(
            deps,
            kModernUiUrl,
            APP_VERSION_FILE_W,
            [hwnd](HWND) {
                if (gFloatingChat) gFloatingChat->Open(hwnd);
            });

        PostMessageW(hwnd, WM_APP + 1, 0, 0);
        return 0;
    }

    case WM_APP + 1:
    {
        auto deps = gRuntime.BuildBootstrapDeps(hwnd);

        AppBootstrap::StartBackend(
            deps,
            kModernUiUrl,
            [&](const std::string& reason) {
                RestartTwitchHelixPoller(reason);
            });

        PostMessageW(hwnd, WM_APP_SPLASH_READY, 0, 0);
        return 0;
    }

    case WM_APP_LOG:
    {
        UiLog_HandleAppLogMessage(lParam);
        return 0;
    }

    case WM_APP_SPLASH_READY:
    {
        SplashScreen::OnAppReady(hwnd);
        return 0;
    }

    case WM_CLOSE:
    {
        auto deps = BuildShutdownDeps();
        AppShutdown::BeginShutdown(deps, hwnd);
        return 0;
    }

    case WM_SIZE:
        WebViewHost::ResizeToClient(hwnd);
        return 0;

    case WM_DESTROY:
    {
        // Safety net: if we somehow got here without WM_CLOSE
        UiLog_SetLogHwnd(nullptr);
        SplashScreen::Destroy();
        WebViewHost::Destroy();

        auto deps = BuildShutdownDeps();
        AppShutdown::BeginShutdown(deps, nullptr);

        PostQuitMessage(0);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}


static RECT CenteredWindowRect(int width, int height)
{
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    RECT rc{ 0, 0, width, height };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    const int windowWidth = rc.right - rc.left;
    const int windowHeight = rc.bottom - rc.top;

    const int x = wa.left + ((wa.right - wa.left) - windowWidth) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - windowHeight) / 2;

    return RECT{ x, y, x + windowWidth, y + windowHeight };
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(hrCom) || hrCom == RPC_E_CHANGED_MODE;

    const DWORD uiThreadId = GetCurrentThreadId();

    const wchar_t CLASS_NAME[] = L"StreamHubWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    // Diagnostic: check RegisterClassW result
    ATOM reg = RegisterClassW(&wc);
    if (!reg) {
        DWORD e = GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            wchar_t buf[256];
            _snwprintf_s(
                buf,
                _countof(buf),
                _TRUNCATE,
                L"[Startup] RegisterClassW failed. GetLastError=%lu",
                e
            );

            LogLine(buf);
            MessageBoxW(nullptr, buf, L"Mode-S Client", MB_ICONERROR);
            return 0;
        }
    }

    WebViewHost::EnsureSharedEnvironment();

    // Splash screen shown while the main window initializes
    SplashScreen::Create(hInstance, kAppDisplayName, APP_VERSION_FILE_W);

    // Process any pending messages so the splash paints immediately.
    MSG sm{};
    while (PeekMessageW(&sm, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&sm);
        DispatchMessageW(&sm);
    }

    RECT mainRc = CenteredWindowRect(1440, 900);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, kAppDisplayName,
        WS_OVERLAPPEDWINDOW,
        mainRc.left, mainRc.top,
        mainRc.right - mainRc.left,
        mainRc.bottom - mainRc.top,
        nullptr, nullptr, hInstance, nullptr
    );

    gMainWnd = hwnd;
    UiLog_SetUiContext(hwnd, uiThreadId, WM_APP_LOG);

    // Set window title to include build/file version
    {
        std::wstring title = std::wstring(kAppDisplayName) + L" " + APP_VERSION_FILE_W;
        SetWindowTextW(hwnd, title.c_str());
    }


    if (!hwnd) {
        DWORD e = GetLastError();
        wchar_t buf[256];
        _snwprintf_s(
            buf,
            _countof(buf),
            _TRUNCATE,
            L"[Startup] CreateWindowExW failed. GetLastError=%lu",
            e
        );
        LogLine(buf);
        MessageBoxW(nullptr, buf, L"Mode-S Client", MB_ICONERROR);
        return 0;
    }

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)));

    // Defer showing the main UI until initialization is complete (splash stays visible)
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }

    return 0;
}