#pragma once

#include <windows.h>
#include <string>

class SplashScreen
{
public:
    SplashScreen() = default;
    ~SplashScreen() = default;

    SplashScreen(const SplashScreen&) = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    HWND Create(HINSTANCE hInstance,
        const wchar_t* appDisplayName,
        const wchar_t* appVersionFile);

    void Destroy();
    void BeginCloseThenShowMain(HWND mainWnd, UINT delayMs);
    bool IsWindowValid() const;

    HWND GetHwnd() const noexcept { return m_hwnd; }
    HWND GetLogHwnd() const noexcept { return m_logHwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    HWND m_logHwnd = nullptr;

    const wchar_t* m_appDisplayName = L"";
    const wchar_t* m_appVersionFile = L"";

    static constexpr UINT_PTR kCloseTimerId = 1001;
};