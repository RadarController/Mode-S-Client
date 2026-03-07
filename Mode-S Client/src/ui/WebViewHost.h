#pragma once

#include <windows.h>
#include <functional>
#include <string>

#ifndef HAVE_WEBVIEW2
#  if defined(__has_include)
#    if __has_include("WebView2.h")
#      include <wrl.h>
#      include "WebView2.h"
#      define HAVE_WEBVIEW2 1
#    else
#      define HAVE_WEBVIEW2 0
#    endif
#  else
#    define HAVE_WEBVIEW2 0
#  endif
#endif

#if !HAVE_WEBVIEW2
#error WebViewHost requires WebView2 to build.
#endif

class WebViewHost
{
public:
    using OpenChatCallback = std::function<void()>;

    WebViewHost() = default;
    ~WebViewHost() = default;

    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    HRESULT Create(HWND parent,
        const std::wstring& buildInfo,
        OpenChatCallback onOpenChat);

    void Navigate(const wchar_t* url);
    void Resize();
    bool IsReady() const noexcept;

private:
    HWND m_parent = nullptr;
    OpenChatCallback m_onOpenChat;
    std::wstring m_pendingUrl;

    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webview;
};