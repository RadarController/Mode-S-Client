#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#if defined(__has_include)
#  if __has_include("WebView2.h")
#    include <wrl.h>
#    include "WebView2.h"
#    define HAVE_WEBVIEW2 1
#  else
#    define HAVE_WEBVIEW2 0
#  endif
#else
#  define HAVE_WEBVIEW2 0
#endif

#if !HAVE_WEBVIEW2
#error Mode-S Client now requires WebView2 to build.
#endif

#include <atomic>
#include <string>
#include <functional>
#include <vector>

#include "resource.h"
#include "ui/WebViewHost.h"

using Microsoft::WRL::ComPtr;

namespace {
    constexpr wchar_t kAuthPopupClassName[] = L"RCAuthPopupWindow";

    ComPtr<ICoreWebView2Environment> gSharedEnvironment;
    std::vector<std::function<void(ICoreWebView2Environment*)>> gSharedEnvironmentWaiters;
    bool                             gSharedEnvironmentCreating = false;

    ComPtr<ICoreWebView2Controller>  gMainWebController;
    ComPtr<ICoreWebView2>            gMainWebView;
    std::atomic<bool>                gHttpReady{ false };
    std::function<void(HWND)>        gOnOpenChat;

    HWND                             gAuthPopupHwnd = nullptr;
    ComPtr<ICoreWebView2Controller>  gAuthPopupController;
    ComPtr<ICoreWebView2>            gAuthPopupWebView;
    std::wstring                     gAuthPopupInitialUrl;
    std::wstring                     gAuthPopupTitle;
    std::wstring                     gAuthPopupPlatform;

    void FlushSharedEnvironmentWaiters(ICoreWebView2Environment* env)
    {
        auto waiters = std::move(gSharedEnvironmentWaiters);
        gSharedEnvironmentWaiters.clear();
        for (auto& waiter : waiters) {
            if (waiter) waiter(env);
        }
    }

    std::wstring JsonUnescape(const std::wstring& s)
    {
        std::wstring out;
        out.reserve(s.size());

        bool esc = false;
        for (wchar_t ch : s) {
            if (!esc) {
                if (ch == L'\\') {
                    esc = true;
                    continue;
                }
                out.push_back(ch);
                continue;
            }

            switch (ch) {
            case L'\\': out.push_back(L'\\'); break;
            case L'"':  out.push_back(L'"'); break;
            case L'/':  out.push_back(L'/'); break;
            case L'b':  out.push_back(L'\b'); break;
            case L'f':  out.push_back(L'\f'); break;
            case L'n':  out.push_back(L'\n'); break;
            case L'r':  out.push_back(L'\r'); break;
            case L't':  out.push_back(L'\t'); break;
            default:    out.push_back(ch); break;
            }

            esc = false;
        }

        if (esc) out.push_back(L'\\');
        return out;
    }

    bool ExtractJsonStringValue(const std::wstring& json,
                                const std::wstring& key,
                                std::wstring* out)
    {
        if (out) out->clear();

        const std::wstring token = L"\"" + key + L"\"";
        size_t pos = json.find(token);
        if (pos == std::wstring::npos) return false;

        pos = json.find(L':', pos + token.size());
        if (pos == std::wstring::npos) return false;

        pos = json.find(L'"', pos + 1);
        if (pos == std::wstring::npos) return false;

        ++pos;
        std::wstring raw;
        bool esc = false;
        for (; pos < json.size(); ++pos) {
            const wchar_t ch = json[pos];
            if (!esc && ch == L'"') {
                if (out) *out = JsonUnescape(raw);
                return true;
            }

            raw.push_back(ch);

            if (!esc && ch == L'\\') esc = true;
            else esc = false;
        }

        return false;
    }

    RECT CenteredRectForWindow(HWND owner, int width, int height)
    {
        RECT rc{};
        RECT anchor{};

        if (owner && IsWindow(owner) && GetWindowRect(owner, &anchor)) {
            rc.left = anchor.left + (((anchor.right - anchor.left) - width) / 2);
            rc.top = anchor.top + (((anchor.bottom - anchor.top) - height) / 2);
        } else {
            rc.left = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
            rc.top = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        }

        rc.right = rc.left + width;
        rc.bottom = rc.top + height;
        return rc;
    }

    void ResizeAuthPopupToClient()
    {
        if (!gAuthPopupController || !gAuthPopupHwnd) return;

        RECT rc{};
        GetClientRect(gAuthPopupHwnd, &rc);
        gAuthPopupController->put_Bounds(rc);
    }

    void CleanupAuthPopup()
    {
        if (gAuthPopupController) {
            gAuthPopupController->Close();
        }
        gAuthPopupWebView.Reset();
        gAuthPopupController.Reset();
        gAuthPopupHwnd = nullptr;
        gAuthPopupInitialUrl.clear();
        gAuthPopupTitle.clear();
        gAuthPopupPlatform.clear();
    }

    void CloseAuthPopup()
    {
        if (gAuthPopupHwnd && IsWindow(gAuthPopupHwnd)) {
            DestroyWindow(gAuthPopupHwnd);
            return;
        }
        CleanupAuthPopup();
    }

    LRESULT CALLBACK AuthPopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg) {
        case WM_SIZE:
            ResizeAuthPopupToClient();
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            CleanupAuthPopup();
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void EnsureAuthPopupWindowClass()
    {
        static bool registered = false;
        if (registered) return;

        WNDCLASSW wc{};
        wc.lpfnWndProc = AuthPopupWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kAuthPopupClassName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        RegisterClassW(&wc);
        registered = true;
    }

    void OpenExternalUrl(const std::wstring& url)
    {
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenAuthPopupInternal(HWND owner,
                               const std::wstring& url,
                               const std::wstring& title,
                               const std::wstring& platform)
    {
        gAuthPopupInitialUrl = url;
        gAuthPopupTitle = title.empty() ? L"Sign in" : title;
        gAuthPopupPlatform = platform;

        if (gAuthPopupHwnd && IsWindow(gAuthPopupHwnd)) {
            SetWindowTextW(gAuthPopupHwnd, gAuthPopupTitle.c_str());
            ShowWindow(gAuthPopupHwnd, SW_SHOWNORMAL);
            SetForegroundWindow(gAuthPopupHwnd);
            if (gAuthPopupWebView) {
                gAuthPopupWebView->Navigate(gAuthPopupInitialUrl.c_str());
            }
            return;
        }

        EnsureAuthPopupWindowClass();

        const RECT rc = CenteredRectForWindow(owner, 980, 760);
        gAuthPopupHwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kAuthPopupClassName,
            gAuthPopupTitle.c_str(),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,
            rc.left,
            rc.top,
            rc.right - rc.left,
            rc.bottom - rc.top,
            owner,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        if (!gAuthPopupHwnd) {
            CleanupAuthPopup();
            return;
        }

        ShowWindow(gAuthPopupHwnd, SW_SHOWNORMAL);
        UpdateWindow(gAuthPopupHwnd);

        WebViewHost::WithSharedEnvironment(
            [url = gAuthPopupInitialUrl](ICoreWebView2Environment* env) {
                if (!env || !gAuthPopupHwnd || !IsWindow(gAuthPopupHwnd)) return;

                env->CreateCoreWebView2Controller(
                    gAuthPopupHwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [url](HRESULT ctlResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(ctlResult) || !controller || !gAuthPopupHwnd) {
                                return ctlResult;
                            }

                            gAuthPopupController = controller;

                            ICoreWebView2* core = nullptr;
                            gAuthPopupController->get_CoreWebView2(&core);
                            if (core) {
                                gAuthPopupWebView.Attach(core);

                                {
                                    EventRegistrationToken tok{};
                                    gAuthPopupWebView->add_WebMessageReceived(
                                        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                                LPWSTR jsonRaw = nullptr;
                                                args->get_WebMessageAsJson(&jsonRaw);
                                                const std::wstring json = jsonRaw ? jsonRaw : L"";
                                                if (jsonRaw) CoTaskMemFree(jsonRaw);

                                                std::wstring type;
                                                if (!ExtractJsonStringValue(json, L"type", &type)) {
                                                    return S_OK;
                                                }

                                                if (type == L"auth_popup_result") {
                                                    std::wstring status;
                                                    std::wstring title;
                                                    ExtractJsonStringValue(json, L"status", &status);
                                                    ExtractJsonStringValue(json, L"title", &title);

                                                    if (!title.empty() && gAuthPopupHwnd && IsWindow(gAuthPopupHwnd)) {
                                                        SetWindowTextW(gAuthPopupHwnd, title.c_str());
                                                    }

                                                    if (status == L"success") {
                                                        CloseAuthPopup();
                                                    } else if (gAuthPopupHwnd && IsWindow(gAuthPopupHwnd)) {
                                                        ShowWindow(gAuthPopupHwnd, SW_SHOWNORMAL);
                                                        SetForegroundWindow(gAuthPopupHwnd);
                                                    }
                                                }

                                                return S_OK;
                                            }).Get(),
                                        &tok);
                                }

                                {
                                    EventRegistrationToken tok{};
                                    gAuthPopupWebView->add_NewWindowRequested(
                                        Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                                LPWSTR uriRaw = nullptr;
                                                args->get_Uri(&uriRaw);
                                                const std::wstring uri = uriRaw ? uriRaw : L"";
                                                if (uriRaw) CoTaskMemFree(uriRaw);

                                                if (!uri.empty()) {
                                                    OpenExternalUrl(uri);
                                                    args->put_Handled(TRUE);
                                                }

                                                return S_OK;
                                            }).Get(),
                                        &tok);
                                }

                                ICoreWebView2Settings* settings = nullptr;
                                gAuthPopupWebView->get_Settings(&settings);
                                if (settings) {
                                    settings->put_IsStatusBarEnabled(FALSE);
                                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                                    settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                                    settings->Release();
                                }

                                gAuthPopupWebView->Navigate(url.c_str());
                            }

                            gAuthPopupController->put_IsVisible(TRUE);
                            ResizeAuthPopupToClient();
                            return S_OK;
                        }).Get());
            });
    }
}

namespace WebViewHost {


void EnsureSharedEnvironment()
{
    if (gSharedEnvironment || gSharedEnvironmentCreating) {
        return;
    }

    gSharedEnvironmentCreating = true;

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT envResult, ICoreWebView2Environment* env) -> HRESULT {
                gSharedEnvironmentCreating = false;

                if (SUCCEEDED(envResult) && env) {
                    gSharedEnvironment = env;
                }

                FlushSharedEnvironmentWaiters(gSharedEnvironment.Get());
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        gSharedEnvironmentCreating = false;
        FlushSharedEnvironmentWaiters(nullptr);
    }
}

void WithSharedEnvironment(std::function<void(ICoreWebView2Environment*)> onReady)
{
    if (gSharedEnvironment) {
        onReady(gSharedEnvironment.Get());
        return;
    }

    gSharedEnvironmentWaiters.push_back(std::move(onReady));
    EnsureSharedEnvironment();
}

bool Create(HWND hwnd,
            const std::wstring& initialUrl,
            const std::wstring& buildInfo,
            std::function<void(HWND)> onOpenChat)
{
    gOnOpenChat = std::move(onOpenChat);

    WithSharedEnvironment(
        [hwnd, initialUrl, buildInfo](ICoreWebView2Environment* env) {
            if (!env) return;

            env->CreateCoreWebView2Controller(
                hwnd,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [hwnd, initialUrl, buildInfo](HRESULT ctlResult, ICoreWebView2Controller* controller) -> HRESULT {
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
                                            const std::wstring json = jsonRaw ? jsonRaw : L"";
                                            if (jsonRaw) CoTaskMemFree(jsonRaw);

                                            std::wstring type;
                                            if (!ExtractJsonStringValue(json, L"type", &type)) {
                                                if (json.find(L"open_chat") != std::wstring::npos) {
                                                    if (gOnOpenChat) gOnOpenChat(hwnd);
                                                }
                                                return S_OK;
                                            }

                                            if (type == L"open_chat") {
                                                if (gOnOpenChat) gOnOpenChat(hwnd);
                                                return S_OK;
                                            }

                                            if (type == L"open_auth_popup") {
                                                std::wstring url;
                                                std::wstring title;
                                                std::wstring platform;
                                                ExtractJsonStringValue(json, L"url", &url);
                                                ExtractJsonStringValue(json, L"title", &title);
                                                ExtractJsonStringValue(json, L"platform", &platform);

                                                if (!url.empty()) {
                                                    OpenAuthPopupInternal(hwnd, url, title, platform);
                                                }
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
                                                if (gOnOpenChat) gOnOpenChat(hwnd);
                                                args->put_Handled(TRUE);
                                                return S_OK;
                                            }

                                            if (uri.find(L"/auth/twitch/start") != std::wstring::npos ||
                                                uri.find(L"/auth/youtube/start") != std::wstring::npos) {
                                                const std::wstring title =
                                                    (uri.find(L"/auth/twitch/start") != std::wstring::npos)
                                                    ? L"Twitch sign-in"
                                                    : L"YouTube sign-in";
                                                const std::wstring platform =
                                                    (uri.find(L"/auth/twitch/start") != std::wstring::npos)
                                                    ? L"twitch"
                                                    : L"youtube";

                                                OpenAuthPopupInternal(hwnd, uri, title, platform);
                                                args->put_Handled(TRUE);
                                                return S_OK;
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
                                std::wstring js = L"window.__APP_BUILDINFO='" + buildInfo + L"';"
                                                  L"document.addEventListener('DOMContentLoaded',function(){"
                                                  L"var el=document.getElementById('buildInfo');"
                                                  L"if(el){el.textContent=window.__APP_BUILDINFO;}"
                                                  L"});";
                                gMainWebView->AddScriptToExecuteOnDocumentCreated(js.c_str(), nullptr);
                            }

                            if (gHttpReady.load()) {
                                gMainWebView->Navigate(initialUrl.c_str());
                            }
                        }

                        gMainWebController->put_IsVisible(TRUE);
                        RECT rc{};
                        GetClientRect(hwnd, &rc);
                        gMainWebController->put_Bounds(rc);
                        return S_OK;
                    }).Get());
        });

    return true;
}

void Navigate(const std::wstring& url)
{
    if (gMainWebView) {
        gMainWebView->Navigate(url.c_str());
    }
}

void SetHttpReadyAndNavigate(const std::wstring& url)
{
    gHttpReady = true;
    Navigate(url);
}

void ResizeToClient(HWND hwnd)
{
    if (!gMainWebController) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    gMainWebController->put_Bounds(rc);
}

void Destroy()
{
    CloseAuthPopup();

    if (gMainWebController) {
        gMainWebController->Close();
    }
    gMainWebView.Reset();
    gMainWebController.Reset();
    gOnOpenChat = nullptr;
    gHttpReady = false;
}

bool IsReady()
{
    return gMainWebView != nullptr;
}

} // namespace WebViewHost
