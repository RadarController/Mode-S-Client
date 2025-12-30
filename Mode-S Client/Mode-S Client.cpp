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

#include "AppConfig.h"
#include "AppState.h"
#include "TikTokSidecar.h"
#include "ObsWsClient.h"
#include "TwitchIrcWsClient.h"

#define IDC_TIKTOK_EDIT   1001
#define IDC_TWITCH_EDIT   1002
#define IDC_YOUTUBE_EDIT  1003
#define IDC_SAVE_BTN      1004
#define IDC_START_TIKTOK  1005
#define IDC_RESTART_TIKTOK 1006
#define IDC_START_TWITCH  1007
#define IDC_RESTART_TWITCH 1008

#define IDC_TIKTOK_COOKIES 1009
static std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

static HWND hStartTikTokBtn = nullptr, hRestartTikTokBtn = nullptr;


static HWND gLog = nullptr;
static std::atomic<bool> gRunning{ true };

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

// --- TikTok cookie modal (no resource file required) -------------------------

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
        // Only react to button clicks; EDIT controls send WM_COMMAND too (e.g. EN_SETFOCUS).
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

    // Simple modal loop
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

// ---------------------------------------------------------------------------

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
    // remove all '@'
    s.erase(std::remove(s.begin(), s.end(), '@'), s.end());
    return Trim(s);
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

static bool StartOrRestartTikTokSidecar(TikTokSidecar& tiktok,
    AppState& state,
    HWND hwndLog,
    HWND hTikTokEdit)
{
    // read + sanitize
    std::string raw = ToUtf8(GetWindowTextWString(hTikTokEdit));
    std::string cleaned = SanitizeTikTok(raw);

    // write cleaned value back into textbox (so user sees what will be used)
    SetWindowTextW(hTikTokEdit, ToW(cleaned).c_str());

    if (cleaned.empty()) {
        if (hwndLog) LogLine(L"TikTok username is empty. Enter it first.");
        return false;
    }

    // stop existing sidecar if running (safe to call always)
    tiktok.stop();

    // Make sure config.json gets updated too (optional — but nice)
    // (We’ll save on Save button; start/restart doesn’t force-save.)

    std::wstring exeDir = GetExeDir();
    std::wstring sidecarPath = exeDir + L"\\sidecar\\tiktok_sidecar.py";

    // Start sidecar. Python sidecar reads config.json, but we also want it to
    // work immediately even before Save. Easiest: we write config.json first.
    // If you prefer “must click Save”, remove this block.

    LogLine((L"Starting python sidecar: " + sidecarPath).c_str());

    _wputenv_s(
        L"WHITELIST_AUTHENTICATED_SESSION_ID_HOST",
        L"tiktok.eulerstream.com"
    );

    bool ok = tiktok.start(L"python", sidecarPath, [&](const nlohmann::json& j) {
        std::string type = j.value("type", "");
        //        if (type.rfind("tiktok.", 0) == 0) LogLine(ToW(("SIDE CAR EVENT: " + type)).c_str());
        //replaced with below
        if (type.rfind("tiktok.", 0) == 0) {
            std::string msg = j.value("message", "");
            std::string extra;
            if (!msg.empty()) extra = " | " + msg;
            LogLine(ToW(("SIDE CAR EVENT: " + type + extra)).c_str());
        }
        //end
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


static std::string SanitizeTwitchLogin(std::string s)
{
    // trim spaces
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();

    // remove leading '#'
    if (!s.empty() && s[0] == '#') s.erase(s.begin());

    // lowercase for consistency
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
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

    // stop existing
    twitch.stop();

    // Anonymous read-only login:
    // PASS SCHMOOPIIE + NICK justinfanXXXX (Twitch-supported anonymous user)
    // Channel to join is the streamer login.
    int suffix = 10000 + (GetTickCount() % 50000);
    std::string nick = "justinfan" + std::to_string(suffix);

    bool ok = twitch.start("SCHMOOPIIE", nick, channel,
        [&](const std::string& user, const std::string& message) {

            // Show on the main log window (like TikTok)
            LogLine(ToW("TWITCH CHAT: " + user + " | " + message));

            // Keep storing in AppState (if you use it later)
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


LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static AppConfig config;

    static HWND hTikTok = nullptr, hTwitch = nullptr, hYouTube = nullptr;
    static HWND hSave = nullptr;

    static AppState state;
    static TikTokSidecar tiktok;
    static TwitchIrcWsClient twitch;
    static std::thread serverThread;
    static std::thread metricsThread;
    static ObsWsClient obs;

    switch (msg) {

    case WM_CREATE:
    {
        // Log box
        gLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            10, 75, 760, 355, hwnd, nullptr, nullptr, nullptr);

        LogLine(L"Starting StreamHub (polling overlay)...");
        LogLine(L"Overlay: http://localhost:17845/overlay/chat.html");
        LogLine(L"Metrics: http://localhost:17845/api/metrics");

        // Load saved config (if any)
        if (config.Load()) {
            LogLine(L"Loaded config.json");
        }
        else {
            LogLine(L"No config.json found yet. Please enter channel details and click Save.");
        }

        // Labels + edit boxes
        CreateWindowW(L"STATIC", L"TikTok (no @):", WS_CHILD | WS_VISIBLE,
            10, 10, 140, 20, hwnd, nullptr, nullptr, nullptr);

        hTikTok = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.tiktok_unique_id).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            150, 8, 200, 24, hwnd, (HMENU)IDC_TIKTOK_EDIT, nullptr, nullptr);

        // TikTok cookies (session) button
        CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE,
            352, 8, 24, 24, hwnd, (HMENU)IDC_TIKTOK_COOKIES, nullptr, nullptr);

        // "..." button to edit TikTok cookie fields
        CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE,
            352, 8, 16, 24, hwnd, (HMENU)IDC_TIKTOK_COOKIES, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Twitch login:", WS_CHILD | WS_VISIBLE,
            370, 10, 90, 20, hwnd, nullptr, nullptr, nullptr);

        hTwitch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.twitch_login).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            460, 8, 180, 24, hwnd, (HMENU)IDC_TWITCH_EDIT, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"YouTube handle:", WS_CHILD | WS_VISIBLE,
            10, 42, 140, 20, hwnd, nullptr, nullptr, nullptr);

        hYouTube = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ToW(config.youtube_handle).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            150, 40, 200, 24, hwnd, (HMENU)IDC_YOUTUBE_EDIT, nullptr, nullptr);

        // Buttons
        hSave = CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE,
            370, 40, 80, 24, hwnd, (HMENU)IDC_SAVE_BTN, nullptr, nullptr);

        hStartTikTokBtn = CreateWindowW(L"BUTTON", L"Start TikTok", WS_CHILD | WS_VISIBLE,
            460, 40, 110, 24, hwnd, (HMENU)IDC_START_TIKTOK, nullptr, nullptr);

        hRestartTikTokBtn = CreateWindowW(L"BUTTON", L"Restart TikTok", WS_CHILD | WS_VISIBLE,
            580, 40, 120, 24, hwnd, (HMENU)IDC_RESTART_TIKTOK, nullptr, nullptr);

        // Twitch buttons
        CreateWindowW(L"BUTTON", L"Start Twitch", WS_CHILD | WS_VISIBLE,
            650, 8, 110, 24, hwnd, (HMENU)IDC_START_TWITCH, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"Restart Twitch", WS_CHILD | WS_VISIBLE,
            650, 40, 110, 24, hwnd, (HMENU)IDC_RESTART_TWITCH, nullptr, nullptr);

        // Disable Start/Restart until TikTok field has something valid
        UpdateTikTokButtons(hTikTok, hStartTikTokBtn, hRestartTikTokBtn);

        LogLine(L"TikTok is not started yet. Enter TikTok username then click Start or Restart.");

        // Start HTTP server in background thread (only once)
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
                }
                else {
                    res.set_content(html, "text/html; charset=utf-8");
                }
                });

            svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
                res.set_redirect("/overlay/chat.html");
                });

            svr.listen("127.0.0.1", 17845);
            });

        // Metrics / OBS update loop (OBS is stub right now)
        metricsThread = std::thread([&]() {
            while (gRunning) {
                auto m = state.get_metrics();

                obs.set_text("TOTAL_VIEWER_COUNT", std::to_string(m.total_viewers()));
                obs.set_text("TOTAL_FOLLOWER_COUNT", std::to_string(m.total_followers()));

                Sleep(5000);
            }
            });

        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);

        // Enable/disable Start/Restart as user types TikTok username
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

        if (id == IDC_TIKTOK_COOKIES) {
            // Open modal to edit TikTok session cookies. Changes apply in-memory;
            // user must click Save to persist them to config.json.
            TikTokCookiesDraft draft;
            draft.sessionid = ToW(config.tiktok_sessionid);
            draft.sessionid_ss = ToW(config.tiktok_sessionid_ss);
            draft.tt_target_idc = ToW(config.tiktok_tt_target_idc);

            if (EditTikTokCookiesModal(hwnd, draft) && draft.accepted) {
                config.tiktok_sessionid = ToUtf8(draft.sessionid);
                config.tiktok_sessionid_ss = ToUtf8(draft.sessionid_ss);
                config.tiktok_tt_target_idc = ToUtf8(draft.tt_target_idc);
                LogLine(L"TikTok cookies updated (not saved yet). Click Save to persist.");
            }
            return 0;
        }

        if (id == IDC_SAVE_BTN) {
            // Save + sanitize TikTok (strip @, trim)
            config.tiktok_unique_id = SanitizeTikTok(ToUtf8(GetWindowTextWString(hTikTok)));
            SetWindowTextW(hTikTok, ToW(config.tiktok_unique_id).c_str());

            config.twitch_login = SanitizeTwitchLogin(ToUtf8(GetWindowTextWString(hTwitch)));
            SetWindowTextW(hTwitch, ToW(config.twitch_login).c_str());
            config.youtube_handle = ToUtf8(GetWindowTextWString(hYouTube));

            if (config.Save()) {
                LogLine(L"Saved config.json.");
            }
            else {
                LogLine(L"ERROR: Failed to save config.json");
            }

            // Refresh button enabled/disabled state after sanitizing
            UpdateTikTokButtons(hTikTok, hStartTikTokBtn, hRestartTikTokBtn);
            return 0;
        }

        break;
    }

    case WM_SIZE:
    {
        RECT rc; GetClientRect(hwnd, &rc);
        if (gLog) MoveWindow(gLog, 10, 75, rc.right - 20, rc.bottom - 85, TRUE);
        return 0;
    }

    case WM_DESTROY:
        gRunning = false;
        tiktok.stop();
        PostQuitMessage(0);
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

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Mode-S Client (StreamHub)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 500,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return 0;
    ShowWindow(hwnd, nCmdShow);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}