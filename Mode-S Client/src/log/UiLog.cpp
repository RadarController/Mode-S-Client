#include "log/UiLog.h"

#include <mutex>
#include <string>

#include "AppState.h"

static std::mutex gLogMutex;

static HWND  gLogEdit = nullptr;
static HWND  gSplashLogEdit = nullptr;
static HWND  gMainWndForLog = nullptr;
static DWORD gUiThreadIdForLog = 0;
static UINT  gWmAppLog = 0;

static AppState* gStateForWebLog = nullptr;

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

void UiLog_SetUiContext(HWND mainWnd, DWORD uiThreadId, UINT wmAppLog)
{
    gMainWndForLog = mainWnd;
    gUiThreadIdForLog = uiThreadId;
    gWmAppLog = wmAppLog;
}

void UiLog_SetLogHwnd(HWND logEdit)
{
    gLogEdit = logEdit;
}

void UiLog_SetSplashHwnd(HWND splashLogEdit)
{
    gSplashLogEdit = splashLogEdit;
}

void UiLog_SetWebLogState(AppState* state)
{
    gStateForWebLog = state;
}

void UiLog_AppendOnUiThread(const std::wstring& s)
{
    if (!gLogEdit && !gSplashLogEdit) return;

    // Make each log call atomic across threads and avoid double newlines.
    std::lock_guard<std::mutex> _lk(gLogMutex);

    std::wstring clean = s;

    // Trim trailing CR/LF
    while (!clean.empty() && (clean.back() == L'\r' || clean.back() == L'\n'))
        clean.pop_back();

    if (gLogEdit) {
        int len = GetWindowTextLengthW(gLogEdit);
        SendMessageW(gLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(gLogEdit, EM_REPLACESEL, FALSE, (LPARAM)clean.c_str());
        SendMessageW(gLogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }

    if (gSplashLogEdit) {
        int slen = GetWindowTextLengthW(gSplashLogEdit);
        SendMessageW(gSplashLogEdit, EM_SETSEL, (WPARAM)slen, (LPARAM)slen);
        SendMessageW(gSplashLogEdit, EM_REPLACESEL, FALSE, (LPARAM)clean.c_str());
        SendMessageW(gSplashLogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }
}

void UiLog_HandleAppLogMessage(LPARAM lParam)
{
    auto* heap = reinterpret_cast<std::wstring*>(lParam);
    if (heap) {
        UiLog_AppendOnUiThread(*heap);
        delete heap;
    }
}

void LogLine(const std::wstring& s)
{
    // Also feed the Web UI log buffer (served via /api/log)
    if (gStateForWebLog) {
        gStateForWebLog->push_log_utf8(ToUtf8(s));
    }

    // If called from a worker thread, marshal to the UI thread to avoid deadlocks/freezes.
    if (gUiThreadIdForLog != 0 && GetCurrentThreadId() != gUiThreadIdForLog) {
        if (gMainWndForLog && gWmAppLog != 0) {
            auto* heap = new std::wstring(s);
            if (!PostMessageW(gMainWndForLog, gWmAppLog, 0, (LPARAM)heap)) {
                delete heap; // fallback if posting fails
            }
        }
        return;
    }

    UiLog_AppendOnUiThread(s);
}

// Update