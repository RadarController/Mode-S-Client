#pragma once

#include <string>
#include <windows.h>

class AppState;

// Logging for:
// - legacy Win32 UI log edit control
// - splash log edit control
// - Web UI log buffer via AppState (/api/log)
//
// This module centralises log marshalling and avoids Mode-S Client.cpp owning logging internals.

void UiLog_SetUiContext(HWND mainWnd, DWORD uiThreadId, UINT wmAppLog);
void UiLog_SetLogHwnd(HWND logEdit);
void UiLog_SetSplashHwnd(HWND splashLogEdit);
void UiLog_SetWebLogState(AppState* state);

// Called from anywhere (UI thread or worker thread).
void LogLine(const std::wstring& s);

// Called only on the UI thread.
void UiLog_AppendOnUiThread(const std::wstring& s);

// WM_APP_LOG handler helper (owns deleting the heap string).
void UiLog_HandleAppLogMessage(LPARAM lParam);

// Update