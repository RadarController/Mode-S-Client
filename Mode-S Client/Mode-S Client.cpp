#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <thread>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <ctime>

#include "httplib.h"
#include "json.hpp"

#include "resource.h"
#include "AppConfig.h"
#include "AppState.h"
#include "TikTokSidecar.h"
#include "ObsWsClient.h"
#include "TwitchIrcWsClient.h"

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
#define IDC_START_YOUTUBE      1010
#define IDC_RESTART_YOUTUBE    1011
#define IDC_CLEAR_LOG          1012
#define IDC_COPY_LOG           1013
#define IDC_OVERLAY_FONT       1020
#define IDC_OVERLAY_SIZE       1021
#define IDC_OVERLAY_SHADOW     1022

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
static HWND hGroupTikTok = nullptr, hGroupTwitch = nullptr, hGroupYouTube = nullptr;
static HWND hLblTikTok = nullptr, hLblTwitch = nullptr, hLblYouTube = nullptr;
static HWND hHint = nullptr;
static HWND hGroupSettings = nullptr;

static HWND hTikTok = nullptr, hTwitch = nullptr, hYouTube = nullptr;
static HWND hTikTokCookies = nullptr;

static HWND hSave = nullptr;
static HWND hStartTikTokBtn = nullptr, hRestartTikTokBtn = nullptr;
static HWND hStartTwitchBtn = nullptr, hRestartTwitchBtn = nullptr;
static HWND hStartYouTubeBtn = nullptr, hRestartYouTubeBtn = nullptr;

static HWND hClearLogBtn = nullptr, hCopyLogBtn = nullptr;

static std::atomic<bool> gRunning{ true };

// Chat overlay UI controls
static HWND hGroupOverlay = nullptr;
static HWND hLblOverlayFont = nullptr;
static HWND hOverlayFont = nullptr;
static HWND hLblOverlaySize = nullptr;
static HWND hOverlaySize = nullptr;
static HWND hOverlayShadow = nullptr;

// Forward decl
static void LayoutControls(HWND hwnd);

// --------------------------- Helpers ----------------------------------------
static std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

static void LogLine(const std::wstring& s) {
    if (!gLog) return;
    int len = GetWindowTextLengthW(gLog);
    SendMessageW(gLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(gLog, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
    SendMessageW(gLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring GetWindowTextWString(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring w(len, L'\0');
    GetWindowTextW(h, w.data(), len + 1);
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
    if (hRestartBtn) EnableWindow(hRestartBtn, enable);
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
    // Good enough for common Google font family names like "Inter", "Roboto Slab", etc.
    // Spaces become '+'. (If you later support weight/ital etc you can expand this.)
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

// --------------------------- Platform start helpers -------------------------
static bool StartOrRestartTikTokSidecar(TikTokSidecar& tiktok,
    AppState& state,
    HWND hwndLog,
    HWND hTikTokEdit)
{
    std::string raw = ToUtf8(GetWindowTextWString(hTikTokEdit));
    std::string cleaned = SanitizeTikTok(raw);
    SetWindowTextW(hTikTokEdit, ToW(cleaned).c_str());

    if (cleaned.empty()) {
        if (hwndLog) LogLine(L"TikTok username is empty. Enter it first.");
        return false;
    }

    tiktok.stop();

    std::wstring exeDir = GetExeDir();
    std::wstring sidecarPath = exeDir + L"\\sidecar\\tiktok_sidecar.py";

    LogLine((L"Starting python sidecar: " + sidecarPath));

    _wputenv_s(L"WHITELIST_AUTHENTICATED_SESSION_ID_HOST", L"tiktok.eulerstream.com");

    bool ok = tiktok.start(L"python", sidecarPath, [&](const nlohmann::json& j) {
        std::string type = j.value("type", "");
        if (type.rfind("tiktok.", 0) == 0) {
            std::string msg = j.value("message", "");
            std::string extra;
            if (!msg.empty()) extra = " | " + msg;
            LogLine(ToW(("TIKTOK: " + type + extra)));
        }
        if (type == "tiktok.chat") {
            ChatMessage c;
            c.platform = "TikTok";
            c.user = j.value("user", "unknown");
            c.message = j.value("message", "");
            double ts = j.value("ts", 0.0);
            c.ts_ms = (std::int64_t)(ts * 1000.0);
            state.add_chat(std::move(c));
        }
        else if (type == "tiktok.viewers") {
            state.set_tiktok_viewers(j.value("viewers", 0));
        }
        });

    if (ok) {
        if (hwndLog) LogLine(L"TikTok sidecar started/restarted.");
    }
    else {
        if (hwndLog) LogLine(L"ERROR: Could not start TikTok sidecar. Check Python + TikTokLive install.");
    }
    return ok;
}

static bool StartOrRestartTwitchChat(TwitchIrcWsClient& twitch,
    AppState& state,
    HWND hwndLog,
    HWND hTwitchEdit)
{
    std::string raw = ToUtf8(GetWindowTextWString(hTwitchEdit));
    std::string channel = SanitizeTwitchLogin(raw);
    SetWindowTextW(hTwitchEdit, ToW(channel).c_str());

    if (channel.empty()) {
        if (hwndLog) LogLine(L"Twitch login is empty. Enter it first.");
        return false;
    }

    twitch.stop();

    int suffix = 10000 + (GetTickCount() % 50000);
    std::string nick = "justinfan" + std::to_string(suffix);

    bool ok = twitch.start("SCHMOOPIIE", nick, channel,
        [&](const std::string& user, const std::string& message) {
            LogLine(ToW("TWITCH: " + user + " | " + message));
            ChatMessage c;
            c.platform = "Twitch";
            c.user = user;
            c.message = message;
            c.ts_ms = (std::int64_t)(time(nullptr) * 1000);
            state.add_chat(std::move(c));
        });

    if (ok) {
        if (hwndLog) LogLine(L"Twitch chat started/restarted.");
    }
    else {
        if (hwndLog) LogLine(L"ERROR: Could not start Twitch chat.");
    }

    return ok;
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
    }

    // Twitch column
    {
        int x = x0 + (colW + gap) + innerPad;
        int y = y0 + 28;
        int w = colW - innerPad * 2;

        MoveWindow(hLblTwitch, x, y, w, labelH, TRUE);
        y += labelH + 6;

        MoveWindow(hTwitch, x, y, w, editH, TRUE);
        y += editH + rowGap;

        int bw = (w - gap) / 2;
        MoveWindow(hStartTwitchBtn, x, y, bw, btnH, TRUE);
        MoveWindow(hRestartTwitchBtn, x + bw + gap, y, bw, btnH, TRUE);
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
    }

    // Hint line
    int hintY = y0 + topH + 6;
    MoveWindow(hHint, pad, hintY, W - pad * 2, 18, TRUE);

    // Overlay Style panel - under all three channels
    int overlayTop = hintY + 22;
    int overlayH = 102;

    MoveWindow(hGroupOverlay, pad, overlayTop, W - pad * 2, overlayH, TRUE);

    int ox = pad + 12;
    int oy = overlayTop + 22;

    // Font label + edit
    MoveWindow(hLblOverlayFont, ox, oy, W - pad * 2 - 24, 18, TRUE);
    oy += 20;
    MoveWindow(hOverlayFont, ox, oy, (W - pad * 2) - 24 - 140, 24, TRUE);

    // Size label + edit on right
    int rightW = 120;
    int rightX = pad + (W - pad * 2) - 12 - rightW;
    MoveWindow(hLblOverlaySize, rightX, oy - 20, rightW, 18, TRUE);
    MoveWindow(hOverlaySize, rightX, oy, rightW, 24, TRUE);

    oy += 30;
    MoveWindow(hOverlayShadow, ox, oy, 220, 20, TRUE);

    // Settings section (Save button) - below Overlay Style
    int settingsTop = overlayTop + overlayH + 10;
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
    static TikTokSidecar tiktok;
    static TwitchIrcWsClient twitch;
    static std::thread serverThread;
    static std::thread metricsThread;
    static ObsWsClient obs;

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
        if (config.Load()) {
            // ok
        }

        // Group boxes
        hGroupTikTok = CreateWindowW(L"BUTTON", L"TikTok", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hGroupTwitch = CreateWindowW(L"BUTTON", L"Twitch", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hGroupYouTube = CreateWindowW(L"BUTTON", L"YouTube", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        // Labels + edits
        hLblTikTok = CreateWindowW(L"STATIC", L"Username (no @)", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hTikTok = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.tiktok_unique_id).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_TIKTOK_EDIT, nullptr, nullptr);
        hTikTokCookies = CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_TIKTOK_COOKIES, nullptr, nullptr);

        hLblTwitch = CreateWindowW(L"STATIC", L"Channel login", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hTwitch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.twitch_login).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_TWITCH_EDIT, nullptr, nullptr);

        hLblYouTube = CreateWindowW(L"STATIC", L"Handle / Channel", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        hYouTube = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.youtube_handle).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_YOUTUBE_EDIT, nullptr, nullptr);

        // Buttons
        hGroupSettings = CreateWindowW(L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        hSave = CreateWindowW(L"BUTTON", L"Save settings", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_SAVE_BTN, nullptr, nullptr);

        hStartTikTokBtn = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_START_TIKTOK, nullptr, nullptr);
        hRestartTikTokBtn = CreateWindowW(L"BUTTON", L"Restart", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_RESTART_TIKTOK, nullptr, nullptr);

        hStartTwitchBtn = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_START_TWITCH, nullptr, nullptr);
        hRestartTwitchBtn = CreateWindowW(L"BUTTON", L"Restart", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_RESTART_TWITCH, nullptr, nullptr);

        hStartYouTubeBtn = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_START_YOUTUBE, nullptr, nullptr);
        hRestartYouTubeBtn = CreateWindowW(L"BUTTON", L"Restart", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_RESTART_YOUTUBE, nullptr, nullptr);
        EnableWindow(hStartYouTubeBtn, FALSE);
        EnableWindow(hRestartYouTubeBtn, FALSE);

        hHint = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        // Overlay Style group
        hGroupOverlay = CreateWindowW(L"BUTTON", L"Overlay Style",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        hLblOverlayFont = CreateWindowW(L"STATIC", L"Google Font family (e.g. Inter, Roboto, Montserrat)",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        hOverlayFont = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            ToW(config.overlay_font_family).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_OVERLAY_FONT, nullptr, nullptr);

        hLblOverlaySize = CreateWindowW(L"STATIC", L"Text size (px)",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        {
            std::wstring sz = std::to_wstring(config.overlay_font_size);
            hOverlaySize = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", sz.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)IDC_OVERLAY_SIZE, nullptr, nullptr);
        }

        hOverlayShadow = CreateWindowW(L"BUTTON", L"Enable text shadow",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)IDC_OVERLAY_SHADOW, nullptr, nullptr);

        SendMessageW(hOverlayShadow, BM_SETCHECK, config.overlay_text_shadow ? BST_CHECKED : BST_UNCHECKED, 0);

        // Log + tools
        gLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);

        hClearLogBtn = CreateWindowW(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_CLEAR_LOG, nullptr, nullptr);
        hCopyLogBtn = CreateWindowW(L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_COPY_LOG, nullptr, nullptr);

        // Apply font
        HWND controls[] = {
            hGroupTikTok,hGroupTwitch,hGroupYouTube,
            hLblTikTok,hLblTwitch,hLblYouTube,hHint,hGroupSettings,
            hTikTok,hTwitch,hYouTube,hTikTokCookies,
            hSave,hStartTikTokBtn,hRestartTikTokBtn,hStartTwitchBtn,hRestartTwitchBtn,hStartYouTubeBtn,hRestartYouTubeBtn,
            gLog,hClearLogBtn,hCopyLogBtn,
            hGroupOverlay,hLblOverlayFont,hOverlayFont,hLblOverlaySize,hOverlaySize,hOverlayShadow
        };
        for (HWND c : controls) if (c) SendMessageW(c, WM_SETFONT, (WPARAM)gFontUi, TRUE);

        // Initial logs
        LogLine(L"Starting Mode-S Client overlay...");
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

        // Start HTTP server in background thread
        serverThread = std::thread([&]() {
            httplib::Server svr;

            svr.Get("/api/metrics", [&](const httplib::Request&, httplib::Response& res) {
                auto j = state.metrics_json();
                res.set_content(j.dump(2), "application/json; charset=utf-8");
                });

            svr.Get("/api/chat", [&](const httplib::Request&, httplib::Response& res) {
                auto j = state.chat_json();
                res.set_content(j.dump(2), "application/json; charset=utf-8");
                });

            svr.Get("/overlay/chat.html", [&](const httplib::Request&, httplib::Response& res) {
                std::wstring htmlPath = GetExeDir() + L"\\assets\\overlay\\chat.html";
                auto html = ReadFileUtf8(htmlPath);
                if (html.empty()) {
                    res.status = 404;
                    res.set_content("chat.html not found (check Copy to Output Directory)", "text/plain");
                    return;
                }

                // Build injected values from config
                std::string fontFamily = Trim(config.overlay_font_family);
                std::string fontStack = "sans-serif";
                std::string googleLink = "";

                if (!fontFamily.empty()) {
                    // Use the chosen font, but still fall back to sans-serif
                    fontStack = "'" + fontFamily + "', sans-serif";

                    // Google fonts CSS URL
                    std::string enc = UrlEncodeGoogleFontFamily(fontFamily);
                    std::string url = "https://fonts.googleapis.com/css2?family=" + enc + "&display=swap";
                    googleLink = "<link rel=\"stylesheet\" href=\"" + url + "\">";
                }

                std::string shadow = config.overlay_text_shadow
                    ? "0 0 4px rgba(0,0,0,.8)"
                    : "none";

                ReplaceAll(html, "%%GOOGLE_FONT_LINK%%", googleLink);
                ReplaceAll(html, "%%FONT_STACK%%", fontStack);
                ReplaceAll(html, "%%FONT_SIZE%%", std::to_string(config.overlay_font_size));
                ReplaceAll(html, "%%TEXT_SHADOW%%", shadow);

                res.set_content(html, "text/html; charset=utf-8");
                });

            svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
                res.set_redirect("/overlay/chat.html");
                });

            svr.listen("127.0.0.1", 17845);
            });
        serverThread.detach();

        metricsThread = std::thread([&]() {
            while (gRunning) {
                auto m = state.get_metrics();
                obs.set_text("TOTAL_VIEWER_COUNT", std::to_string(m.total_viewers()));
                obs.set_text("TOTAL_FOLLOWER_COUNT", std::to_string(m.total_followers()));
                Sleep(5000);
            }
            });
        metricsThread.detach();

        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);

        if (HIWORD(wParam) == EN_CHANGE && id == IDC_TIKTOK_EDIT) {
            UpdateTikTokButtons(hTikTok, hStartTikTokBtn, hRestartTikTokBtn);
            return 0;
        }

        if (id == IDC_START_TIKTOK || id == IDC_RESTART_TIKTOK) {
            StartOrRestartTikTokSidecar(tiktok, state, gLog, hTikTok);
            return 0;
        }

        if (id == IDC_START_TWITCH || id == IDC_RESTART_TWITCH) {
            StartOrRestartTwitchChat(twitch, state, gLog, hTwitch);
            return 0;
        }

        if (id == IDC_START_YOUTUBE || id == IDC_RESTART_YOUTUBE) {
            LogLine(L"YouTube support is UI-ready but not implemented yet.");
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

        if (id == IDC_SAVE_BTN) {
            config.tiktok_unique_id = SanitizeTikTok(ToUtf8(GetWindowTextWString(hTikTok)));
            SetWindowTextW(hTikTok, ToW(config.tiktok_unique_id).c_str());

            config.twitch_login = SanitizeTwitchLogin(ToUtf8(GetWindowTextWString(hTwitch)));
            SetWindowTextW(hTwitch, ToW(config.twitch_login).c_str());

            config.youtube_handle = ToUtf8(GetWindowTextWString(hYouTube));

            // Overlay style
            config.overlay_font_family = ToUtf8(GetWindowTextWString(hOverlayFont));
            config.overlay_font_family = Trim(config.overlay_font_family);

            int sizePx = ParseIntOrDefault(GetWindowTextWString(hOverlaySize), config.overlay_font_size);
            sizePx = ClampInt(sizePx, 8, 72);
            config.overlay_font_size = sizePx;

            config.overlay_text_shadow = (SendMessageW(hOverlayShadow, BM_GETCHECK, 0, 0) == BST_CHECKED);

            // reflect clamped size back into the box
            SetWindowTextW(hOverlaySize, std::to_wstring(config.overlay_font_size).c_str());

            // Persist everything
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

        break;
    }

    case WM_SIZE:
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

        // NOTE: Readonly EDIT controls are reported as STATIC for color messages.
        // gLog is ES_READONLY, so we must paint it OPAQUE to avoid text ghosting/overlap on scroll.
        if (hCtl == gLog)
        {
            SetTextColor(hdc, gClrText);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, gClrEditBg);
            return (LRESULT)(gBrushEdit ? gBrushEdit : GetStockObject(BLACK_BRUSH));
        }

        // Normal labels
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, (hCtl == hHint) ? gClrHint : gClrText);
        return (LRESULT)(gBrushBg ? gBrushBg : GetStockObject(BLACK_BRUSH));
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        HWND hCtl = (HWND)lParam;

        // Keep input boxes "normal" (white) so they're readable.
        // (gLog is ES_READONLY so it is handled in WM_CTLCOLORSTATIC above.)
        (void)hCtl;
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, RGB(255, 255, 255));
        return (INT_PTR)GetStockObject(WHITE_BRUSH);
    }

    case WM_DESTROY:
        gRunning = false;
        tiktok.stop();
        twitch.stop();        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const wchar_t CLASS_NAME[] = L"StreamHubWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"RadarController Mode-S Client",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1440, 810,
        nullptr, nullptr, hInstance, nullptr
    );

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)));

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}