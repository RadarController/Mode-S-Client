// Mode-S Client.cpp
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wininet.h>
#include <winhttp.h>
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
#include "http/HttpServer.h"
#include "chat/ChatAggregator.h"
#include "twitch/TwitchHelixService.h"
#include "twitch/TwitchIrcWsClient.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchAuth.h"
#include "youtube/YouTubeAuth.h"
#include "tiktok/TikTokSidecar.h"
#include "tiktok/TikTokFollowersService.h"
#include "youtube/YouTubeLiveChatService.h"
#include "euroscope/EuroScopeIngestService.h"
#include "obs/ObsWsClient.h"
#include "floating/FloatingChat.h"
#include "platform/PlatformControl.h"

// Web UI log capture: LogLine() will also push into AppState so /api/log can display it.
static AppState* gStateForWebLog = nullptr;

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

// --------------------------- WebView2 (Modern UI host) ----------------------
#if !defined(HAVE_WEBVIEW2)
#  if defined(__has_include)
#    if __has_include("WebView2.h")
#      include <wrl.h>
#      include "WebView2.h"
#      define HAVE_WEBVIEW2 1
#    else
#      define HAVE_WEBVIEW2 0
#    endif
#  else
#    define HAVE_WEBVIEW2 0
#  endif
#endif

#if HAVE_WEBVIEW2
using Microsoft::WRL::ComPtr;
static ComPtr<ICoreWebView2Controller> gMainWebController;
static ComPtr<ICoreWebView2>           gMainWebView;
#endif

// Modern WebView UI is now the primary dashboard/control surface.
// Flip this to false only for legacy Win32 fallback/debugging.
static bool gUseModernUi = true;
static std::atomic<bool> gHttpReady{ false };
static const wchar_t* kModernUiUrl = L"http://127.0.0.1:17845/app";

// --------------------------- Globals (UI handles) ---------------------------
static HWND gLog = nullptr;
static std::mutex gLogMutex;
static HWND gMainWnd = nullptr;
static DWORD gUiThreadId = 0;
static constexpr UINT WM_APP_LOG = WM_APP + 100;
// Splash screen
static HWND gSplashWnd = nullptr;
static HWND gSplashLog = nullptr;
static constexpr UINT WM_APP_SPLASH_READY = WM_APP + 201;
static constexpr UINT_PTR SPLASH_CLOSE_TIMER_ID = 1001;
static constexpr UINT SPLASH_CLOSE_DELAY_MS = 1 * 1000; // test: keep splash for 1s after ready
static const wchar_t* kAppDisplayName = L"StreamingATC.Live Mode-S Client";
static const wchar_t* kAppVersion = APP_VERSION_W; // auto-generated per build

// Floating chat instance
// Moved to FloatingChat class in src/floating/FloatingChat.*
static std::unique_ptr<class FloatingChat> gFloatingChat;

// App lifecycle
static std::atomic<bool> gRunning{ true };

// Helix poller is restartable independently of full app shutdown.
static std::atomic<bool> gTwitchHelixRunning{ true };
static std::string gTwitchHelixBoundLogin;

// Forward decl
static HWND CreateSplashWindow(HINSTANCE hInstance);
static void DestroySplashWindow();

// --------------------------- Helpers ----------------------------------------
static std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

// Read Twitch user access token from config.json.
// Stored at: { "twitch": { "user_access_token": "..." } }
static std::string ReadTwitchUserAccessToken()
{
    auto try_path = [](const std::filesystem::path& p) -> std::string {
        try {
            std::ifstream f(p);
            if (!f.is_open()) return {};
            nlohmann::json j;
            f >> j;
            return j.value("twitch", nlohmann::json::object()).value("user_access_token", "");
        } catch (...) {
            return {};
        }
    };

    // Prefer config.json next to the exe
    std::filesystem::path p1 = std::filesystem::path(GetExeDir()) / "config.json";
    std::string tok = try_path(p1);
    if (!tok.empty()) return tok;

    // Fallback: current working directory
    return try_path(std::filesystem::path("config.json"));
}

static void AppendLogOnUiThread(const std::wstring& s)
{
    if (!gLog && !gSplashLog) return;

    // Make each log call atomic across threads and avoid double newlines.
    std::lock_guard<std::mutex> _lk(gLogMutex);

    std::wstring clean = s;

    // Trim trailing CR/LF
    while (!clean.empty() && (clean.back() == L'\r' || clean.back() == L'\n'))
        clean.pop_back();

    int len = GetWindowTextLengthW(gLog);
    SendMessageW(gLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(gLog, EM_REPLACESEL, FALSE, (LPARAM)clean.c_str());
    SendMessageW(gLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");

    if (gSplashLog) {
        int slen = GetWindowTextLengthW(gSplashLog);
        SendMessageW(gSplashLog, EM_SETSEL, (WPARAM)slen, (LPARAM)slen);
        SendMessageW(gSplashLog, EM_REPLACESEL, FALSE, (LPARAM)clean.c_str());
        SendMessageW(gSplashLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }
}

static void LogLine(const std::wstring& s)
{
    // Also feed the Web UI log buffer (served via /api/log)
    if (gStateForWebLog) {
        gStateForWebLog->push_log_utf8(ToUtf8(s));
    }
    // If called from a worker thread, marshal to the UI thread to avoid deadlocks/freezes.
    if (gUiThreadId != 0 && GetCurrentThreadId() != gUiThreadId) {
        if (gMainWnd) {
            auto* heap = new std::wstring(s);
            if (!PostMessageW(gMainWnd, WM_APP_LOG, 0, (LPARAM)heap)) {
                delete heap; // fallback if posting fails
            }
        }
        return;
    }

    AppendLogOnUiThread(s);
}

static void JoinWithTimeout(std::thread& t, DWORD timeoutMs, const wchar_t* name)
{
    if (!t.joinable()) return;

    HANDLE h = (HANDLE)t.native_handle();
    DWORD wait = WaitForSingleObject(h, timeoutMs);

    if (wait == WAIT_OBJECT_0) {
        t.join();
        LogLine(std::wstring(L"Joined thread: ") + name);
        return;
    }

    // Timed out (or failed). Don't hang shutdown.
    LogLine(std::wstring(L"Timeout waiting for thread: ") + name + L" (detaching)");
    t.detach(); // process exit will still terminate it if we quit main loop
}

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}

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

static std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

static std::string Trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

static std::string SanitizeTikTok(const std::string& input)
{
    std::string s = Trim(input);
    s.erase(std::remove(s.begin(), s.end(), '@'), s.end());
    return Trim(s);
}

static std::string SanitizeYouTubeHandle(const std::string& input)
{
    std::string s = Trim(input);
    // YouTube handles are typically entered as "@handle" - strip leading @
    s.erase(std::remove(s.begin(), s.end(), '@'), s.end());
    // remove spaces (handles cannot contain spaces)
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){ return std::isspace(c) != 0; }), s.end());
    return Trim(s);
}

static std::string SanitizeTwitchLogin(std::string s)
{
    s = Trim(s);
    if (!s.empty() && s[0] == '#') s.erase(s.begin());
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string ReadFileUtf8(const std::wstring& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string data;
    data.resize(sz > 0 ? (size_t)sz : 0);
    if (sz > 0) fread(data.data(), 1, (size_t)sz, f);
    fclose(f);
    return data;
}

//Chat overlay helpers
static void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string UrlEncodeGoogleFontFamily(std::string family)
{
    for (char& c : family) {
        if (c == ' ') c = '+';
    }
    return family;
}

static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int ParseIntOrDefault(const std::wstring& w, int def)
{
    try {
        if (w.empty()) return def;
        return std::stoi(w);
    }
    catch (...) {
        return def;
    }

}

// --------------------------- Splash screen ---------------------------------
static LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // Basic, dependency-free splash: title + version + live log output
        HFONT hFontTitle = nullptr;
        HFONT hFontBody = nullptr;

        {
            LOGFONTW lf{};
            lf.lfHeight = -22;
            lf.lfWeight = FW_SEMIBOLD;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            hFontTitle = CreateFontIndirectW(&lf);
        }
        {
            LOGFONTW lf{};
            lf.lfHeight = -16;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            hFontBody = CreateFontIndirectW(&lf);
        }

        // Store fonts as window properties so we can delete on destroy
        SetPropW(hwnd, L"splash_font_title", hFontTitle);
        SetPropW(hwnd, L"splash_font_body", hFontBody);

        const int pad = 14;

        HWND hTitle = CreateWindowW(L"STATIC", kAppDisplayName,
            WS_CHILD | WS_VISIBLE,
            pad, pad, 520, 28, hwnd, nullptr, nullptr, nullptr);

        std::wstring ver = std::wstring(L"Loading… ") + kAppVersion;
        HWND hVer = CreateWindowW(L"STATIC", ver.c_str(),
            WS_CHILD | WS_VISIBLE,
            pad, pad + 30, 520, 20, hwnd, nullptr, nullptr, nullptr);

        gSplashLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            pad, pad + 58, 520, 220, hwnd, nullptr, nullptr, nullptr);

        if (hTitle) SendMessageW(hTitle, WM_SETFONT, (WPARAM)hFontTitle, TRUE);
        if (hVer)   SendMessageW(hVer, WM_SETFONT, (WPARAM)hFontBody, TRUE);
        if (gSplashLog) SendMessageW(gSplashLog, WM_SETFONT, (WPARAM)hFontBody, TRUE);

        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(230, 230, 230));

        static HBRUSH hBg = CreateSolidBrush(RGB(18, 18, 18));
        static HBRUSH hEditBg = CreateSolidBrush(RGB(32, 32, 32));

        if (msg == WM_CTLCOLOREDIT) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(32, 32, 32));
            return (LRESULT)hEditBg;
        }

        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)hBg;
    }

    case WM_ERASEBKGND:
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        static HBRUSH hBg = CreateSolidBrush(RGB(18, 18, 18));
        FillRect((HDC)wParam, &rc, hBg);
        return 1;
    }


    case WM_TIMER:
    {
        if (wParam == SPLASH_CLOSE_TIMER_ID) {
            KillTimer(hwnd, SPLASH_CLOSE_TIMER_ID);

            // Close splash
            DestroySplashWindow();

            // Now reveal main UI
            if (gMainWnd && IsWindow(gMainWnd)) {
                ShowWindow(gMainWnd, SW_SHOWDEFAULT);
                UpdateWindow(gMainWnd);
                SetForegroundWindow(gMainWnd);
            }
            return 0;
        }
        break;
    }

    case WM_CLOSE:
    {
        // If the user closes the splash, treat it as "exit app".
        // Close main window too (it may still be hidden during boot).
        if (gMainWnd && IsWindow(gMainWnd)) {
            DestroyWindow(gMainWnd);
        }

        DestroyWindow(hwnd);      // close splash
        PostQuitMessage(0);       // end message loop
        return 0;
    }

    case WM_DESTROY:
    {
        // cleanup fonts stored as properties
        if (auto h = (HFONT)GetPropW(hwnd, L"splash_font_title")) { DeleteObject(h); RemovePropW(hwnd, L"splash_font_title"); }
        if (auto h = (HFONT)GetPropW(hwnd, L"splash_font_body")) { DeleteObject(h); RemovePropW(hwnd, L"splash_font_body"); }
        gSplashLog = nullptr;

        // If splash is closing and main window isn't alive, exit.
        if (!gMainWnd || !IsWindow(gMainWnd)) {
            PostQuitMessage(0);
        }
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateSplashWindow(HINSTANCE hInstance)
{
    const wchar_t* kSplashClass = L"ModeSClientSplash";

    static std::atomic<bool> registered{ false };
    if (!registered.exchange(true))
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = SplashWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = kSplashClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassW(&wc);
    }

    const int w = 560;
    const int h = 320;

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.left + ((wa.right - wa.left) - w) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        kSplashClass,
        kAppDisplayName,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        nullptr, nullptr, hInstance, nullptr);

    if (hwnd) {
        {
            std::wstring title = std::wstring(kAppDisplayName) + L" " + APP_VERSION_FILE_W;
            SetWindowTextW(hwnd, title.c_str());
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

static void DestroySplashWindow()
{
    if (gSplashWnd && IsWindow(gSplashWnd)) {
        DestroyWindow(gSplashWnd);
    }
    gSplashWnd = nullptr;
    gSplashLog = nullptr;
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
    static AppConfig config;

    static AppState state;
    static ChatAggregator chat;
    static TikTokSidecar tiktok;
    static TikTokSidecar youtube;
    static TwitchIrcWsClient twitch;
    static TwitchEventSubWsClient twitchEventSub;
    static TwitchAuth twitchAuth;
    static YouTubeAuth youtubeAuth;
    static YouTubeLiveChatService youtubeChat;
    static std::unique_ptr<HttpServer> gHttp;
    static std::thread metricsThread;
    static std::thread twitchHelixThread;
    static std::thread tiktokFollowersThread;
    static EuroScopeIngestService euroscope;
    static ObsWsClient obs;

    // Centralised shutdown routine (idempotent).
   // Lives inside WndProc so it can see the static service/thread instances above.
    auto BeginShutdown = [&](HWND hwndToDestroy)
        {
            static std::atomic<bool> shuttingDown{ false };
            if (shuttingDown.exchange(true)) return;

            OutputDebugStringW(L"SHUTDOWN: BeginShutdown()\n");

            // 1) Flip flags so loops exit
            gRunning = false;
            gTwitchHelixRunning = false;
            OutputDebugStringW(L"SHUTDOWN: flags set\n");

            // 2) Stop HTTP early
            if (gHttp) {
                OutputDebugStringW(L"SHUTDOWN: stopping HTTP\n");
                gHttp->Stop();
                gHttp.reset();
                OutputDebugStringW(L"SHUTDOWN: HTTP stopped\n");
            }

            // 3) Join threads next
            if (tiktokFollowersThread.joinable()) {
                OutputDebugStringW(L"SHUTDOWN: join tiktokFollowersThread...\n");
                tiktokFollowersThread.join();
                OutputDebugStringW(L"SHUTDOWN: joined tiktokFollowersThread\n");
            }

            if (twitchHelixThread.joinable()) {
                OutputDebugStringW(L"SHUTDOWN: join twitchHelixThread...\n");
                twitchHelixThread.join();
                OutputDebugStringW(L"SHUTDOWN: joined twitchHelixThread\n");
            }

            if (metricsThread.joinable()) {
                OutputDebugStringW(L"SHUTDOWN: join metricsThread...\n");
                metricsThread.join();
                OutputDebugStringW(L"SHUTDOWN: joined metricsThread\n");
            }

            // 4) Stop services last (less risk of deadlock)
            OutputDebugStringW(L"SHUTDOWN: stopping services...\n");

            OutputDebugStringW(L"SHUTDOWN: stopping twitchEventSub...\n");
            try { twitchEventSub.Stop(); }
            catch (...) {}
            OutputDebugStringW(L"SHUTDOWN: stopped twitchEventSub\n");

            OutputDebugStringW(L"SHUTDOWN: stopping twitchAuth...\n");
            try { twitchAuth.Stop(); }
            catch (...) {}
            OutputDebugStringW(L"SHUTDOWN: stopped twitchAuth\n");

            OutputDebugStringW(L"SHUTDOWN: stopping twitch...\n");
            try { twitch.stop(); }
            catch (...) {}
            OutputDebugStringW(L"SHUTDOWN: stopped twitch\n");

            OutputDebugStringW(L"SHUTDOWN: stopping youtubeChat...\n");
            try { youtubeChat.stop(); }
            catch (...) {}
            OutputDebugStringW(L"SHUTDOWN: stopped youtubeChat\n");

            OutputDebugStringW(L"SHUTDOWN: stopping youtube...\n");
            try { youtube.stop(); }
            catch (...) {}
            OutputDebugStringW(L"SHUTDOWN: stopped youtube\n");

            OutputDebugStringW(L"SHUTDOWN: stopping tiktok...\n");
            try { tiktok.stop(); }
            catch (...) {}
            OutputDebugStringW(L"SHUTDOWN: stopped tiktok\n");

            OutputDebugStringW(L"SHUTDOWN: services stopped\n");

            // Destroy window to reach WM_DESTROY -> PostQuitMessage
            if (hwndToDestroy && IsWindow(hwndToDestroy)) {
                OutputDebugStringW(L"SHUTDOWN: destroying window\n");
                DestroyWindow(hwndToDestroy);
            }
        };

    // Subscribe the chatbot handler only once.
    // Must live at function scope (not inside a switch/case) to avoid C++ case-jump
    // rules that trigger C2360/C2361.
    static bool botSubscribed = false;

    // Restartable Helix poller (needed when Twitch channel/login changes).
    auto RestartTwitchHelixPoller = [&](const std::string& reason) {
        const std::string login = config.twitch_login;
        if (login.empty()) return;

        // Avoid unnecessary restarts.
        if (gTwitchHelixBoundLogin == login && twitchHelixThread.joinable()) return;

        LogLine(L"TWITCH: restarting Helix poller (" + ToW(reason) + L")");

        // Stop existing poller thread if running.
        if (twitchHelixThread.joinable()) {
            gTwitchHelixRunning = false;
            twitchHelixThread.join();
        }

        // Reset UI + metrics immediately so we don't show previous channel values.
        state.set_twitch_viewers(0);
        state.set_twitch_followers(0);
        state.set_twitch_live(false);

        gTwitchHelixRunning = true;
        gTwitchHelixBoundLogin = login;

        twitchHelixThread = StartTwitchHelixPoller(
            hwnd,
            config,
            state,
            gTwitchHelixRunning,
            0,
            TwitchHelixUiCallbacks{
                /*log*/ [](const std::wstring& s) { OutputDebugStringW((s + L"\n").c_str()); },
                /*set_status*/   [&](const std::wstring& /*s*/) {},
                /*set_live*/     [&](bool /*live*/) {},
                /*set_viewers*/  [&](int /*v*/) {},
                /*set_followers*/[&](int /*f*/) {}
            }
        );
    };

switch (msg) {

    case WM_CREATE:
    {
        // Load config first (so edits start populated)
        (void)config.Load();

        // -----------------------------------------------------------------
        // Bot commands persistence (2.2)
        // Store bot_commands.json next to the exe, and load it at startup.
        // -----------------------------------------------------------------
        try {
            std::filesystem::path botPath = std::filesystem::path(GetExeDir()) / "bot_commands.json";
            state.set_bot_commands_storage_path(ToUtf8(botPath.wstring()));
            if (state.load_bot_commands_from_disk()) {
                LogLine(L"BOT: loaded commands from bot_commands.json");
            }
            else {
                LogLine(L"BOT: no bot_commands.json found (or empty/invalid) - starting with in-memory defaults");
            }
        }
        catch (...) {
            LogLine(L"BOT: failed to set/load bot commands storage path");
        }

        // -----------------------------------------------------------------
        // Bot safety settings persistence (2.3)
        // Store bot_settings.json next to the exe, and load it at startup.
        // -----------------------------------------------------------------
        try {
            std::filesystem::path setPath = std::filesystem::path(GetExeDir()) / "bot_settings.json";
            state.set_bot_settings_storage_path(ToUtf8(setPath.wstring()));
            if (state.load_bot_settings_from_disk()) {
                LogLine(L"BOT: loaded settings from bot_settings.json");
            }
            else {
                LogLine(L"BOT: no bot_settings.json found (or empty/invalid) - using defaults");
            }
        }
        catch (...) {
            LogLine(L"BOT: failed to set/load bot settings storage path");
        }


// -----------------------------------------------------------------
// Overlay header settings persistence
// Store overlay_header.json next to the exe, and load it at startup.
// -----------------------------------------------------------------
try {
    std::filesystem::path hdrPath = std::filesystem::path(GetExeDir()) / "overlay_header.json";
    state.set_overlay_header_storage_path(ToUtf8(hdrPath.wstring()));
    if (state.load_overlay_header_from_disk()) {
        LogLine(L"OVERLAY: loaded header settings from overlay_header.json");
    }
    else {
        LogLine(L"OVERLAY: no overlay_header.json found (or empty/invalid) - using defaults");
    }
}
catch (...) {
    LogLine(L"OVERLAY: failed to set/load overlay header settings path");
}

        // Allow LogLine() to feed the Web UI (/api/log)
        gStateForWebLog = &state;

        // -----------------------------------------------------------------
        // Bot command handler (injects into ChatAggregator)
        // -----------------------------------------------------------------
        // We subscribe once during WM_CREATE. When any chat message that begins
        // with '!' is added to the unified ChatAggregator, we attempt to resolve
        // a command and inject a single bot reply back into the same feed.
        //
        // NOTE:
        // - This does NOT send messages to Twitch/YouTube/TikTok yet.
        // - Role data (mod/broadcaster) is not available at this stage, so both
        //   are treated as false here. The test endpoint can simulate roles.
        if (!botSubscribed) {
            botSubscribed = true;
            chat.Subscribe([pChat=&chat, pState=&state, pTwitch=&twitch, pTikTok = &tiktok](const ChatMessage& m) {
                // Avoid responding to ourselves.
                if (m.user == "StreamingATC.Bot") return;
                if (m.message.size() < 2 || m.message[0] != '!') return;

                auto to_lower = [](std::string s) {
                    std::transform(s.begin(), s.end(), s.begin(),
                        [](unsigned char c) { return (char)std::tolower(c); });
                    return s;
                };
                auto replace_all = [](std::string s, const std::string& from, const std::string& to) {
                    if (from.empty()) return s;
                    size_t pos = 0;
                    while ((pos = s.find(from, pos)) != std::string::npos) {
                        s.replace(pos, from.size(), to);
                        pos += to.size();
                    }
                    return s;
                };

                // Extract first token after '!'
                size_t start = 1;
                while (start < m.message.size() && std::isspace((unsigned char)m.message[start])) start++;
                size_t end = start;
                while (end < m.message.size() && !std::isspace((unsigned char)m.message[end])) end++;
                if (end <= start) return;

                std::string cmd_lc = to_lower(m.message.substr(start, end - start));
                if (cmd_lc.empty()) return;

                const long long now_ms_ll = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                // Load bot safety settings (configurable via bot_settings.json).
                const AppState::BotSettings bot_settings = pState->bot_settings_snapshot();
                if (bot_settings.silent_mode) {
                    return;
                }

                // -----------------------------------------------------------------
                // Global safety limits (2.3): per-user + per-platform rate limiting
                // -----------------------------------------------------------------
                // These sit on top of per-command cooldowns to prevent spam/ban risk.
                // They apply regardless of command.
                static std::mutex rl_mu;
                static std::unordered_map<std::string, long long> last_by_user;
                static std::unordered_map<std::string, long long> last_by_platform;

                const long long kUserGapMs = (long long)bot_settings.per_user_gap_ms;
                const long long kPlatformGapMs = (long long)bot_settings.per_platform_gap_ms;

                std::string platform_lc = to_lower(m.platform);
                std::string user_key = platform_lc + "|" + m.user;

                {
                    std::lock_guard<std::mutex> lk(rl_mu);
                    if (kPlatformGapMs > 0) {
                        auto itp = last_by_platform.find(platform_lc);
                        if (itp != last_by_platform.end() && (now_ms_ll - itp->second) < kPlatformGapMs) {
                            return;
                        }
                    }
                    if (kUserGapMs > 0) {
                        auto itu = last_by_user.find(user_key);
                        if (itu != last_by_user.end() && (now_ms_ll - itu->second) < kUserGapMs) {
                            return;
                        }
                    }
                    // Reserve slots now (so parallel threads don't double-fire).
                    last_by_platform[platform_lc] = now_ms_ll;
                    last_by_user[user_key] = now_ms_ll;
                }

                // Enforce enabled/cooldown/scope (2.2: role flags carried on ChatMessage).
                std::string template_reply = pState->bot_try_get_response(
                    cmd_lc,
                    /*is_mod*/m.is_mod,
                    /*is_broadcaster*/m.is_broadcaster,
                    now_ms_ll
                );
                if (template_reply.empty()) return;

                std::string reply = template_reply;
                reply = replace_all(reply, "{user}", m.user);
                reply = replace_all(reply, "{platform}", platform_lc);

                // Clamp reply length for safety (platform limits vary; keep conservative).
                const size_t kMaxReplyLen = bot_settings.max_reply_len;
                if (kMaxReplyLen > 0 && reply.size() > kMaxReplyLen) {
                    reply.resize(kMaxReplyLen);
                }
                if (kMaxReplyLen == 0) {
                    // Explicitly configured to suppress replies.
                    return;
                }

                ChatMessage bot{};
                bot.platform = m.platform;
                bot.user = "StreamingATC.Bot";
                bot.message = reply;
                bot.ts_ms = (uint64_t)(now_ms_ll + 1);
                pChat->Add(std::move(bot));

                // Also send back to the origin platform (Twitch first).
                // This keeps overlay injection but makes the bot respond on-platform too.
                if (platform_lc == "twitch" && pTwitch) {
                    if (!pTwitch->SendPrivMsg(reply)) {
                        OutputDebugStringA("[BOT] Twitch send failed\n");
                    }
                }
                // Send back to the origin platform (Tiktok).
                if (platform_lc == "tiktok" && pTikTok) {
                    if (!pTikTok->send_chat(reply)) {
                        OutputDebugStringA("[BOT] TikTok send failed (sidecar)\n");
                    }
                }

            });
        }

        // Log what AppConfig actually loaded (helps diagnose mismatched JSON keys vs. AppConfig mapping).
        {
            std::wstring snap = L"CONFIG: AppConfig snapshot ";
            snap += L"twitch_login='";
            snap += ToW(config.twitch_login);
            snap += L"' twitch_client_id='";
            snap += ToW(config.twitch_client_id);
            snap += L"' twitch_client_secret_len=";
            snap += std::to_wstring(config.twitch_client_secret.size());
            LogLine(snap.c_str());
        }

        
        // If enabled, host the new modern UI (HTML/CSS) directly inside the main window via WebView2.
        // This keeps all existing backend threads + HTTP API intact, but replaces the legacy Win32 control layout.
#if HAVE_WEBVIEW2
        if (gUseModernUi) {
            // Create a hidden log control so existing LogLine() plumbing still works (optional).
            gLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

            if (!gFloatingChat) gFloatingChat = std::make_unique<FloatingChat>();

            HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
                nullptr, nullptr, nullptr,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [hwnd](HRESULT envResult, ICoreWebView2Environment* env) -> HRESULT {
                        if (FAILED(envResult) || !env) return envResult;
                        env->CreateCoreWebView2Controller(
                            hwnd,
                            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [hwnd](HRESULT ctlResult, ICoreWebView2Controller* controller) -> HRESULT {
                                    if (FAILED(ctlResult) || !controller) return ctlResult;

                                    gMainWebController = controller;

                                    ICoreWebView2* core = nullptr;
                                    gMainWebController->get_CoreWebView2(&core);
                                    if (core) {
                                        gMainWebView.Attach(core);

                                        {
                                            EventRegistrationToken tok{};
                                            gMainWebView->add_WebMessageReceived(
                                                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                                    [hwnd](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                                        LPWSTR jsonRaw = nullptr;
                                                        args->get_WebMessageAsJson(&jsonRaw);
                                                        std::wstring json = jsonRaw ? jsonRaw : L"";
                                                        if (jsonRaw) CoTaskMemFree(jsonRaw);

                                                        if (json.find(L"open_chat") != std::wstring::npos) {
                                                            if (gFloatingChat) gFloatingChat->Open(hwnd);
                                                        }
                                                        return S_OK;
                                                    }).Get(),
                                                        &tok);
                                        }

                                        {
                                            EventRegistrationToken tok{};
                                            gMainWebView->add_NewWindowRequested(
                                                Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                                    [hwnd](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                                        LPWSTR uriRaw = nullptr;
                                                        args->get_Uri(&uriRaw);
                                                        std::wstring uri = uriRaw ? uriRaw : L"";
                                                        if (uriRaw) CoTaskMemFree(uriRaw);

                                                        if (uri.find(L"/overlay/chat.html") != std::wstring::npos) {
                                                            if (gFloatingChat) gFloatingChat->Open(hwnd);
                                                            args->put_Handled(TRUE);
                                                        }
                                                        return S_OK;
                                                    }).Get(),
                                                        &tok);
                                        }

                                        ICoreWebView2Settings* settings = nullptr;
                                        gMainWebView->get_Settings(&settings);
                                        if (settings) {
                                            settings->put_IsStatusBarEnabled(FALSE);
                                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                                            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                                            settings->Release();
                                        }

                                        {
                                            std::wstring build = APP_VERSION_FILE_W;
                                            std::wstring js = L"window.__APP_BUILDINFO='" + build + L"';"
                                                L"document.addEventListener('DOMContentLoaded',function(){"
                                                L"var el=document.getElementById('buildInfo');"
                                                L"if(el){el.textContent=window.__APP_BUILDINFO;}"
                                                L"});";
                                            gMainWebView->AddScriptToExecuteOnDocumentCreated(js.c_str(), nullptr);
                                        }

                                        if (gHttpReady.load()) {
                                            gMainWebView->Navigate(kModernUiUrl);
                                        }
                                    }

                                    gMainWebController->put_IsVisible(TRUE);
                                    RECT rc; GetClientRect(hwnd, &rc);
                                    gMainWebController->put_Bounds(rc);
                                    return S_OK;
                                }).Get());
                        return S_OK;
                    }).Get());
            (void)hr;

            LogLine(L"Starting Mode-S Client overlay");
            LogLine(L"Overlay: http://localhost:17845/overlay/chat.html");
            LogLine(L"Metrics: http://localhost:17845/api/metrics");

            if (config.tiktok_unique_id.empty() && config.twitch_login.empty() && config.youtube_handle.empty()) {
                LogLine(L"No config.json found yet. Configure your platform details in the Settings page.");
            }
            else {
                LogLine(L"Loaded config.json");
            }

            PostMessageW(hwnd, WM_APP + 1, 0, 0);
            return 0;
        }
#endif
        return 0;
    }
    case WM_APP + 1:
    {
        // Start HTTP server in background thread

        // Start HTTP server (API + overlay)
        {
            HttpServer::Options opt;
            opt.bind_host = "127.0.0.1";
            opt.port = 17845;
            opt.overlay_root = std::filesystem::path(GetExeDir()) / "assets" / "overlay";
            // Platform control callbacks for the /app Web UI
            opt.start_tiktok = [&]() -> bool {
                config.tiktok_unique_id = SanitizeTikTok(config.tiktok_unique_id);
                if (config.tiktok_unique_id.empty()) {
                    LogLine(L"TIKTOK: username is empty - refusing to start");
                    return false;
                }

                return PlatformControl::StartOrRestartTikTokSidecar(
                    tiktok, state, chat,
                    GetExeDir(),
                    config.tiktok_unique_id,
                    hwnd,
                    [](const std::wstring& s) { LogLine(s); });
            };
            opt.stop_tiktok = [&]() -> bool {
                PlatformControl::StopTikTok(tiktok, state, hwnd, 0, [](const std::wstring& s) { LogLine(s); });
                return true;
            };

            opt.start_twitch = [&]() -> bool {
                config.twitch_login = SanitizeTwitchLogin(config.twitch_login);
                if (config.twitch_login.empty()) {
                    LogLine(L"TWITCH: channel login is empty - refusing to start");
                    return false;
                }

                std::string token = twitchAuth.GetAccessToken().value_or("");
                if (token.empty()) {
                    LogLine(L"TWITCH: token not available yet (auth not ready) - refusing to start");
                    return false;
                }

                if (gTwitchHelixBoundLogin != config.twitch_login) {
                    RestartTwitchHelixPoller("web dashboard start");
                }

                const bool ok = PlatformControl::StartOrRestartTwitchIrc(
                    twitch, state, chat,
                    config.twitch_login,
                    token,
                    [](const std::wstring& s) { LogLine(s); });

                if (!ok) return false;

                twitchEventSub.Start(
                    config.twitch_client_id,
                    token,
                    config.twitch_login,
                    [&](const ChatMessage& msg) {
                        ChatMessage c = msg;
                        c.is_event = true;
                        chat.Add(std::move(c));
                    },
                    [&](const nlohmann::json& ev) { state.add_twitch_eventsub_event(ev); },
                    [&](const nlohmann::json& st) {
                        state.set_twitch_eventsub_status(st);

                        // Optional: push “new” error events into AppState ring buffer
                        static std::uint64_t last_seq = 0;
                        const std::uint64_t seq = st.value("last_error_seq", (std::uint64_t)0);
                        if (seq != 0 && seq != last_seq) {
                            last_seq = seq;
                            const std::string msg = st.value("last_error", std::string{});
                            if (!msg.empty()) state.push_twitch_eventsub_error(msg);
                        }
                    }
                );

                return true;
                };
            opt.stop_twitch = [&]() -> bool {
                PlatformControl::StopTwitch(twitch, state, hwnd, 0, [](const std::wstring& s) { LogLine(s); });
                return true;
            };

            opt.start_youtube = [&]() -> bool {
                config.youtube_handle = SanitizeYouTubeHandle(config.youtube_handle);
                if (config.youtube_handle.empty()) {
                    LogLine(L"YOUTUBE: handle/channel is empty - refusing to start");
                    return false;
                }

                const bool ok = PlatformControl::StartOrRestartYouTubeSidecar(
                    youtube, state, chat,
                    GetExeDir(),
                    config.youtube_handle,
                    hwnd,
                    [](const std::wstring& s) { LogLine(s); });

                if (!ok) return false;

                youtubeChat.start(config.youtube_handle, chat,
                    [](const std::wstring& s) { LogLine(s); },
                    &state);

                return true;
                };

            opt.stop_youtube = [&]() -> bool {
                PlatformControl::StopYouTube(youtube, state, hwnd, 0, [](const std::wstring& s) { LogLine(s); });
                youtubeChat.stop();
                return true;
            };

            // Twitch OAuth (interactive) endpoints
            // Provides /auth/twitch/start and /auth/twitch/callback so you can (re)authorize with chat:read/chat:edit.
            opt.twitch_auth_build_authorize_url = [&](const std::string& redirect_uri, std::string* out_error) -> std::string {
                std::string err;
                const std::string url = twitchAuth.BuildAuthorizeUrl(redirect_uri, &err);
                if (url.empty()) {
                    if (out_error) *out_error = err;
                    LogLine(ToW(std::string("TWITCHAUTH: BuildAuthorizeUrl failed: ") + err));
                }
                return url;
            };

            opt.twitch_auth_handle_callback = [&](const std::string& code,
                                                  const std::string& state,
                                                  const std::string& redirect_uri,
                                                  std::string* out_error) -> bool {
                 std::string err;
                 const bool ok = twitchAuth.HandleOAuthCallback(code, state, redirect_uri, &err);
                if (!ok) {
                    if (out_error) *out_error = err;
                    LogLine(ToW(std::string("TWITCHAUTH: OAuth callback failed: ") + err));
                }
                return ok;
            };

            // YouTube OAuth (interactive) endpoints
            // Provides /auth/youtube/start and /auth/youtube/callback so you can authorize YouTube Data API access.
            opt.youtube_auth_build_authorize_url = [&](const std::string& redirect_uri, std::string* out_error) -> std::string {
                std::string err;
                const std::string url = youtubeAuth.BuildAuthorizeUrl(redirect_uri, &err);
                if (url.empty()) {
                    if (out_error) *out_error = err;
                    LogLine(ToW(std::string("YTAUTH: BuildAuthorizeUrl failed: ") + err));
                }
                return url;
            };

            opt.youtube_auth_handle_callback = [&](const std::string& code,
                                                   const std::string& state,
                                                   const std::string& redirect_uri,
                                                   std::string* out_error) -> bool {
                std::string err;
                const bool ok = youtubeAuth.HandleOAuthCallback(code, state, redirect_uri, &err);
                if (!ok) {
                    if (out_error) *out_error = err;
                    LogLine(ToW(std::string("YTAUTH: OAuth callback failed: ") + err));
                }
                return ok;
            };

            // YouTube access token provider (used by /api/youtube/vod/* endpoints)
            opt.youtube_get_access_token = []() -> std::optional<std::string> {
                return youtubeAuth.GetAccessToken();
            };

            // YouTube OAuth status (read-only): used by the UI to show "connected" vs "not connected".
            // IMPORTANT: Do not return tokens here; only booleans + non-sensitive metadata.
            opt.youtube_auth_info_json = []() {
                nlohmann::json j;
                j["ok"] = true;
                j["start_url"] = "/auth/youtube/start";
                j["oauth_routes_wired"] = true;

                const auto snap = youtubeAuth.GetTokenSnapshot();
                j["has_refresh_token"] = snap.has_value() && !snap->refresh_token.empty();
                j["has_access_token"]  = snap.has_value() && !snap->access_token.empty();
                j["expires_at_unix"]   = snap.has_value() ? snap->expires_at_unix : 0;
                j["scope"]             = snap.has_value() ? snap->scope_joined : "";
                j["channel_id"]        = youtubeAuth.GetChannelId().value_or("");

                // Keep existing fields for callers that display scopes (and for debug).
                j["scopes_readable"] = std::string(YouTubeAuth::RequiredScopeReadable());
                j["scopes_encoded"] = std::string(YouTubeAuth::RequiredScopeEncoded());
                return j.dump(2);
            };


            gHttp = std::make_unique<HttpServer>(state, chat, euroscope, config, opt,
                [](const std::wstring& s) { LogLine(s); });

            gHttp->Start();
        }

        // Signal that the HTTP server is ready for WebView2 navigation.
        gHttpReady = true;
#if HAVE_WEBVIEW2
        if (gUseModernUi && gMainWebView) {
            gMainWebView->Navigate(kModernUiUrl);
        }
#endif


        metricsThread = std::thread([&]() {
            while (gRunning) {
                auto m = state.get_metrics();
                obs.set_text("TOTAL_VIEWER_COUNT", std::to_string(m.total_viewers()));
                obs.set_text("TOTAL_FOLLOWER_COUNT", std::to_string(m.total_followers()));
                Sleep(5000);
            }
            });

        
            if (!youtubeAuth.Start()) {
                LogLine(L"YOUTUBE: OAuth token refresh/start failed (check config: youtube.client_id / youtube.client_secret / youtube.refresh_token)");
            } else {
                LogLine(L"YOUTUBE: OAuth token refresh/start OK");
            }

LogLine(L"TWITCH: starting Helix poller thread");

        // Bind the poller to the current config.twitch_login.
        RestartTwitchHelixPoller("init");
LogLine(L"TIKTOK: starting followers poller thread");
        tiktokFollowersThread = StartTikTokFollowersPoller(
            hwnd,
            config,
            state,
            gRunning,
            0,
            TikTokFollowersUiCallbacks{
                /*log*/           [](const std::wstring& s) { LogLine(s); },
                /*set_status*/    [](const std::wstring& /*s*/) { /* optional */ },
                /*set_followers*/ [&](int /*f*/) {}
            }
        );
        // Twitch OAuth token refresh (silent) - runs during boot while splash is visible
        {
            LogLine(L"TWITCH: refreshing OAuth token...");
            std::string auth_err;
            // When TwitchAuth refreshes tokens, push the new access token into IRC + EventSub.
// When TwitchAuth refreshes tokens, push the new access token into IRC + EventSub.
            twitchAuth.on_tokens_updated = [&](const std::string& access,
                const std::string& /*refresh*/,
                const std::string& login) {
                    LogLine(L"TWITCH: tokens updated - restarting EventSub + IRC");

                    // Defensive: validated login can be empty depending on validate/refresh edge cases.
                    const std::string effective_login = !login.empty() ? login : config.twitch_login;

                    twitchEventSub.UpdateAccessToken(access);
                    twitchEventSub.Stop();
                    twitchEventSub.Start(
                        config.twitch_client_id,
                        access,
                        config.twitch_login,

                        // EventSub -> Chat (optional messages you inject)
                        [&](const ChatMessage& msg) {
                            ChatMessage c = msg;
                            c.is_event = true;
                            chat.Add(std::move(c));
                        },

                        // EventSub -> Alerts overlay
                        [&](const nlohmann::json& ev) { state.add_twitch_eventsub_event(ev); },

                        // EventSub -> Status/health (THIS is what makes /api/twitch/eventsub/status meaningful)
                        [&](const nlohmann::json& st) {
                            state.set_twitch_eventsub_status(st);

                            // Push new error events into AppState's ring buffer (optional but useful)
                            static std::uint64_t last_seq = 0;
                            const std::uint64_t seq = st.value("last_error_seq", (std::uint64_t)0);
                            if (seq != 0 && seq != last_seq) {
                                last_seq = seq;
                                const std::string msg = st.value("last_error", std::string{});
                                if (!msg.empty()) state.push_twitch_eventsub_error(msg);
                            }
                        }
                    );

                    PlatformControl::StartOrRestartTwitchIrc(
                        twitch, state, chat,
                        effective_login,
                        access,
                        [](const std::wstring& s) { LogLine(s); }
                    );
                };

            if (!twitchAuth.Start()) {
                LogLine(L"TWITCH: OAuth token refresh/start failed (check config: twitch_client_id / twitch_client_secret / twitch.user_refresh_token)");
            } else {
                LogLine(L"TWITCH: OAuth token refresh worker started");
            }
        }

        // Signal that the app has finished initializing and the main window can be shown.
        PostMessageW(hwnd, WM_APP_SPLASH_READY, 0, 0);

        return 0;
    }

    case WM_APP_LOG:
    {
        auto* heap = reinterpret_cast<std::wstring*>(lParam);
        if (heap) {
            AppendLogOnUiThread(*heap);
            delete heap;
        }
        return 0;
    }

    case WM_APP_SPLASH_READY:
    {
        // Background init has completed; keep splash visible briefly (test) then show main UI.
        if (gSplashWnd && IsWindow(gSplashWnd)) {
            // Start (or restart) the close timer on the splash window.
            SetTimer(gSplashWnd, SPLASH_CLOSE_TIMER_ID, SPLASH_CLOSE_DELAY_MS, nullptr);
        }
        else {
            // Fallback: if splash doesn't exist, just show the main UI immediately.
            ShowWindow(hwnd, SW_SHOWDEFAULT);
            UpdateWindow(hwnd);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }

case WM_CLOSE:
    BeginShutdown(hwnd);
    return 0;

case WM_SIZE:
#if HAVE_WEBVIEW2
        if (gUseModernUi && gMainWebController) {
            RECT rc; GetClientRect(hwnd, &rc);
            gMainWebController->put_Bounds(rc);
            return 0;
        }
#endif

    case WM_DESTROY:
        // Safety net: if we somehow got here without WM_CLOSE
        BeginShutdown(nullptr);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    gUiThreadId = GetCurrentThreadId();

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
            wsprintfW(buf, L"RegisterClassW failed. GetLastError=%lu", e);
            MessageBoxW(nullptr, buf, L"Mode-S Client", MB_ICONERROR);
            return 0;
        }
    }

    // Splash screen shown while the main window initializes
    gSplashWnd = CreateSplashWindow(hInstance);

    // Process any pending messages so the splash paints immediately.
    MSG sm{};
    while (PeekMessageW(&sm, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&sm);
        DispatchMessageW(&sm);
    }

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, kAppDisplayName,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900,
        nullptr, nullptr, hInstance, nullptr
    );

    gMainWnd = hwnd;

    // Set window title to include build/file version
    {
        std::wstring title = std::wstring(kAppDisplayName) + L" " + APP_VERSION_FILE_W;
        SetWindowTextW(hwnd, title.c_str());
    }


    if (!hwnd) {
        DWORD e = GetLastError();
        wchar_t buf[256];
        wsprintfW(buf, L"CreateWindowExW failed. GetLastError=%lu", e);
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

    return 0;
}