#include "ui/SplashScreen.h"

#include <atomic>
#include <string>

#include "log/UiLog.h"

HWND SplashScreen::Create(HINSTANCE hInstance,
    const wchar_t* appDisplayName,
    const wchar_t* appVersionFile)
{
    m_appDisplayName = appDisplayName ? appDisplayName : L"";
    m_appVersionFile = appVersionFile ? appVersionFile : L"";

    const wchar_t* kSplashClass = L"ModeSClientSplash";

    static std::atomic<bool> registered{ false };
    if (!registered.exchange(true))
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = SplashScreen::WndProc;
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
        m_appDisplayName,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        nullptr, nullptr, hInstance, this);

    if (hwnd) {
        std::wstring title = std::wstring(m_appDisplayName) + L" " + m_appVersionFile;
        SetWindowTextW(hwnd, title.c_str());
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    m_hwnd = hwnd;
    return hwnd;
}

void SplashScreen::Destroy()
{
    if (m_hwnd && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
    m_logHwnd = nullptr;
}

void SplashScreen::BeginCloseThenShowMain(HWND mainWnd, UINT delayMs)
{
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        if (mainWnd && IsWindow(mainWnd)) {
            ShowWindow(mainWnd, SW_SHOWDEFAULT);
            UpdateWindow(mainWnd);
            SetForegroundWindow(mainWnd);
        }
        return;
    }

    SetPropW(m_hwnd, L"splash_main_wnd", mainWnd);
    SetTimer(m_hwnd, kCloseTimerId, delayMs, nullptr);
}

bool SplashScreen::IsWindowValid() const
{
    return m_hwnd && IsWindow(m_hwnd);
}

LRESULT CALLBACK SplashScreen::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SplashScreen* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SplashScreen*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else {
        self = reinterpret_cast<SplashScreen*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SplashScreen::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
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

        SetPropW(hwnd, L"splash_font_title", hFontTitle);
        SetPropW(hwnd, L"splash_font_body", hFontBody);

        const int pad = 14;

        HWND hTitle = CreateWindowW(L"STATIC", m_appDisplayName,
            WS_CHILD | WS_VISIBLE,
            pad, pad, 520, 28, hwnd, nullptr, nullptr, nullptr);

        std::wstring ver = std::wstring(L"Loading… ") + m_appVersionFile;
        HWND hVer = CreateWindowW(L"STATIC", ver.c_str(),
            WS_CHILD | WS_VISIBLE,
            pad, pad + 30, 520, 20, hwnd, nullptr, nullptr, nullptr);

        m_logHwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            pad, pad + 58, 520, 220, hwnd, nullptr, nullptr, nullptr);

        UiLog_SetSplashHwnd(m_logHwnd);

        if (hTitle) SendMessageW(hTitle, WM_SETFONT, (WPARAM)hFontTitle, TRUE);
        if (hVer) SendMessageW(hVer, WM_SETFONT, (WPARAM)hFontBody, TRUE);
        if (m_logHwnd) SendMessageW(m_logHwnd, WM_SETFONT, (WPARAM)hFontBody, TRUE);

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
        if (wParam == kCloseTimerId) {
            KillTimer(hwnd, kCloseTimerId);

            HWND mainWnd = (HWND)GetPropW(hwnd, L"splash_main_wnd");

            Destroy();

            if (mainWnd && IsWindow(mainWnd)) {
                ShowWindow(mainWnd, SW_SHOWDEFAULT);
                UpdateWindow(mainWnd);
                SetForegroundWindow(mainWnd);
            }
            return 0;
        }
        break;
    }

    case WM_CLOSE:
    {
        HWND mainWnd = (HWND)GetPropW(hwnd, L"splash_main_wnd");
        if (mainWnd && IsWindow(mainWnd)) {
            DestroyWindow(mainWnd);
        }

        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;
    }

    case WM_DESTROY:
    {
        if (auto h = (HFONT)GetPropW(hwnd, L"splash_font_title")) {
            DeleteObject(h);
            RemovePropW(hwnd, L"splash_font_title");
        }
        if (auto h = (HFONT)GetPropW(hwnd, L"splash_font_body")) {
            DeleteObject(h);
            RemovePropW(hwnd, L"splash_font_body");
        }

        RemovePropW(hwnd, L"splash_main_wnd");

        UiLog_SetSplashHwnd(nullptr);
        m_logHwnd = nullptr;
        m_hwnd = nullptr;

        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}