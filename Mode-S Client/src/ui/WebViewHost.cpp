#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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

#include "ui/WebViewHost.h"

using Microsoft::WRL::ComPtr;

namespace {
    ComPtr<ICoreWebView2Environment> gSharedEnvironment;
    std::vector<std::function<void(ICoreWebView2Environment*)>> gSharedEnvironmentWaiters;
    bool                             gSharedEnvironmentCreating = false;

    ComPtr<ICoreWebView2Controller>  gMainWebController;
    ComPtr<ICoreWebView2>            gMainWebView;
    std::atomic<bool>                gHttpReady{ false };
    std::function<void(HWND)>        gOnOpenChat;

    void FlushSharedEnvironmentWaiters(ICoreWebView2Environment* env)
    {
        auto waiters = std::move(gSharedEnvironmentWaiters);
        gSharedEnvironmentWaiters.clear();
        for (auto& waiter : waiters) {
            if (waiter) waiter(env);
        }
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
                                            std::wstring json = jsonRaw ? jsonRaw : L"";
                                            if (jsonRaw) CoTaskMemFree(jsonRaw);

                                            if (json.find(L"open_chat") != std::wstring::npos) {
                                                if (gOnOpenChat) gOnOpenChat(hwnd);
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
