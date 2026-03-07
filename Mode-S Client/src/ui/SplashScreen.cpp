#include "SplashScreen.h"

#include <windows.h>
#include <string>

namespace SplashScreen {
namespace {

HWND gSplashWnd = nullptr;
HWND gMainWnd = nullptr;
constexpr UINT_PTR kCloseTimerId = 1001;
constexpr UINT kCloseDelayMs = 1000;
const wchar_t* kSplashClassName = L"ModeSClientImageSplash";

LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_ERASEBKGND:
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(10, 15, 22));
        FillRect(reinterpret_cast<HDC>(wParam), &rc, bg);
        DeleteObject(bg);

        // Simple centered "image-like" panel so the file compiles cleanly even
        // before a bitmap-loading version is swapped in.
        RECT inner = rc;
        InflateRect(&inner, -24, -24);
        HBRUSH panel = CreateSolidBrush(RGB(18, 28, 40));
        FillRect(reinterpret_cast<HDC>(wParam), &inner, panel);
        DeleteObject(panel);

        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(225, 235, 245));
        std::wstring text = L"Starting StreamingATC.Live";
        DrawTextW(reinterpret_cast<HDC>(wParam), text.c_str(), -1, &inner,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return 1;
    }

    case WM_TIMER:
        if (wParam == kCloseTimerId) {
            KillTimer(hwnd, kCloseTimerId);
            Destroy();
            if (gMainWnd && IsWindow(gMainWnd)) {
                ShowWindow(gMainWnd, SW_SHOWDEFAULT);
                UpdateWindow(gMainWnd);
                SetForegroundWindow(gMainWnd);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        // Treat closing the splash as closing the app during startup.
        if (gMainWnd && IsWindow(gMainWnd)) {
            DestroyWindow(gMainWnd);
        }
        Destroy();
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        if (hwnd == gSplashWnd) {
            gSplashWnd = nullptr;
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterSplashClass(HINSTANCE hInstance)
{
    static bool registered = false;
    if (registered) return true;

    WNDCLASSW wc{};
    wc.lpfnWndProc = SplashWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kSplashClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.style = CS_HREDRAW | CS_VREDRAW;

    ATOM atom = RegisterClassW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    registered = true;
    return true;
}

} // namespace

bool Create(HINSTANCE hInstance, const wchar_t* displayName, const wchar_t* versionText)
{
    (void)displayName;
    (void)versionText;

    if (gSplashWnd && IsWindow(gSplashWnd)) {
        return true;
    }

    if (!RegisterSplashClass(hInstance)) {
        return false;
    }

    const int w = 720;
    const int h = 405;

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.left + ((wa.right - wa.left) - w) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    gSplashWnd = CreateWindowExW(
        WS_EX_TOPMOST,
        kSplashClassName,
        L"Starting…",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        nullptr, nullptr, hInstance, nullptr);

    if (!gSplashWnd) {
        return false;
    }

    ShowWindow(gSplashWnd, SW_SHOW);
    UpdateWindow(gSplashWnd);
    return true;
}

void OnAppReady(HWND mainWindow)
{
    gMainWnd = mainWindow;

    if (gSplashWnd && IsWindow(gSplashWnd)) {
        SetTimer(gSplashWnd, kCloseTimerId, kCloseDelayMs, nullptr);
        return;
    }

    if (gMainWnd && IsWindow(gMainWnd)) {
        ShowWindow(gMainWnd, SW_SHOWDEFAULT);
        UpdateWindow(gMainWnd);
        SetForegroundWindow(gMainWnd);
    }
}

void Destroy()
{
    if (gSplashWnd && IsWindow(gSplashWnd)) {
        HWND hwnd = gSplashWnd;
        gSplashWnd = nullptr;
        DestroyWindow(hwnd);
    }
}

} // namespace SplashScreen
