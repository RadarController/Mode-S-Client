#include "youtube/YouTubeSupporterProvider.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <ctime>
#include <utility>

#include "json.hpp"

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

void SafeLog(const std::function<void(const std::wstring&)>& log, const std::wstring& msg) {
    if (!log) return;
    try { log(msg); } catch (...) {}
}

std::string UrlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
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

    DWORD status = 0; DWORD status_size = sizeof(status);
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

std::int64_t ParseIsoToEpochMs(const std::string& iso) {
    if (iso.size() < 19) return 0;
    std::tm tm{};
    try {
        tm.tm_year = std::stoi(iso.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(iso.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(iso.substr(8, 2));
        tm.tm_hour = std::stoi(iso.substr(11, 2));
        tm.tm_min = std::stoi(iso.substr(14, 2));
        tm.tm_sec = std::stoi(iso.substr(17, 2));
    } catch (...) {
        return 0;
    }
#ifdef _WIN32
    const __time64_t sec = _mkgmtime64(&tm);
    if (sec < 0) return 0;
    return static_cast<std::int64_t>(sec) * 1000;
#else
    const std::time_t sec = timegm(&tm);
    if (sec < 0) return 0;
    return static_cast<std::int64_t>(sec) * 1000;
#endif
}

std::string JsonString(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    return {};
}

} // namespace

namespace youtube {

YouTubeSupporterProvider::YouTubeSupporterProvider(AccessTokenFn access_token,
                                                   StringFn channel_id,
                                                   LogFn log)
    : access_token_(std::move(access_token))
    , channel_id_(std::move(channel_id))
    , log_(std::move(log)) {
}

supporter::FetchResult YouTubeSupporterProvider::FetchRecent(int limit) const {
    supporter::FetchResult out;
    if (limit <= 0) limit = 16;
    if (limit > 50) limit = 50;

    const auto token = access_token_ ? access_token_() : std::nullopt;

    if (!token.has_value() || token->empty()) {
        out.status.error = "youtube access token unavailable";
        return out;
    }

    out.status.connected = true;

    std::wstring headers;
    headers += L"Authorization: Bearer " + ToW(*token) + L"\r\n";
    const std::string path = "/youtube/v3/members?part=snippet&mode=all_current&maxResults=" +
        std::to_string(limit);

    HttpResult res = WinHttpGet(L"www.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, ToW(path), headers, true);
    if (res.status < 200 || res.status >= 300) {
        std::string body = res.body;
        if (body.size() > 400) body.resize(400);

        out.status.error = "youtube members.list request failed: HTTP " +
            std::to_string(res.status) + ": " + body;

        SafeLog(log_, L"[Supporters][YouTube] members.list failed with HTTP " +
            std::to_wstring(res.status) + L" body=" + ToW(body));
        return out;
    }

    try {
        json j = json::parse(res.body);
        if (j.contains("items") && j["items"].is_array()) {
            for (const auto& row : j["items"]) {
                supporter::RecentSupporter item;
                item.platform = "youtube";
                item.support_type = "member";
                item.id = row.value("etag", std::string{});

                const auto& snippet = row.contains("snippet") && row["snippet"].is_object()
                    ? row["snippet"] : json::object();

                if (snippet.contains("membershipsDetails") && snippet["membershipsDetails"].is_object()) {
                    const auto& memberships = snippet["membershipsDetails"];

                    if (memberships.contains("membershipsDuration") && memberships["membershipsDuration"].is_object()) {
                        const auto& duration = memberships["membershipsDuration"];
                        item.supported_at_ms = ParseIsoToEpochMs(duration.value("memberSince", std::string{}));
                    }

                    item.tier_name = memberships.value("highestAccessibleLevelDisplayName", std::string{});
                }

                if (snippet.contains("memberDetails") && snippet["memberDetails"].is_object()) {
                    const auto& member = snippet["memberDetails"];
                    item.display_name = member.value("displayName", std::string{});
                    if (item.id.empty()) item.id = member.value("channelId", std::string{});
                }

                if (item.display_name.empty()) continue;
                out.items.push_back(std::move(item));
            }
        }
    } catch (...) {
        out.status.error = "youtube members.list response parse failed";
        SafeLog(log_, L"[Supporters][YouTube] Failed to parse members.list response");
        return out;
    }

    out.status.ok = out.status.error.empty();
    out.status.item_count = static_cast<int>(out.items.size());
    return out;
}

} // namespace youtube
