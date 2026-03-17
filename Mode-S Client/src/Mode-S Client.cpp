// Mode-S Client.cpp
// Main native entry point for the desktop app.
// At this stage of the refactor, this file is intentionally kept to:
// - process startup / shutdown shell logic
// - own the main Win32 window procedure
// - delegate real work to extracted modules

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <string>
#include <thread>
#include <memory>

#include "resource.h"
#include "version.h"

// Core app state/config.
#include "AppConfig.h"
#include "AppState.h"

// Extracted app lifecycle modules.
#include "app/AppBootstrap.h"   // startup / backend initialisation
#include "app/AppRuntime.h"     // long-lived runtime ownership
#include "app/AppShutdown.h"    // orderly shutdown / teardown

// Core helpers.
#include "core/AppPaths.h"
#include "core/StringUtil.h"

// Local HTTP server.
#include "http/HttpServer.h"

// Chat aggregation.
#include "chat/ChatAggregator.h"

// Twitch integration pieces.
#include "twitch/TwitchHelixService.h"
#include "twitch/TwitchHelixController.h"
#include "twitch/TwitchIrcWsClient.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchAuth.h"

// TikTok integration pieces.
#include "tiktok/TikTokSidecar.h"
#include "tiktok/TikTokFollowersService.h"

// YouTube integration pieces.
#include "youtube/YouTubeAuth.h"
#include "youtube/YouTubeLiveChatService.h"

// Other app services / integrations.
#include "euroscope/EuroScopeIngestService.h"
#include "obs/ObsWsClient.h"
#include "floating/FloatingChat.h"
#include "platform/PlatformControl.h"

// Native UI helpers extracted from this file.
#include "ui/WindowEffects.h"
#include "ui/WindowLayout.h"

// UI logging.
#include "log/UiLog.h"

// Main UI hosting pieces.
#include "ui/WebViewHost.h"
#include "ui/SplashScreen.h"

// URL for the local WebView-hosted modern UI.
// The HTTP server serves this locally on 127.0.0.1.
static const wchar_t* kModernUiUrl = L"http://127.0.0.1:17845/app";

// --------------------------- Globals (UI handles) ---------------------------

// Main native window handle for the app.
static HWND gMainWnd = nullptr;

// Custom app message used to marshal log output safely back to the UI thread.
static constexpr UINT WM_APP_LOG = WM_APP + 100;

// Custom app message posted when startup is complete and the splash can hand over.
static constexpr UINT WM_APP_SPLASH_READY = WM_APP + 201;

// User-facing app display name.
static const wchar_t* kAppDisplayName = L"StreamingATC.Live Mode-S Client";

// Floating chat instance.
// This is optional UI chrome opened alongside the main window if enabled.
static std::unique_ptr<class FloatingChat> gFloatingChat;

// Long-lived runtime owner.
// Holds services, worker threads, lifecycle flags, config/state, etc.
static AppRuntime gRuntime;

// --------------------------- Main Window ------------------------------------

// Main Win32 window procedure.
// This handles top-level lifecycle messages and delegates real work to extracted modules.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    // Small local helper that builds the dependency bundle used by AppShutdown.
    // This keeps the shutdown call sites tidy.
    auto BuildShutdownDeps = [&]() -> AppShutdown::Dependencies {
        return gRuntime.BuildShutdownDeps();
        };

    // Restartable Twitch Helix poller helper.
    // This is needed when Twitch login/channel context changes and the Helix poller
    // must be rebound to the correct account.
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
        // Build startup dependencies from the shared runtime object.
        auto deps = gRuntime.BuildBootstrapDeps(hwnd);

        // Ensure floating chat exists before initialisation callback tries to open it.
        if (!gFloatingChat) gFloatingChat = std::make_unique<FloatingChat>();

        // Perform UI/state initialisation:
        // - prepare config/state
        // - initialise splash/main UI relationship
        // - wire up initial UI pieces
        AppBootstrap::InitializeUiAndState(
            deps,
            kModernUiUrl,
            APP_VERSION_FILE_W,
            [hwnd](HWND) {
                if (gFloatingChat) gFloatingChat->Open(hwnd);
            });

        // Post a follow-up app message so backend startup happens asynchronously
        // after window creation has returned.
        PostMessageW(hwnd, WM_APP + 1, 0, 0);
        return 0;
    }

    case WM_APP + 1:
    {
        // Build startup dependencies again for backend bring-up.
        auto deps = gRuntime.BuildBootstrapDeps(hwnd);

        // Start backend services:
        // - local HTTP server
        // - platform integrations
        // - background workers
        // - other runtime pieces needed by the UI
        AppBootstrap::StartBackend(
            deps,
            kModernUiUrl,
            [&](const std::string& reason) {
                RestartTwitchHelixPoller(reason);
            });

        // Tell the splash screen startup is complete.
        PostMessageW(hwnd, WM_APP_SPLASH_READY, 0, 0);
        return 0;
    }

    case WM_APP_LOG:
    {
        // Process a marshalled log message on the UI thread.
        UiLog_HandleAppLogMessage(lParam);
        return 0;
    }

    case WM_APP_SPLASH_READY:
    {
        // Hand off from splash to main app window once startup is finished.
        SplashScreen::OnAppReady(hwnd);
        return 0;
    }

    case WM_CLOSE:
    {
        // Begin orderly shutdown when the user closes the main window.
        auto deps = BuildShutdownDeps();
        AppShutdown::BeginShutdown(deps, hwnd);
        return 0;
    }

    case WM_SIZE:
        // Keep the embedded WebView filling the client area as the native window resizes.
        WebViewHost::ResizeToClient(hwnd);
        return 0;

    case WM_DESTROY:
    {
        // Safety-net shutdown path:
        // if we somehow reach destruction without WM_CLOSE having completed cleanly,
        // still tear down UI pieces and invoke the central shutdown routine.
        UiLog_SetLogHwnd(nullptr);
        SplashScreen::Destroy();
        WebViewHost::Destroy();

        auto deps = BuildShutdownDeps();
        AppShutdown::BeginShutdown(deps, nullptr);

        // End the Win32 message loop.
        PostQuitMessage(0);
        return 0;
    }

    default:
        break;
    }

    // Default processing for messages we do not explicitly handle.
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Native Windows process entry point.
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {

    // Initialise COM for the UI thread.
    // Apartment-threaded COM is typically required for WebView2 / UI-related COM work.
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Record whether COM was successfully initialised by this call, or whether
    // it was already initialised differently.
    const bool comInitialized = SUCCEEDED(hrCom) || hrCom == RPC_E_CHANGED_MODE;

    // Cache the UI thread ID for log-message marshalling and other thread-aware UI tasks.
    const DWORD uiThreadId = GetCurrentThreadId();

    // Native Win32 window class name.
    const wchar_t CLASS_NAME[] = L"StreamHubWindow";

    // Define the native main window class.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;               // message handler
    wc.hInstance = hInstance;              // owning module instance
    wc.lpszClassName = CLASS_NAME;         // class name used by CreateWindowExW
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); // standard arrow cursor
    wc.hbrBackground = nullptr;            // no automatic background brush; UI paints itself

    // Register the window class.
    // If it already exists, that is acceptable.
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

            // Log and surface a startup failure if the window class cannot be registered.
            LogLine(buf);
            MessageBoxW(nullptr, buf, L"Mode-S Client", MB_ICONERROR);
            return 0;
        }
    }

    // Pre-create / ensure the shared WebView environment is ready before the main window
    // tries to host web content.
    WebViewHost::EnsureSharedEnvironment();

    // Create the splash screen shown during initial startup.
    SplashScreen::Create(hInstance, kAppDisplayName, APP_VERSION_FILE_W);

    // Pump any pending messages immediately so the splash paints straight away
    // rather than appearing frozen while startup continues.
    MSG sm{};
    while (PeekMessageW(&sm, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&sm);
        DispatchMessageW(&sm);
    }

    // Calculate a centred main-window rectangle for a 1440x900 client area.
    RECT mainRc = CenteredWindowRect(1440, 900);

    // Create the main native shell window.
    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, kAppDisplayName,
        WS_OVERLAPPEDWINDOW,
        mainRc.left, mainRc.top,
        mainRc.right - mainRc.left,
        mainRc.bottom - mainRc.top,
        nullptr, nullptr, hInstance, nullptr
    );

    // Store the main window handle globally for any top-level shell usage.
    gMainWnd = hwnd;

    // Tell the UI log system where to post log messages for marshalled UI-thread handling.
    UiLog_SetUiContext(hwnd, uiThreadId, WM_APP_LOG);

    // Set the native window title to include the build/file version.
    {
        std::wstring title = std::wstring(kAppDisplayName) + L" " + APP_VERSION_FILE_W;
        SetWindowTextW(hwnd, title.c_str());
    }

    // If window creation failed, surface the startup error and abort.
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

    // Set big and small window icons from compiled resources.
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)));

    // Keep the main window hidden until startup completes.
    // The splash remains visible during this phase.
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    // Standard Win32 message loop.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Uninitialise COM if this call successfully initialised it.
    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }

    return 0;
}