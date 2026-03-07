#include "ui/WebViewHost.h"

#include <wrl.h>
#include <string>

using Microsoft::WRL::Callback;

HRESULT WebViewHost::Create(HWND parent,
    const std::wstring& buildInfo,
    OpenChatCallback onOpenChat)
{
    m_parent = parent;
    m_onOpenChat = std::move(onOpenChat);

    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        nullptr,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, buildInfo](HRESULT envResult, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(envResult) || !env) return envResult;

                return env->CreateCoreWebView2Controller(
                    m_parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, buildInfo](HRESULT ctlResult, ICoreWebView2Controller* controller) -> HRESULT
                        {
                            if (FAILED(ctlResult) || !controller) return ctlResult;

                            m_controller = controller;

                            ICoreWebView2* core = nullptr;
                            m_controller->get_CoreWebView2(&core);
                            if (!core) return E_FAIL;

                            m_webview.Attach(core);

                            {
                                EventRegistrationToken tok{};
                                m_webview->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                                        {
                                            LPWSTR jsonRaw = nullptr;
                                            args->get_WebMessageAsJson(&jsonRaw);
                                            std::wstring json = jsonRaw ? jsonRaw : L"";
                                            if (jsonRaw) CoTaskMemFree(jsonRaw);

                                            if (json.find(L"open_chat") != std::wstring::npos) {
                                                if (m_onOpenChat) m_onOpenChat();
                                            }
                                            return S_OK;
                                        }).Get(),
                                            &tok);
                            }

                            {
                                EventRegistrationToken tok{};
                                m_webview->add_NewWindowRequested(
                                    Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
                                        {
                                            LPWSTR uriRaw = nullptr;
                                            args->get_Uri(&uriRaw);
                                            std::wstring uri = uriRaw ? uriRaw : L"";
                                            if (uriRaw) CoTaskMemFree(uriRaw);

                                            if (uri.find(L"/overlay/chat.html") != std::wstring::npos) {
                                                if (m_onOpenChat) m_onOpenChat();
                                                args->put_Handled(TRUE);
                                            }
                                            return S_OK;
                                        }).Get(),
                                            &tok);
                            }

                            ICoreWebView2Settings* settings = nullptr;
                            m_webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                                settings->Release();
                            }

                            {
                                std::wstring js =
                                    L"window.__APP_BUILDINFO='" + buildInfo + L"';"
                                    L"document.addEventListener('DOMContentLoaded',function(){"
                                    L"var el=document.getElementById('buildInfo');"
                                    L"if(el){el.textContent=window.__APP_BUILDINFO;}"
                                    L"});";

                                m_webview->AddScriptToExecuteOnDocumentCreated(js.c_str(), nullptr);
                            }

                            m_controller->put_IsVisible(TRUE);
                            Resize();

                            if (!m_pendingUrl.empty()) {
                                m_webview->Navigate(m_pendingUrl.c_str());
                            }

                            return S_OK;
                        }).Get());
            }).Get());
}

void WebViewHost::Navigate(const wchar_t* url)
{
    if (!url) return;

    m_pendingUrl = url;

    if (m_webview) {
        m_webview->Navigate(m_pendingUrl.c_str());
    }
}

void WebViewHost::Resize()
{
    if (!m_controller || !m_parent) return;

    RECT rc{};
    GetClientRect(m_parent, &rc);
    m_controller->put_Bounds(rc);
}

bool WebViewHost::IsReady() const noexcept
{
    return m_webview != nullptr;
}