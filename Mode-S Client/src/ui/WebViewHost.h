#pragma once

#include <windows.h>
#include <string>
#include <functional>

namespace WebViewHost {

bool Create(HWND hwnd,
            const std::wstring& initialUrl,
            const std::wstring& buildInfo,
            std::function<void(HWND)> onOpenChat);

void Navigate(const std::wstring& url);
void SetHttpReadyAndNavigate(const std::wstring& url);
void ResizeToClient(HWND hwnd);
void Destroy();
bool IsReady();

} // namespace WebViewHost
