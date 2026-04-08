#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include "http/WinHttpClient.h"

namespace http {
namespace {

struct WinHttpHandle {
    HINTERNET h = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET v) : h(v) {}

    ~WinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : h(other.h) {
        other.h = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (h) WinHttpCloseHandle(h);
            h = other.h;
            other.h = nullptr;
        }
        return *this;
    }

    bool valid() const { return h != nullptr; }
    operator HINTERNET() const { return h; }
};

} // namespace

HttpResult WinHttpRequestUtf8(const std::wstring& method,
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& extraHeaders,
    const std::string& body,
    bool secure)
{
    HttpResult result;

    WinHttpHandle session(WinHttpOpen(L"Mode-S Client/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.valid()) {
        result.winerr = GetLastError();
        return result;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);

    WinHttpHandle connect(WinHttpConnect(session, host.c_str(), port, 0));
    if (!connect.valid()) {
        result.winerr = GetLastError();
        return result;
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connect,
        method.c_str(),
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!request.valid()) {
        result.winerr = GetLastError();
        return result;
    }

    if (!extraHeaders.empty()) {
        WinHttpAddRequestHeaders(request,
            extraHeaders.c_str(),
            static_cast<ULONG>(-1L),
            WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);
    if (!ok) {
        result.winerr = GetLastError();
        return result;
    }

    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        result.winerr = GetLastError();
        return result;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX))
    {
        result.status = static_cast<int>(status);
    }

    std::string out;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            result.winerr = GetLastError();
            break;
        }
        if (avail == 0) {
            break;
        }

        const size_t cur = out.size();
        out.resize(cur + static_cast<size_t>(avail));

        DWORD read = 0;
        if (!WinHttpReadData(request, out.data() + cur, avail, &read)) {
            result.winerr = GetLastError();
            break;
        }

        out.resize(cur + static_cast<size_t>(read));
    }

    result.body = std::move(out);
    return result;
}

} // namespace http
