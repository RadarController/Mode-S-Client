#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "SplashScreen.h"
#include "ui/WebViewHost.h"

#include <windows.h>
#include <string>
#include <filesystem>

#include <wrl.h>
#include "WebView2.h"

using Microsoft::WRL::ComPtr;

namespace SplashScreen {
    namespace {

        HWND gSplashWnd = nullptr;
        HWND gMainWnd = nullptr;

        std::wstring gDisplayName;
        std::wstring gVersionText;
        std::wstring gLastSplashHtmlPath;
        std::wstring gSplashHtmlContent;

        bool gSplashHtmlReady = false;
        bool gPendingCloseAfterHtmlReady = false;
        bool gSplashWindowShown = false;

        ComPtr<ICoreWebView2Controller> gSplashWebController;
        ComPtr<ICoreWebView2>           gSplashWebView;

        constexpr UINT_PTR kCloseTimerId = 1001;
        constexpr UINT kCloseDelayMs = 1400;
        const wchar_t* kSplashClassName = L"ModeSClientHtmlSplash";
        constexpr UINT WM_APP_INIT_SPLASH_WEBVIEW = WM_APP + 301;

        std::wstring GetExeDir()
        {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring p = path;
            const size_t pos = p.find_last_of(L"\\/");
            return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
        }

        std::wstring GetSplashHtmlPath()
        {
            return GetExeDir() + L"\\assets\\app\\splash\\index.html";
        }

        std::wstring GetSplashAssetDir()
        {
            return GetExeDir() + L"\\assets\\app\\splash";
        }

        std::wstring ReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to)
        {
            if (from.empty()) return text;

            size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::wstring::npos) {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
            return text;
        }

        std::wstring BuildSplashHtmlFromDisk()
        {
            const std::filesystem::path dir = GetSplashAssetDir();
            const std::filesystem::path htmlPath = dir / "index.html";
            const std::filesystem::path cssPath = dir / "splash.css";
            const std::filesystem::path jsPath = dir / "splash.js";

            gLastSplashHtmlPath = htmlPath.wstring();

            auto readFile = [](const std::filesystem::path& path) -> std::wstring {
                HANDLE hFile = CreateFileW(
                    path.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

                if (hFile == INVALID_HANDLE_VALUE) {
                    return L"";
                }

                LARGE_INTEGER size{};
                if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
                    CloseHandle(hFile);
                    return L"";
                }

                std::string bytes;
                bytes.resize(static_cast<size_t>(size.QuadPart));

                DWORD bytesRead = 0;
                if (!ReadFile(hFile, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr)) {
                    CloseHandle(hFile);
                    return L"";
                }

                CloseHandle(hFile);

                bytes.resize(bytesRead);
                if (bytes.empty()) {
                    return L"";
                }

                if (bytes.size() >= 3 &&
                    static_cast<unsigned char>(bytes[0]) == 0xEF &&
                    static_cast<unsigned char>(bytes[1]) == 0xBB &&
                    static_cast<unsigned char>(bytes[2]) == 0xBF) {
                    bytes.erase(0, 3);
                }

                int needed = MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    bytes.data(),
                    static_cast<int>(bytes.size()),
                    nullptr,
                    0);

                std::wstring out;
                if (needed > 0) {
                    out.resize(needed);
                    MultiByteToWideChar(
                        CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        bytes.data(),
                        static_cast<int>(bytes.size()),
                        out.data(),
                        needed);
                    return out;
                }

                needed = MultiByteToWideChar(
                    CP_ACP,
                    0,
                    bytes.data(),
                    static_cast<int>(bytes.size()),
                    nullptr,
                    0);

                if (needed <= 0) {
                    return L"";
                }

                out.resize(needed);
                MultiByteToWideChar(
                    CP_ACP,
                    0,
                    bytes.data(),
                    static_cast<int>(bytes.size()),
                    out.data(),
                    needed);

                return out;
                };

            std::wstring html = readFile(htmlPath);
            const std::wstring css = readFile(cssPath);
            const std::wstring js = readFile(jsPath);

            if (html.empty()) {
                return L"";
            }

            if (!css.empty()) {
                html = ReplaceAll(html, L"<link rel=\"stylesheet\" href=\"./splash.css\">", L"<style>\n" + css + L"\n</style>");
                html = ReplaceAll(html, L"<link rel=\"stylesheet\" href=\"./splash.css\" />", L"<style>\n" + css + L"\n</style>");
                html = ReplaceAll(html, L"<link rel=\"stylesheet\" href=\"./splash.css\"/>", L"<style>\n" + css + L"\n</style>");
                html = ReplaceAll(html, L"<link rel=\"stylesheet\" href='./splash.css'>", L"<style>\n" + css + L"\n</style>");
                html = ReplaceAll(html, L"<link rel=\"stylesheet\" href='./splash.css' />", L"<style>\n" + css + L"\n</style>");
                html = ReplaceAll(html, L"<link rel=\"stylesheet\" href='./splash.css'/>", L"<style>\n" + css + L"\n</style>");
            }

            if (!js.empty()) {
                html = ReplaceAll(html, L"<script src=\"./splash.js\"></script>", L"<script>\n" + js + L"\n</script>");
                html = ReplaceAll(html, L"<script src='./splash.js'></script>", L"<script>\n" + js + L"\n</script>");
            }

            return html;
        }

        std::wstring EscapeForJsSingleQuoted(const std::wstring& s)
        {
            std::wstring out;
            out.reserve(s.size() + 16);
            for (wchar_t ch : s) {
                switch (ch) {
                case L'\\': out += L"\\\\"; break;
                case L'\'': out += L"\\'"; break;
                case L'\r': break;
                case L'\n': out += L"\\n"; break;
                default: out += ch; break;
                }
            }
            return out;
        }

        void CenterWindowToWorkArea(HWND hwnd)
        {
            if (!hwnd || !IsWindow(hwnd)) return;

            RECT rc{};
            GetWindowRect(hwnd, &rc);

            RECT wa{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            const int x = wa.left + ((wa.right - wa.left) - w) / 2;
            const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

            SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
        }

        void ShowMainWindowCentered()
        {
            if (!gMainWnd || !IsWindow(gMainWnd)) return;
            CenterWindowToWorkArea(gMainWnd);
            ShowWindow(gMainWnd, SW_SHOWDEFAULT);
            UpdateWindow(gMainWnd);
            SetForegroundWindow(gMainWnd);
        }

        void ResizeSplashWebView()
        {
            if (!gSplashWnd || !gSplashWebController) return;

            RECT rc{};
            GetClientRect(gSplashWnd, &rc);
            gSplashWebController->put_Bounds(rc);
        }

        void DestroySplashWebView()
        {
            if (gSplashWebController) {
                gSplashWebController->Close();
            }
            gSplashWebView.Reset();
            gSplashWebController.Reset();
            gSplashHtmlContent.clear();
            gSplashHtmlReady = false;
        }

        void InitSplashWebView(HWND hwnd)
        {
            gLastSplashHtmlPath = GetSplashHtmlPath();
            gSplashHtmlReady = false;

            CreateCoreWebView2EnvironmentWithOptions(
                nullptr, nullptr, nullptr,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [hwnd](HRESULT envResult, ICoreWebView2Environment* env) -> HRESULT {
                        if (FAILED(envResult) || !env) {
                            return S_OK;
                        }

                        return env->CreateCoreWebView2Controller(
                            hwnd,
                            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [hwnd](HRESULT ctlResult, ICoreWebView2Controller* controller) -> HRESULT {
                                    if (FAILED(ctlResult) || !controller) {
                                        return S_OK;
                                    }

                                    gSplashWebController = controller;
                                    gSplashWebController->put_IsVisible(FALSE);

                                    ICoreWebView2* core = nullptr;
                                    gSplashWebController->get_CoreWebView2(&core);
                                    if (!core) {
                                        return S_OK;
                                    }

                                    gSplashWebView.Attach(core);

                                    ICoreWebView2Settings* settings = nullptr;
                                    gSplashWebView->get_Settings(&settings);
                                    if (settings) {
                                        settings->put_IsStatusBarEnabled(FALSE);
                                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                                        settings->put_AreDevToolsEnabled(FALSE);
                                        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                                        settings->put_IsZoomControlEnabled(FALSE);
                                        settings->Release();
                                    }

                                    std::wstring js = L"window.__SPLASH_DISPLAY_NAME='" + EscapeForJsSingleQuoted(gDisplayName) + L"';"
                                        L"window.__SPLASH_VERSION='" + EscapeForJsSingleQuoted(gVersionText) + L"';"
                                        L"window.__SPLASH_READY = false;"
                                        L"document.addEventListener('DOMContentLoaded',function(){"
                                        L"if(window.__setSplashMeta){window.__setSplashMeta(window.__SPLASH_DISPLAY_NAME, window.__SPLASH_VERSION);}"
                                        L"});";
                                    gSplashWebView->AddScriptToExecuteOnDocumentCreated(js.c_str(), nullptr);

                                    gSplashWebView->add_NavigationCompleted(
                                        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                            [hwnd](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                                BOOL success = FALSE;
                                                if (args) {
                                                    args->get_IsSuccess(&success);
                                                }

                                                gSplashHtmlReady = (success == TRUE);

                                                if (gSplashHtmlReady && gSplashWnd && IsWindow(gSplashWnd) && !gSplashWindowShown) {
                                                    gSplashWindowShown = true;
                                                    ShowWindow(gSplashWnd, SW_SHOW);
                                                    UpdateWindow(gSplashWnd);
                                                }

                                                if (gSplashWebController) {
                                                    gSplashWebController->put_IsVisible(gSplashHtmlReady ? TRUE : FALSE);
                                                }

                                                if (gSplashHtmlReady && gPendingCloseAfterHtmlReady && gSplashWnd && IsWindow(gSplashWnd)) {
                                                    gPendingCloseAfterHtmlReady = false;
                                                    SetTimer(gSplashWnd, kCloseTimerId, kCloseDelayMs, nullptr);
                                                }
                                                return S_OK;
                                            }).Get(),
                                                nullptr);

                                    ResizeSplashWebView();

                                    const std::wstring splashHtml = BuildSplashHtmlFromDisk();
                                    if (!splashHtml.empty()) {
                                        gSplashWebView->NavigateToString(splashHtml.c_str());
                                    }

                                    return S_OK;
                                }).Get());
                    }).Get());
        }

        LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg) {
            case WM_CREATE:
                PostMessageW(hwnd, WM_APP_INIT_SPLASH_WEBVIEW, 0, 0);
                return 0;

            case WM_APP_INIT_SPLASH_WEBVIEW:
                InitSplashWebView(hwnd);
                return 0;

            case WM_SIZE:
                ResizeSplashWebView();
                return 0;

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT:
            {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_TIMER:
                if (wParam == kCloseTimerId) {
                    KillTimer(hwnd, kCloseTimerId);
                    Destroy();
                    ShowMainWindowCentered();
                    return 0;
                }
                break;

            case WM_CLOSE:
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
                DestroySplashWebView();
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
            wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            wc.style = CS_HREDRAW | CS_VREDRAW;

            const ATOM atom = RegisterClassW(&wc);
            if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return false;
            }

            registered = true;
            return true;
        }

    } // namespace

    bool Create(HINSTANCE hInstance, const wchar_t* displayName, const wchar_t* versionText)
    {
        gDisplayName = displayName ? displayName : L"StreamingATC.Live";
        gVersionText = versionText ? versionText : L"";
        gLastSplashHtmlPath = GetSplashHtmlPath();
        gSplashHtmlReady = false;
        gPendingCloseAfterHtmlReady = false;
        gSplashWindowShown = false;

        if (gSplashWnd && IsWindow(gSplashWnd)) {
            return true;
        }

        if (!RegisterSplashClass(hInstance)) {
            return false;
        }

        const int w = 960;
        const int h = 540;

        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int x = wa.left + ((wa.right - wa.left) - w) / 2;
        const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

        gSplashWnd = CreateWindowExW(
            WS_EX_TOPMOST,
            kSplashClassName,
            L"Starting…",
            WS_POPUP,
            x, y, w, h,
            nullptr, nullptr, hInstance, nullptr);

        if (!gSplashWnd) {
            return false;
        }

        return true;
    }

    void OnAppReady(HWND mainWindow)
    {
        gMainWnd = mainWindow;

        if (gSplashWebView && gSplashHtmlReady) {
            gSplashWebView->ExecuteScript(
                L"window.__SPLASH_READY = true; if (window.__onNativeReady) window.__onNativeReady();",
                nullptr);
        }

        if (gSplashWnd && IsWindow(gSplashWnd)) {
            if (gSplashHtmlReady) {
                SetTimer(gSplashWnd, kCloseTimerId, kCloseDelayMs, nullptr);
            }
            else {
                gPendingCloseAfterHtmlReady = true;
            }
            return;
        }

        ShowMainWindowCentered();
    }

    void Destroy()
    {
        if (gSplashWnd && IsWindow(gSplashWnd)) {
            HWND hwnd = gSplashWnd;
            gSplashWnd = nullptr;
            DestroyWindow(hwnd);
        }
        else {
            DestroySplashWebView();
        }
    }

} // namespace SplashScreen