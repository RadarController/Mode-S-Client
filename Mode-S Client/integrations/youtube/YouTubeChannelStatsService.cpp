#include "youtube/YouTubeChannelStatsService.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <chrono>
#include <limits>
#include <string>
#include <utility>

#include "AppState.h"
#include "json.hpp"
#include "youtube/YouTubeAuth.h"

using json = nlohmann::json;

namespace {

std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

void SafeLog(const youtube::YouTubeChannelStatsService::LogFn& log, const std::wstring& msg) {
    if (!log) return;
    try { log(msg); } catch (...) {}
}

struct HttpResult {
    int status = 0;
    DWORD winerr = 0;
    std::string body;
};

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET v) : h(v) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h; }
    bool valid() const { return h != nullptr; }
};

HttpResult WinHttpGet(const std::wstring& host,
                      INTERNET_PORT port,
                      const std::wstring& path,
                      const std::wstring& extra_headers,
                      bool secure) {
    HttpResult r;
    WinHttpHandle session(WinHttpOpen(L"ModeSClient/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.valid()) { r.winerr = GetLastError(); return r; }

    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    WinHttpHandle connect(WinHttpConnect(session, host.c_str(), port, 0));
    if (!connect.valid()) { r.winerr = GetLastError(); return r; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.valid()) { r.winerr = GetLastError(); return r; }

    const std::wstring base_headers =
        L"Accept: application/json\r\n"
        L"Accept-Encoding: identity\r\n"
        L"Connection: close\r\n";
    WinHttpAddRequestHeaders(request, base_headers.c_str(), static_cast<ULONG>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    if (!extra_headers.empty()) {
        WinHttpAddRequestHeaders(request, extra_headers.c_str(), static_cast<ULONG>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) { r.winerr = GetLastError(); return r; }

    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) { r.winerr = GetLastError(); return r; }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        r.status = static_cast<int>(status);
    }

    std::string out;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) { r.winerr = GetLastError(); break; }
        if (avail == 0) break;

        const size_t cur = out.size();
        out.resize(cur + static_cast<size_t>(avail));

        DWORD read = 0;
        if (!WinHttpReadData(request, out.data() + cur, avail, &read)) { r.winerr = GetLastError(); break; }
        out.resize(cur + static_cast<size_t>(read));
    }

    r.body = std::move(out);
    return r;
}

bool TryParseSubscriberCount(const json& node, int& outCount, std::string* outError) {
    try {
        const auto items = node.value("items", json::array());
        if (!items.is_array() || items.empty()) {
            if (outError) *outError = "channels.list returned no channel items";
            return false;
        }

        const auto stats = items[0].value("statistics", json::object());
        if (!stats.is_object()) {
            if (outError) *outError = "channels.list response missing statistics";
            return false;
        }

        if (stats.value("hiddenSubscriberCount", false) && !stats.contains("subscriberCount")) {
            if (outError) *outError = "channel subscriber count is hidden";
            return false;
        }

        long long count = 0;
        if (stats.contains("subscriberCount")) {
            const auto& v = stats["subscriberCount"];
            if (v.is_string()) count = std::stoll(v.get<std::string>());
            else if (v.is_number_integer()) count = v.get<long long>();
            else if (v.is_number_float()) count = static_cast<long long>(v.get<double>());
            else {
                if (outError) *outError = "subscriberCount had an unexpected type";
                return false;
            }
        }
        else {
            if (outError) *outError = "channels.list response missing subscriberCount";
            return false;
        }

        if (count < 0) count = 0;
        if (count > static_cast<long long>(std::numeric_limits<int>::max())) {
            count = static_cast<long long>(std::numeric_limits<int>::max());
        }

        outCount = static_cast<int>(count);
        return true;
    }
    catch (const std::exception& ex) {
        if (outError) *outError = std::string("subscriberCount parse failed: ") + ex.what();
        return false;
    }
    catch (...) {
        if (outError) *outError = "subscriberCount parse failed";
        return false;
    }
}

bool TryFetchSubscriberCount(YouTubeAuth& auth, int& outCount, std::string* outError) {
    if (outError) outError->clear();
    outCount = 0;

    const auto token = auth.GetAccessToken();
    if (!token.has_value() || token->empty()) {
        if (outError) *outError = "youtube access token unavailable";
        return false;
    }

    std::wstring headers;
    headers += L"Authorization: Bearer " + ToW(*token) + L"\r\n";

    const HttpResult r = WinHttpGet(
        L"www.googleapis.com",
        INTERNET_DEFAULT_HTTPS_PORT,
        L"/youtube/v3/channels?part=statistics&mine=true&maxResults=1",
        headers,
        true);

    if (r.status != 200 || r.body.empty()) {
        if (outError) {
            *outError = "channels.list failed: HTTP " + std::to_string(r.status);
            if (r.status == 0 && r.winerr != 0) {
                *outError += " winerr=" + std::to_string(static_cast<unsigned long>(r.winerr));
            }
            if (!r.body.empty()) {
                *outError += " body=" + r.body;
            }
        }
        return false;
    }

    try {
        const auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            if (outError) *outError = "channels.list returned invalid JSON";
            return false;
        }
        return TryParseSubscriberCount(j, outCount, outError);
    }
    catch (const std::exception& ex) {
        if (outError) *outError = std::string("channels.list parse failed: ") + ex.what();
        return false;
    }
    catch (...) {
        if (outError) *outError = "channels.list parse failed";
        return false;
    }
}

} // namespace

namespace youtube {

YouTubeChannelStatsService::~YouTubeChannelStatsService() {
    Stop();
}

bool YouTubeChannelStatsService::Start(YouTubeAuth& auth, AppState& state, LogFn log) {
    Stop();

    auth_ = &auth;
    state_ = &state;
    log_ = std::move(log);

    running_.store(true);
    thread_ = std::thread([this]() { worker(); });
    return true;
}

void YouTubeChannelStatsService::Stop() {
    running_.store(false);

    if (thread_.joinable()) {
        try {
            thread_.join();
        }
        catch (...) {
        }
    }

    auth_ = nullptr;
    state_ = nullptr;
    log_ = nullptr;
}

void YouTubeChannelStatsService::worker() {
    std::string last_status;

    while (running_.load()) {
        if (!auth_ || !state_) break;

        int subscriber_count = 0;
        std::string error;
        const bool ok = TryFetchSubscriberCount(*auth_, subscriber_count, &error);

        if (!ok) {
            if (error != last_status) {
                last_status = error;
                SafeLog(log_, L"YOUTUBE: channel stats warning: " + ToW(error));
            }
        }
        else {
            state_->set_youtube_followers(subscriber_count);

            if (!last_status.empty()) {
                SafeLog(log_, L"YOUTUBE: channel stats service recovered.");
            }
            last_status.clear();
        }

        for (int i = 0; i < 60 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

} // namespace youtube
