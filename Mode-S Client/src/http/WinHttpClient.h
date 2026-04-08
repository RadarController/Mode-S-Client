#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <string>

namespace http {

struct HttpResult {
    int status = 0;
    DWORD winerr = 0;
    std::string body;
};

HttpResult WinHttpRequestUtf8(const std::wstring& method,
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& extraHeaders,
    const std::string& body,
    bool secure);

} // namespace http
