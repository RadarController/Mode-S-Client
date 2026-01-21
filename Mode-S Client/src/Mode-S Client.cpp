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
#include "AppConfig.h"
#include "AppState.h"
#include "http/HttpServer.h"
#include "chat/ChatAggregator.h"
#include "twitch/TwitchHelixService.h"
#include "twitch/TwitchIrcWsClient.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchAuth.h"
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

// Flip this to false to revert to the legacy Win32 control UI.
static bool gUseModernUi = true;
static std::atomic<bool> gHttpReady{ false };
static const wchar_t* kModernUiUrl = L"http://127.0.0.1:17845/app";


// --------------------------- Control IDs ------------------------------------
#define IDC_TIKTOK_EDIT        1001
#define IDC_TWITCH_EDIT        1002
#define IDC_YOUTUBE_EDIT       1003
#define IDC_SAVE_BTN           1004
#define IDC_START_TIKTOK       1005
#define IDC_RESTART_TIKTOK     1006
#define IDC_START_TWITCH       1007
#define IDC_RESTART_TWITCH     1008
#define IDC_TIKTOK_COOKIES     1009
#define IDC_TWITCH_SETTINGS    1014
#define IDC_START_YOUTUBE      1010
#define IDC_RESTART_YOUTUBE    1011
#define IDC_CLEAR_LOG          1012
#define IDC_COPY_LOG           1013

// --------------------------- Theme (Material-ish dark) ----------------------
static COLORREF gClrBg = RGB(18, 18, 18);
static COLORREF gClrPanel = RGB(24, 24, 24);
static COLORREF gClrEditBg = RGB(32, 32, 32);
static COLORREF gClrText = RGB(230, 230, 230);
static COLORREF gClrHint = RGB(170, 170, 170);

static HBRUSH gBrushBg = nullptr;
static HBRUSH gBrushPanel = nullptr;
static HBRUSH gBrushEdit = nullptr;
static HFONT  gFontUi = nullptr;

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
static const wchar_t* kAppVersion = L"v0.4.0"; // TODO: wire to resource/file version

static HWND hGroupTikTok = nullptr, hGroupTwitch = nullptr, hGroupYouTube = nullptr;
static HWND hLblTikTok = nullptr, hLblTwitch = nullptr, hLblYouTube = nullptr;
static HWND hHint = nullptr;
static HWND hGroupSettings = nullptr;

static HWND hTikTok = nullptr, hTwitch = nullptr, hYouTube = nullptr;
static HWND hTikTokCookies = nullptr;
static HWND hTwitchSettings = nullptr;

static HWND hSave = nullptr;
static HWND hStartTikTokBtn = nullptr, hRestartTikTokBtn = nullptr;
static HWND hStartTwitchBtn = nullptr, hRestartTwitchBtn = nullptr;
static HWND hStartYouTubeBtn = nullptr, hRestartYouTubeBtn = nullptr;

static HWND hClearLogBtn = nullptr, hCopyLogBtn = nullptr;

// Button to open a floating chat window
static HWND hOpenFloatingChatBtn = nullptr;

// Floating chat instance
// Moved to FloatingChat class in src/floating/FloatingChat.*
static std::unique_ptr<class FloatingChat> gFloatingChat;

static std::atomic<bool> gRunning{ true };


// Helix poller is restartable independently of full app shutdown.
static std::atomic<bool> gTwitchHelixRunning{ true };
static std::string gTwitchHelixBoundLogin;
// --- Platform status widgets ---
static HWND gTikTokStatus = nullptr, gTikTokViewers = nullptr, gTikTokFollowers = nullptr;
static HWND gTwitchStatus = nullptr, gTwitchViewers = nullptr, gTwitchFollowers = nullptr, gTwitchHelix = nullptr;
static std::wstring gTwitchHelixStatus = L"Helix: (idle)";
static HWND gYouTubeStatus = nullptr, gYouTubeViewers = nullptr, gYouTubeFollowers = nullptr;

// --- Platform state (UI only for now) ---
static bool gTikTokLive = false, gTwitchLive = false, gYouTubeLive = false;
static int  gTikTokViewerCount = 0, gTwitchViewerCount = 0, gYouTubeViewerCount = 0;
static int  gTikTokFollowerCount = 0, gTwitchFollowerCount = 0, gYouTubeFollowerCount = 0;
// Forward decl
static void LayoutControls(HWND hwnd);
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

// --------------------------- Twitch EventSub (WebSocket) --------------------
// Receives on-stream events (follow, sub, gift sub) and forwards them into ChatAggregator
// as synthetic chat messages so the overlay can render them interleaved with chat.


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

static std::wstring GetWindowTextWString(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return L"";
    std::wstring w;
    // Allocate len+1 to receive the null terminator safely, then resize to actual copied length.
    w.resize(len + 1);
    int copied = GetWindowTextW(h, &w[0], len + 1);
    if (copied < 0) return L"";
    w.resize(copied);
    return w;
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

static void UpdateTikTokButtons(HWND hTikTokEdit, HWND hStartBtn, HWND hRestartBtn)
{
    if (!hTikTokEdit) return;
    auto raw = ToUtf8(GetWindowTextWString(hTikTokEdit));
    auto cleaned = SanitizeTikTok(raw);
    BOOL enable = !cleaned.empty();
    if (hStartBtn)   EnableWindow(hStartBtn, enable);
    // Restart/Stop button is disabled for now (Stop functionality not implemented yet).
    if (hRestartBtn) EnableWindow(hRestartBtn, FALSE);
}


static void UpdateYouTubeButtons(HWND hYouTubeEdit, HWND hStartBtn, HWND hRestartBtn)
{
    if (!hYouTubeEdit) return;
    auto raw = ToUtf8(GetWindowTextWString(hYouTubeEdit));
    auto cleaned = SanitizeYouTubeHandle(raw);
    BOOL enable = !cleaned.empty();
    if (hStartBtn)   EnableWindow(hStartBtn, enable);
    // Restart/Stop button is disabled for now (Stop functionality not implemented yet).
    if (hRestartBtn) EnableWindow(hRestartBtn, FALSE);
}

static void UpdateTwitchButtons(HWND hTwitchEdit, HWND hStartBtn, HWND hStopBtn)
{
    if (!hTwitchEdit) return;
    auto raw = ToUtf8(GetWindowTextWString(hTwitchEdit));
    auto cleaned = SanitizeTwitchLogin(raw);
    BOOL enable = !cleaned.empty();
    if (hStartBtn) EnableWindow(hStartBtn, enable);
    // Stop button is disabled for now (Stop functionality not implemented yet).
    if (hStopBtn) EnableWindow(hStopBtn, FALSE);
}



static void UpdatePlatformStatusUI(const Metrics& m)
{
    auto set = [](HWND h, const std::wstring& s) { if (h) SetWindowTextW(h, s.c_str()); };

    // Pull latest values from AppState metrics (single source of truth for UI)
    gTikTokViewerCount = m.tiktok_viewers;
    gTikTokFollowerCount = m.tiktok_followers;
    gTikTokLive = m.tiktok_live;

    gTwitchViewerCount = m.twitch_viewers;
    gTwitchFollowerCount = m.twitch_followers;
    gTwitchLive = m.twitch_live;

    gYouTubeViewerCount = m.youtube_viewers;
    gYouTubeFollowerCount = m.youtube_followers;
    gYouTubeLive = m.youtube_live;
    set(gTikTokStatus, gTikTokLive ? L"Status: LIVE" : L"Status: OFFLINE");
    set(gTikTokViewers, L"Viewers: " + std::to_wstring(gTikTokViewerCount));
    set(gTikTokFollowers, L"Followers: " + std::to_wstring(gTikTokFollowerCount));

    set(gTwitchStatus, gTwitchLive ? L"Status: LIVE" : L"Status: OFFLINE");
    set(gTwitchViewers, L"Viewers: " + std::to_wstring(gTwitchViewerCount));
    set(gTwitchFollowers, L"Followers: " + std::to_wstring(gTwitchFollowerCount));
    set(gTwitchHelix, gTwitchHelixStatus);

    set(gYouTubeStatus, gYouTubeLive ? L"Status: LIVE" : L"Status: OFFLINE");
    set(gYouTubeViewers, L"Viewers: " + std::to_wstring(gYouTubeViewerCount));
    set(gYouTubeFollowers, L"Followers: " + std::to_wstring(gYouTubeFollowerCount));
}

// Pull metrics through the local HTTP endpoint so the UI reflects exactly what overlays/OBS see.
static bool TryFetchMetricsFromApi(Metrics& out)
{
    HttpResult r = WinHttpRequest(L"GET", L"127.0.0.1", 17845, L"/api/metrics", L"", "", false);
    if (r.status != 200 || r.body.empty()) return false;

    try {
        auto j = nlohmann::json::parse(r.body);

        out.ts_ms = j.value("ts_ms", 0LL);
        out.twitch_viewers = j.value("twitch_viewers", 0);
        out.youtube_viewers = j.value("youtube_viewers", 0);
        out.tiktok_viewers = j.value("tiktok_viewers", 0);

        out.twitch_followers = j.value("twitch_followers", 0);
        out.youtube_followers = j.value("youtube_followers", 0);
        out.tiktok_followers = j.value("tiktok_followers", 0);

        out.twitch_live = j.value("twitch_live", false);
        out.youtube_live = j.value("youtube_live", false);
        out.tiktok_live = j.value("tiktok_live", false);
        return true;
    }
    catch (...) {
        return false;
    }
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

    case WM_DESTROY:
    {
        // cleanup fonts stored as properties
        if (auto h = (HFONT)GetPropW(hwnd, L"splash_font_title")) { DeleteObject(h); RemovePropW(hwnd, L"splash_font_title"); }
        if (auto h = (HFONT)GetPropW(hwnd, L"splash_font_body")) { DeleteObject(h); RemovePropW(hwnd, L"splash_font_body"); }
        gSplashLog = nullptr;
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




// --------------------------- TikTok cookie modal ----------------------------
struct TikTokCookiesDraft {
    std::wstring sessionid;
    std::wstring sessionid_ss;
    std::wstring tt_target_idc;
    bool accepted = false;
};

static LRESULT CALLBACK TikTokCookiesWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* draft = reinterpret_cast<TikTokCookiesDraft*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    static HWND hSession = nullptr, hSessionSS = nullptr, hTarget = nullptr;

    switch (msg)
    {
    case WM_CREATE:
    {

        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        draft = reinterpret_cast<TikTokCookiesDraft*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(draft));

        CreateWindowW(L"STATIC", L"tiktok_sessionid:", WS_CHILD | WS_VISIBLE,
            12, 14, 120, 18, hwnd, nullptr, nullptr, nullptr);
        hSession = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft->sessionid.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
            140, 10, 320, 24, hwnd, (HMENU)101, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"tiktok_sessionid_ss:", WS_CHILD | WS_VISIBLE,
            12, 46, 120, 18, hwnd, nullptr, nullptr, nullptr);
        hSessionSS = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft->sessionid_ss.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
            140, 42, 320, 24, hwnd, (HMENU)102, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"tiktok_tt_target_idc:", WS_CHILD | WS_VISIBLE,
            12, 78, 120, 18, hwnd, nullptr, nullptr, nullptr);
        hTarget = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft->tt_target_idc.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            140, 74, 160, 24, hwnd, (HMENU)103, nullptr, nullptr);

        CreateWindowW(L"STATIC",
            L"Tip: Paste these from your TikTok cookies (sessionid, sessionid_ss, tt_target_idc).",
            WS_CHILD | WS_VISIBLE,
            12, 106, 448, 18, hwnd, nullptr, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE,
            280, 134, 80, 26, hwnd, (HMENU)IDOK, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            380, 134, 80, 26, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if ((id == IDOK || id == IDCANCEL) && code == BN_CLICKED)
        {
            if (draft && id == IDOK)
            {
                wchar_t buf[512]{};

                GetWindowTextW(hSession, buf, (int)_countof(buf));
                draft->sessionid = buf;

                GetWindowTextW(hSessionSS, buf, (int)_countof(buf));
                draft->sessionid_ss = buf;

                GetWindowTextW(hTarget, buf, (int)_countof(buf));
                draft->tt_target_idc = buf;

                draft->accepted = true;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool EditTikTokCookiesModal(HWND parent, TikTokCookiesDraft& draft)
{
    const wchar_t* kClass = L"StreamHub_TikTokCookiesDlg";

    static std::atomic<bool> registered{ false };
    if (!registered.exchange(true))
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TikTokCookiesWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_ICON1));
        RegisterClassW(&wc);
    }

    RECT pr{};
    GetWindowRect(parent, &pr);

    const int w = 480, h = 200;
    const int x = pr.left + ((pr.right - pr.left) - w) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    EnableWindow(parent, FALSE);

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kClass, L"TikTok Session Cookies",
        WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        parent, nullptr, GetModuleHandleW(nullptr),
        &draft);

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg{};
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(dlg, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    return draft.accepted;
}


// --------------------------- Twitch settings modal -------------------------
struct TwitchSettingsDraft {
    std::wstring login;
    std::wstring client_id;
    std::wstring client_secret;
    bool accepted = false;
};

static LRESULT CALLBACK TwitchSettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HWND hLogin = nullptr, hClientId = nullptr, hSecret = nullptr;

    TwitchSettingsDraft* draft = reinterpret_cast<TwitchSettingsDraft*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        draft = reinterpret_cast<TwitchSettingsDraft*>(cs ? cs->lpCreateParams : nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(draft));

        if (!draft) return -1;

        CreateWindowW(L"STATIC", L"Channel login:", WS_CHILD | WS_VISIBLE,
            12, 14, 120, 18, hwnd, nullptr, nullptr, nullptr);
        hLogin = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft->login.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            140, 10, 320, 24, hwnd, (HMENU)1, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Client ID:", WS_CHILD | WS_VISIBLE,
            12, 46, 120, 18, hwnd, nullptr, nullptr, nullptr);
        hClientId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft->client_id.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            140, 42, 320, 24, hwnd, (HMENU)2, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Client Secret:", WS_CHILD | WS_VISIBLE,
            12, 78, 120, 18, hwnd, nullptr, nullptr, nullptr);
        hSecret = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", draft->client_secret.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
            140, 74, 320, 24, hwnd, (HMENU)3, nullptr, nullptr);

        CreateWindowW(L"STATIC",
            L"Tip: Create an app at dev.twitch.tv/console to get Client ID and Secret.",
            WS_CHILD | WS_VISIBLE,
            12, 106, 448, 18, hwnd, nullptr, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE,
            280, 134, 80, 26, hwnd, (HMENU)IDOK, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            380, 134, 80, 26, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if ((id == IDOK || id == IDCANCEL) && code == BN_CLICKED)
        {
            if (draft && id == IDOK)
            {
                wchar_t buf[1024]{};

                GetWindowTextW(hLogin, buf, (int)_countof(buf));
                draft->login = buf;

                GetWindowTextW(hClientId, buf, (int)_countof(buf));
                draft->client_id = buf;

                GetWindowTextW(hSecret, buf, (int)_countof(buf));
                draft->client_secret = buf;

                draft->accepted = true;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool EditTwitchSettingsModal(HWND parent, TwitchSettingsDraft& draft)
{
    const wchar_t* kClass = L"StreamHub_TwitchSettingsDlg";

    static std::atomic<bool> registered{ false };
    if (!registered.exchange(true))
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TwitchSettingsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_ICON1));
        RegisterClassW(&wc);
    }

    RECT pr{};
    GetWindowRect(parent, &pr);

    const int w = 480, h = 200;
    const int x = pr.left + ((pr.right - pr.left) - w) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    EnableWindow(parent, FALSE);

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kClass, L"Twitch Settings",
        WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        parent, nullptr, GetModuleHandleW(nullptr),
        &draft);

    if (!dlg) {
        EnableWindow(parent, TRUE);
        return false;
    }

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg{};
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(dlg, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    return draft.accepted;
}

// --------------------------- Platform start helpers -------------------------
static bool StartOrRestartTikTokSidecar(TikTokSidecar& tiktok,
    AppState& state,
    ChatAggregator& chat,
    HWND hwndMain,
    HWND hwndLog,
    HWND hTikTokEdit)
{
    std::string raw = ToUtf8(GetWindowTextWString(hTikTokEdit));
    std::string cleaned = SanitizeTikTok(raw);
    SetWindowTextW(hTikTokEdit, ToW(cleaned).c_str());

    return PlatformControl::StartOrRestartTikTokSidecar(
        tiktok, state, chat,
        GetExeDir(),
        cleaned,
        hwndMain,
        [](const std::wstring& s) { LogLine(s); });
}


static bool StartOrRestartYouTubeSidecar(TikTokSidecar& youtube,
    AppState& state,
    ChatAggregator& chat,
    HWND hwndMain,
    HWND hwndLog,
    HWND hYouTubeEdit)
{
    std::string raw = ToUtf8(GetWindowTextWString(hYouTubeEdit));
    std::string cleaned = SanitizeYouTubeHandle(raw);
    SetWindowTextW(hYouTubeEdit, ToW(cleaned).c_str());

    return PlatformControl::StartOrRestartYouTubeSidecar(
        youtube, state, chat,
        GetExeDir(),
        cleaned,
        hwndMain,
        [](const std::wstring& s) { LogLine(s); });
}

// --------------------------- Clipboard helper -------------------------------
static void CopyLogToClipboard(HWND hwndOwner)
{
    if (!gLog) return;
    int len = GetWindowTextLengthW(gLog);
    if (len <= 0) return;

    std::wstring text;
    text.resize((size_t)len);
    GetWindowTextW(gLog, text.data(), len + 1);

    if (!OpenClipboard(hwndOwner)) return;
    EmptyClipboard();

    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* p = GlobalLock(hMem);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
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

// --------------------------- Floating Chat Window ---------------------------
// Floating chat window is now implemented in src/floating/FloatingChat.*
// --------------------------- Layout -----------------------------------------
static void LayoutControls(HWND hwnd)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    const int pad = 16;
    const int gap = 12;

    const int topH = 190;
    int colW = (W - pad * 2 - gap * 2) / 3;
    if (colW < 280) colW = 280;

    int x0 = pad;
    int y0 = pad;

    // Group boxes
    MoveWindow(hGroupTikTok, x0, y0, colW, topH, TRUE);
    MoveWindow(hGroupTwitch, x0 + (colW + gap), y0, colW, topH, TRUE);
    MoveWindow(hGroupYouTube, x0 + 2 * (colW + gap), y0, colW, topH, TRUE);

    const int innerPad = 14;
    const int labelH = 18;
    const int editH = 26;
    const int btnH = 28;
    const int rowGap = 10;

    // TikTok column
    {
        int x = x0 + innerPad;
        int y = y0 + 28;
        int w = colW - innerPad * 2;

        MoveWindow(hLblTikTok, x, y, w, labelH, TRUE);
        y += labelH + 6;

        int editW = w - 32 - 8;
        MoveWindow(hTikTok, x, y, editW, editH, TRUE);
        MoveWindow(hTikTokCookies, x + editW + 8, y, 32, editH, TRUE);
        y += editH + rowGap;

        int bw = (w - gap) / 2;
        MoveWindow(hStartTikTokBtn, x, y, bw, btnH, TRUE);
        MoveWindow(hRestartTikTokBtn, x + bw + gap, y, bw, btnH, TRUE);
        y += btnH + rowGap;

        int infoX = x;
        int infoY = y;
        int infoW = w;
        MoveWindow(gTikTokStatus, infoX, infoY + 0, infoW, 18, TRUE);
        MoveWindow(gTikTokViewers, infoX, infoY + 20, infoW, 18, TRUE);
        MoveWindow(gTikTokFollowers, infoX, infoY + 40, infoW, 18, TRUE);
    }

    // Twitch column
    {
        int x = x0 + (colW + gap) + innerPad;
        int y = y0 + 28;
        int w = colW - innerPad * 2;

        MoveWindow(hLblTwitch, x, y, w, labelH, TRUE);
        y += labelH + 6;

        int editW = w - 32 - 8;
        MoveWindow(hTwitch, x, y, editW, editH, TRUE);
        MoveWindow(hTwitchSettings, x + editW + 8, y, 32, editH, TRUE);
        y += editH + rowGap;

        int bw = (w - gap) / 2;
        MoveWindow(hStartTwitchBtn, x, y, bw, btnH, TRUE);
        MoveWindow(hRestartTwitchBtn, x + bw + gap, y, bw, btnH, TRUE);
        y += btnH + rowGap;

        int infoX = x;
        int infoY = y;
        int infoW = w;
        MoveWindow(gTwitchStatus, infoX, infoY + 0, infoW, 18, TRUE);
        MoveWindow(gTwitchViewers, infoX, infoY + 20, infoW, 18, TRUE);
        MoveWindow(gTwitchFollowers, infoX, infoY + 40, infoW, 18, TRUE);

    }

    // YouTube column
    {
        int x = x0 + 2 * (colW + gap) + innerPad;
        int y = y0 + 28;
        int w = colW - innerPad * 2;

        MoveWindow(hLblYouTube, x, y, w, labelH, TRUE);
        y += labelH + 6;

        MoveWindow(hYouTube, x, y, w, editH, TRUE);
        y += editH + rowGap;

        int bw = (w - gap) / 2;
        MoveWindow(hStartYouTubeBtn, x, y, bw, btnH, TRUE);
        MoveWindow(hRestartYouTubeBtn, x + bw + gap, y, bw, btnH, TRUE);
        y += btnH + rowGap;

        int infoX = x;
        int infoY = y;
        int infoW = w;
        MoveWindow(gYouTubeStatus, infoX, infoY + 0, infoW, 18, TRUE);
        MoveWindow(gYouTubeViewers, infoX, infoY + 20, infoW, 18, TRUE);
        MoveWindow(gYouTubeFollowers, infoX, infoY + 40, infoW, 18, TRUE);

    }

    // Hint line
    int hintY = y0 + topH + 6;
    MoveWindow(hHint, pad, hintY, W - pad * 2, 18, TRUE);

    // Settings section (Save button)
    int settingsTop = hintY + 22;
    int settingsH = 58;
    MoveWindow(hGroupSettings, pad, settingsTop, W - pad * 2, settingsH, TRUE);

    // Save button inside settings section
    int savePad = 12;
    int saveY = settingsTop + 22;
    MoveWindow(hSave, pad + savePad, saveY, (W - pad * 2) - savePad * 2, 28, TRUE);

    // Log tools (below Settings block)
    int toolsY = settingsTop + settingsH + 10;

    int toolH = 28;
    int toolW = 80;
    int wideW = 140;

    MoveWindow(hOpenFloatingChatBtn, pad, toolsY, wideW, toolH, TRUE);

    MoveWindow(hClearLogBtn, W - pad - toolW * 2 - gap, toolsY, toolW, toolH, TRUE);
    MoveWindow(hCopyLogBtn, W - pad - toolW, toolsY, toolW, toolH, TRUE);

    // Log area
    int logY = toolsY + toolH + 10;
    int logH = H - logY - pad;
    if (logH < 140) logH = 140;
    MoveWindow(gLog, pad, logY, W - pad * 2, logH, TRUE);
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
    static YouTubeLiveChatService youtubeChat;
    static std::unique_ptr<HttpServer> gHttp;
    static std::thread metricsThread;
    static std::thread twitchHelixThread;
    static std::thread tiktokFollowersThread;
    static EuroScopeIngestService euroscope;
    static ObsWsClient obs;

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
        gTwitchViewerCount = 0;
        gTwitchFollowerCount = 0;
        gTwitchLive = false;
        gTwitchHelixStatus = L"Helix: restarting...";
        PostMessageW(hwnd, WM_APP + 41, 0, 0);

        gTwitchHelixRunning = true;
        gTwitchHelixBoundLogin = login;

        twitchHelixThread = StartTwitchHelixPoller(
            hwnd,
            config,
            state,
            gTwitchHelixRunning,
            (UINT)(WM_APP + 41),
            TwitchHelixUiCallbacks{
                /*log*/          [](const std::wstring& s) { LogLine(s); },
                /*set_status*/   [&](const std::wstring& s) {
                    gTwitchHelixStatus = s;
                    PostMessageW(hwnd, WM_APP + 41, 0, 0);
                },
                /*set_live*/     [&](bool live) { gTwitchLive = live; },
                /*set_viewers*/  [&](int v) { gTwitchViewerCount = v; },
                /*set_followers*/[&](int f) { gTwitchFollowerCount = f; }
            }
        );
    };

switch (msg) {

    case WM_CREATE:
    {
        if (!gBrushBg)    gBrushBg = CreateSolidBrush(gClrBg);
        if (!gBrushPanel) gBrushPanel = CreateSolidBrush(gClrPanel);
        if (!gBrushEdit)  gBrushEdit = CreateSolidBrush(gClrEditBg);

        if (!gFontUi) {
            LOGFONTW lf{};
            lf.lfHeight = -16;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            gFontUi = CreateFontIndirectW(&lf);
        }

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
            chat.Subscribe([pChat=&chat, pState=&state](const ChatMessage& m) {
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

            // Init WebView2 (controller + view). Navigation happens once the HTTP server is running.
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

                                        // Tidy defaults
                                        ICoreWebView2Settings* settings = nullptr;
                                        gMainWebView->get_Settings(&settings);
                                        if (settings) {
                                            settings->put_IsStatusBarEnabled(FALSE);
                                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                                            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                                            settings->Release();
                                        }

                                        // If the HTTP server is already ready, navigate now.
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

            // Kick off backend init (HTTP server, pollers, etc.)
            PostMessageW(hwnd, WM_APP + 1, 0, 0);
            return 0;
#endif

        // Group boxes
        hGroupTikTok = CreateWindowW(L"BUTTON", L"TikTok", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hGroupTwitch = CreateWindowW(L"BUTTON", L"Twitch", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hGroupYouTube = CreateWindowW(L"BUTTON", L"YouTube", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        // Labels + edits
        hLblTikTok = CreateWindowW(L"STATIC", L"Username (no @)", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hTikTok = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.tiktok_unique_id).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_TIKTOK_EDIT, nullptr, nullptr);
        hTikTokCookies = CreateWindowW(L"BUTTON", L"SET", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_TIKTOK_COOKIES, nullptr, nullptr);

        hLblTwitch = CreateWindowW(L"STATIC", L"Channel login", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hTwitch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.twitch_login).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_TWITCH_EDIT, nullptr, nullptr);
        hTwitchSettings = CreateWindowW(L"BUTTON", L"SET", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_TWITCH_SETTINGS, nullptr, nullptr);

        hLblYouTube = CreateWindowW(L"STATIC", L"Handle / Channel", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hYouTube = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.youtube_handle).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_YOUTUBE_EDIT, nullptr, nullptr);

        // Platform status labels
        gTikTokStatus = CreateWindowW(L"STATIC", L"Status: OFFLINE", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        gTikTokViewers = CreateWindowW(L"STATIC", L"Viewers: 0", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        gTikTokFollowers = CreateWindowW(L"STATIC", L"Followers: 0", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        gTwitchStatus = CreateWindowW(L"STATIC", L"Status: OFFLINE", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        gTwitchViewers = CreateWindowW(L"STATIC", L"Viewers: 0", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        gTwitchFollowers = CreateWindowW(L"STATIC", L"Followers: 0", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        gTwitchHelix = CreateWindowW(L"STATIC", gTwitchHelixStatus.c_str(), WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        gYouTubeStatus = CreateWindowW(L"STATIC", L"Status: OFFLINE", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        gYouTubeViewers = CreateWindowW(L"STATIC", L"Viewers: 0", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        gYouTubeFollowers = CreateWindowW(L"STATIC", L"Followers: 0", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        // Buttons
        hGroupSettings = CreateWindowW(L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hSave = CreateWindowW(L"BUTTON", L"Save settings", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_SAVE_BTN, nullptr, nullptr);

        hStartTikTokBtn = CreateWindowW(L"BUTTON", L"Start/Restart", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_START_TIKTOK, nullptr, nullptr);
        hRestartTikTokBtn = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_RESTART_TIKTOK, nullptr, nullptr);

        hStartTwitchBtn = CreateWindowW(L"BUTTON", L"Start/Restart", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_START_TWITCH, nullptr, nullptr);
        hRestartTwitchBtn = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_RESTART_TWITCH, nullptr, nullptr);

        hStartYouTubeBtn = CreateWindowW(L"BUTTON", L"Start/Restart", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_START_YOUTUBE, nullptr, nullptr);
        hRestartYouTubeBtn = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_RESTART_YOUTUBE, nullptr, nullptr);

        // 'Stop' buttons are placeholders for now (Stop functionality not implemented yet). Disable them.
        if (hRestartTikTokBtn)  EnableWindow(hRestartTikTokBtn, FALSE);
        if (hRestartTwitchBtn)  EnableWindow(hRestartTwitchBtn, FALSE);
        if (hRestartYouTubeBtn) EnableWindow(hRestartYouTubeBtn, FALSE);

        // Initial button enable/disable state based on current input values.
        UpdateTikTokButtons(hTikTok, hStartTikTokBtn, hRestartTikTokBtn);
        UpdateTwitchButtons(hTwitch, hStartTwitchBtn, hRestartTwitchBtn);
        UpdateYouTubeButtons(hYouTube, hStartYouTubeBtn, hRestartYouTubeBtn);

        hHint = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        // Log + tools
        gLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        LogLine(L"APP: UI log initialized");

        hClearLogBtn = CreateWindowW(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_CLEAR_LOG, nullptr, nullptr);
        hCopyLogBtn = CreateWindowW(L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_COPY_LOG, nullptr, nullptr);

        // Give open chat button an ID so WM_COMMAND can handle it cleanly
#ifndef IDC_OPEN_CHAT
#define IDC_OPEN_CHAT        41001
#endif

        HINSTANCE hInst = GetModuleHandleW(nullptr);

        hOpenFloatingChatBtn = CreateWindowW(
            L"BUTTON", L"Open Chat",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0,
            hwnd, (HMENU)(INT_PTR)IDC_OPEN_CHAT,
            hInst, nullptr
        );

        // Create floating chat manager lazily
        gFloatingChat = std::make_unique<FloatingChat>();

        // Apply font
        HWND controls[] = {
            hGroupTikTok,hGroupTwitch,hGroupYouTube,
            hLblTikTok,hLblTwitch,hLblYouTube,hHint,hGroupSettings,
            hTikTok,hTwitch,hYouTube,hTikTokCookies,
            hSave,hStartTikTokBtn,hRestartTikTokBtn,hStartTwitchBtn,hRestartTwitchBtn,hStartYouTubeBtn,hRestartYouTubeBtn,
            gTikTokStatus,gTikTokViewers,gTikTokFollowers,
            gTwitchStatus,gTwitchViewers,gTwitchFollowers,gTwitchHelix,
            gYouTubeStatus,gYouTubeViewers,gYouTubeFollowers,
            gLog,hClearLogBtn,hCopyLogBtn,hOpenFloatingChatBtn
        };
        for (HWND c : controls) if (c) SendMessageW(c, WM_SETFONT, (WPARAM)gFontUi, TRUE);

        {
            Metrics m{};
            if (TryFetchMetricsFromApi(m)) UpdatePlatformStatusUI(m);
            else UpdatePlatformStatusUI(state.get_metrics());
        }

        // Initial logs
        LogLine(L"Starting Mode-S Client overlay");
        LogLine(L"Overlay: http://localhost:17845/overlay/chat.html");
        LogLine(L"Metrics: http://localhost:17845/api/metrics");

        if (config.tiktok_unique_id.empty() && config.twitch_login.empty() && config.youtube_handle.empty()) {
            LogLine(L"No config.json found yet. Please enter channel details and click Save.");
        }
        else {
            LogLine(L"Loaded config.json");
        }

        SetWindowTextW(hHint, L"Tip: Save settings first. TikTok cookies are optional unless required by TikTok.");

        UpdateTikTokButtons(hTikTok, hStartTikTokBtn, hRestartTikTokBtn);
        LayoutControls(hwnd);

        PostMessageW(hwnd, WM_APP + 1, 0, 0);

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
                return PlatformControl::StartOrRestartTikTokSidecar(
                    tiktok, state, chat,
                    GetExeDir(),
                    config.tiktok_unique_id,
                    hwnd,
                    [](const std::wstring& s) { LogLine(s); });
            };
            opt.stop_tiktok = [&]() -> bool {
                PlatformControl::StopTikTok(tiktok, state, hwnd, (UINT)(WM_APP + 41), [](const std::wstring& s) { LogLine(s); });
                return true;
            };

            opt.start_twitch = [&]() -> bool {
                const std::string token = twitchAuth.GetAccessToken().value_or(token);
                const bool ok = PlatformControl::StartOrRestartTwitchIrc(
                    twitch, state, chat,
                    config.twitch_login,
                    token,
                    [](const std::wstring& s) { LogLine(s); });

                if (ok) {
                    // Start EventSub (follows/subs/gifts) using OAuth token in config.json
                    twitchEventSub.Start(
                        config.twitch_client_id,
                        token,
                        config.twitch_login,

                        // Do NOT use chat callback for EventSub
                        nullptr,

                        // EventSub events
                        [&](const nlohmann::json& ev)
                        {
                            state.add_twitch_eventsub_event(ev);
                        },

                        // Optional: status updates
                        [&](const nlohmann::json& st)
                        {
                            state.set_twitch_eventsub_status(st);
                        }
                    );
                    return ok;
                }
            };
            opt.stop_twitch = [&]() -> bool {
                PlatformControl::StopTwitch(twitch, state, hwnd, (UINT)(WM_APP + 41), [](const std::wstring& s) { LogLine(s); });
                return true;
            };

            opt.start_youtube = [&]() -> bool {
                const bool ok = PlatformControl::StartOrRestartYouTubeSidecar(
                    youtube, state, chat,
                    GetExeDir(),
                    config.youtube_handle,
                    hwnd,
                    [](const std::wstring& s) { LogLine(s); });

                youtubeChat.start(config.youtube_handle, chat,
                    [](const std::wstring& s) { LogLine(s); },
                    &state);

                return ok;
                };

            opt.stop_youtube = [&]() -> bool {
                PlatformControl::StopYouTube(youtube, state, hwnd, (UINT)(WM_APP + 41), [](const std::wstring& s) { LogLine(s); });
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
        metricsThread.detach();

        LogLine(L"TWITCH: starting Helix poller thread");

        // Bind the poller to the current config.twitch_login.
        RestartTwitchHelixPoller("init");
LogLine(L"TIKTOK: starting followers poller thread");
        tiktokFollowersThread = StartTikTokFollowersPoller(
            hwnd,
            config,
            state,
            gRunning,
            (UINT)(WM_APP + 41),
            TikTokFollowersUiCallbacks{
                /*log*/           [](const std::wstring& s) { LogLine(s); },
                /*set_status*/    [](const std::wstring& /*s*/) { /* optional */ },
                /*set_followers*/ [&](int f) { gTikTokFollowerCount = f; }
            }
        );
        tiktokFollowersThread.detach();
        // Twitch OAuth token refresh (silent) - runs during boot while splash is visible
        {
            LogLine(L"TWITCH: refreshing OAuth token...");
            std::string auth_err;
            // When TwitchAuth refreshes tokens, push the new access token into IRC + EventSub.
            twitchAuth.on_tokens_updated = [&](const std::string& access, const std::string& /*refresh*/, const std::string& login) {
                // EventSub uses Helix Bearer token (raw).
                twitchEventSub.UpdateAccessToken(access);
                twitchEventSub.Stop();
                twitchEventSub.Start(
                    config.twitch_client_id,
                    access,
                    config.twitch_login,
                    [&](const ChatMessage& msg) { chat.Add(msg); }
                );

                // IRC needs oauth: prefix; TwitchIrcWsClient::StartAuthenticated normalizes.
                PlatformControl::StartOrRestartTwitchIrc(
                    twitch, state, chat,
                    login,
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

    case WM_APP + 41: // refresh platform metrics UI
    {
        Metrics m{};
        if (TryFetchMetricsFromApi(m)) UpdatePlatformStatusUI(m);
        else UpdatePlatformStatusUI(state.get_metrics());
    }
    return 0;

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

case WM_COMMAND:
    {
        const int id = LOWORD(wParam);

        if (HIWORD(wParam) == EN_CHANGE && id == IDC_TIKTOK_EDIT) {
            UpdateTikTokButtons(hTikTok, hStartTikTokBtn, hRestartTikTokBtn);
            return 0;
        }


        if (HIWORD(wParam) == EN_CHANGE && id == IDC_TWITCH_EDIT) {
            UpdateTwitchButtons(hTwitch, hStartTwitchBtn, hRestartTwitchBtn);
            return 0;
        }

        if (HIWORD(wParam) == EN_CHANGE && id == IDC_YOUTUBE_EDIT) {
            UpdateYouTubeButtons(hYouTube, hStartYouTubeBtn, hRestartYouTubeBtn);
            return 0;
        }


        if (id == IDC_START_TIKTOK) {
            StartOrRestartTikTokSidecar(tiktok, state, chat, hwnd, gLog, hTikTok);
            return 0;
        }

        if (id == IDC_START_TWITCH) {
            // Ensure existing Twitch client is stopped before starting to allow restart or channel change
            twitch.stop();
            twitchEventSub.Stop();



            // Apply current UI channel login into config so Helix poller + web UI use the same channel.
            {
                std::string newLogin = SanitizeTwitchLogin(ToUtf8(GetWindowTextWString(hTwitch)));
                if (newLogin != config.twitch_login) {
                    config.twitch_login = newLogin;
                    RestartTwitchHelixPoller("channel change");
                }
            }
            // Start authenticated IRC and sink messages directly into ChatAggregator
            const std::string token = ReadTwitchUserAccessToken();

            bool ok = twitch.StartAuthenticated(
                SanitizeTwitchLogin(ToUtf8(GetWindowTextWString(hTwitch))),   // login / nick
                token,                                                       // raw token from config (will be normalized)
                SanitizeTwitchLogin(ToUtf8(GetWindowTextWString(hTwitch))),   // channel
                chat
            );

            if (ok) {
                LogLine(L"TWITCH: started/restarted IRC client.");
                // Start EventSub (follows/subs/gifts) using existing OAuth token in config.json
                twitchEventSub.Start(
                    config.twitch_client_id,
                    token,
                    config.twitch_login,
                    [&](const ChatMessage& msg)
                    {
                        chat.Add(msg);
                    });
            }
            else {
                LogLine(L"TWITCH: failed to start IRC client (already running or invalid parameters). Consider checking the channel name.");
            }
            return 0;
        }

        
        if (id == IDC_START_YOUTUBE) {
            std::string raw = ToUtf8(GetWindowTextWString(hYouTube));
            std::string cleaned = SanitizeYouTubeHandle(raw);
            SetWindowTextW(hYouTube, ToW(cleaned).c_str());

            PlatformControl::StartOrRestartYouTubeSidecar(
                youtube, state, chat,
                GetExeDir(),
                cleaned,
                hwnd,
                [](const std::wstring& s) { LogLine(s); });

            youtubeChat.start(cleaned, chat,
                [](const std::wstring& s) { LogLine(s); },
                &state);

            return 0;
        }

        if (id == IDC_TIKTOK_COOKIES) {
            TikTokCookiesDraft draft;
            draft.sessionid = ToW(config.tiktok_sessionid);
            draft.sessionid_ss = ToW(config.tiktok_sessionid_ss);
            draft.tt_target_idc = ToW(config.tiktok_tt_target_idc);

            if (EditTikTokCookiesModal(hwnd, draft) && draft.accepted) {
                config.tiktok_sessionid = ToUtf8(draft.sessionid);
                config.tiktok_sessionid_ss = ToUtf8(draft.sessionid_ss);
                config.tiktok_tt_target_idc = ToUtf8(draft.tt_target_idc);
                LogLine(L"TikTok cookies updated (not saved yet). Click Save settings to persist.");
            }
            return 0;
        }

        if (id == IDC_TWITCH_SETTINGS) {
            TwitchSettingsDraft draft;
            draft.login = ToW(config.twitch_login);
            draft.client_id = ToW(config.twitch_client_id);
            draft.client_secret = ToW(config.twitch_client_secret);

            if (EditTwitchSettingsModal(hwnd, draft) && draft.accepted) {
                config.twitch_login = SanitizeTwitchLogin(ToUtf8(draft.login));
                config.twitch_client_id = ToUtf8(draft.client_id);
                config.twitch_client_secret = ToUtf8(draft.client_secret);

                SetWindowTextW(hTwitch, ToW(config.twitch_login).c_str());
                LogLine(L"Twitch settings updated (not saved yet). Click Save settings to persist.");
            

                // If the channel login changed, rebound the Helix poller immediately.
                RestartTwitchHelixPoller("settings");
}
            return 0;
        }

        if (id == IDC_SAVE_BTN) {
            config.tiktok_unique_id = SanitizeTikTok(ToUtf8(GetWindowTextWString(hTikTok)));
            SetWindowTextW(hTikTok, ToW(config.tiktok_unique_id).c_str());

            config.twitch_login = SanitizeTwitchLogin(ToUtf8(GetWindowTextWString(hTwitch)));
            SetWindowTextW(hTwitch, ToW(config.twitch_login).c_str());

            

            // Rebind Helix poller if login changed and user clicked Save.
            RestartTwitchHelixPoller("save");
config.youtube_handle = ToUtf8(GetWindowTextWString(hYouTube));
            config.youtube_handle = Trim(config.youtube_handle);
            config.youtube_handle = SanitizeYouTubeHandle(config.youtube_handle);
            SetWindowTextW(hYouTube, ToW(config.youtube_handle).c_str());
            UpdateYouTubeButtons(hYouTube, hStartYouTubeBtn, hRestartYouTubeBtn);

            // Overlay Style UI removed (will be redesigned on a separate tab/page later)

            if (config.Save()) {
                LogLine(L"Saved config.json.");
            }
            else {
                LogLine(L"ERROR: Failed to save config.json");
            }

            UpdateTikTokButtons(hTikTok, hStartTikTokBtn, hRestartTikTokBtn);
            return 0;
        }

        if (id == IDC_CLEAR_LOG) {
            if (gLog) SetWindowTextW(gLog, L"");
            return 0;
        }

        if (id == IDC_COPY_LOG) {
            CopyLogToClipboard(hwnd);
            LogLine(L"Log copied to clipboard.");
            return 0;
        }

        {
            const int id = LOWORD(wParam);

            if (id == IDC_OPEN_CHAT) {
                if (gFloatingChat) gFloatingChat->Open(hwnd);
                return 0;
            }

            // ...existing handlers...
            break;
        }
    }

    case WM_SIZE:
#if HAVE_WEBVIEW2
        if (gUseModernUi && gMainWebController) {
            RECT rc; GetClientRect(hwnd, &rc);
            gMainWebController->put_Bounds(rc);
            return 0;
        }
#endif
        LayoutControls(hwnd);
        return 0;

    case WM_ERASEBKGND:
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, gBrushBg ? gBrushBg : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        HWND hCtl = (HWND)lParam;

        if (hCtl == gTikTokStatus || hCtl == gTwitchStatus || hCtl == gYouTubeStatus)
        {
            bool live = (hCtl == gTikTokStatus) ? gTikTokLive : (hCtl == gTwitchStatus) ? gTwitchLive : gYouTubeLive;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, live ? RGB(0, 200, 0) : RGB(220, 50, 50));
            return (LRESULT)(gBrushBg ? gBrushBg : GetStockObject(NULL_BRUSH));
        }

        if (hCtl == gLog)
        {
            SetTextColor(hdc, gClrText);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, gClrEditBg);
            return (LRESULT)(gBrushEdit ? gBrushEdit : GetStockObject(BLACK_BRUSH));
        }

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, (hCtl == hHint) ? gClrHint : gClrText);
        return (LRESULT)(gBrushBg ? gBrushBg : GetStockObject(BLACK_BRUSH));
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        HWND hCtl = (HWND)lParam;

        (void)hCtl;
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, RGB(255, 255, 255));
        return (INT_PTR)GetStockObject(WHITE_BRUSH);
    }

    case WM_DESTROY:
        gRunning = false;


        // Stop Helix poller thread cleanly.
        if (twitchHelixThread.joinable()) {
            gTwitchHelixRunning = false;
            twitchHelixThread.join();
        }
        // Stop platform services first (they may still be producing events)
        tiktok.stop();
        youtube.stop();
        twitch.stop();
        twitchEventSub.Stop();
        twitchAuth.Stop();
        youtubeChat.stop();

#if HAVE_WEBVIEW2
        // Tear down WebView2 last to avoid any late WM_SIZE/paint touching released objects.
        if (gMainWebController) {
            gMainWebController->put_IsVisible(FALSE);
            gMainWebController.Reset();
        }
        if (gMainWebView) {
            gMainWebView.Reset();
        }
#endif

        // Stop HTTP server cleanly (stop + join)
        if (gHttp) {
            gHttp->Stop();
            gHttp.reset();
        }

        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

	// NOTE: Some builds have ended up with an extra open block inside the WM_* handlers
	// (usually due to an edit inside a case label). The following brace ensures the
	// switch scope is fully closed before we return to DefWindowProc.
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
        CW_USEDEFAULT, CW_USEDEFAULT, 1440, 810,
        nullptr, nullptr, hInstance, nullptr
    );

    gMainWnd = hwnd;

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