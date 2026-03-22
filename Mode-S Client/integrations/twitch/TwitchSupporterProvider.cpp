#include "twitch/TwitchSupporterProvider.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

#include "json.hpp"
#include "AppState.h"

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

std::string JsonString(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return {};
}

std::int64_t JsonInt64(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_string()) {
        try { return std::stoll(it->get<std::string>()); } catch (...) { return 0; }
    }
    return 0;
}

std::string NiceTier(const std::string& tier) {
    if (tier == "1000") return "Tier 1";
    if (tier == "2000") return "Tier 2";
    if (tier == "3000") return "Tier 3";
    return tier;
}

} // namespace

namespace twitch {

TwitchSupporterProvider::TwitchSupporterProvider(AppState& state,
                                                 std::string login,
                                                 AccessTokenFn access_token,
                                                 std::function<std::optional<std::string>()> client_id,
                                                 LogFn log)
    : state_(state)
    , login_(std::move(login))
    , access_token_(std::move(access_token))
    , client_id_(std::move(client_id))
    , log_(std::move(log)) {
}

supporter::FetchResult TwitchSupporterProvider::FetchRecent(int limit) const {
    supporter::FetchResult out;
    if (limit <= 0) limit = 16;

    const auto token = access_token_ ? access_token_() : std::nullopt;
    if (login_.empty()) {
        out.status.error = "twitch login not configured";
        return out;
    }
    if (!token.has_value() || token->empty()) {
        out.status.error = "twitch access token unavailable";
        return out;
    }

    out.status.connected = true;

    std::unordered_set<std::string> seen_ids;

    try {
        const json evs = state_.twitch_eventsub_events_json(200);
        auto it = evs.find("events");
        if (it != evs.end() && it->is_array()) {
            for (auto rit = it->rbegin(); rit != it->rend(); ++rit) {
                const auto& ev = *rit;
                const std::string type = JsonString(ev, "type");
                if (type != "channel.subscribe" && type != "channel.subscription.message" && type != "channel.subscription.gift") {
                    continue;
                }

                supporter::RecentSupporter item;
                item.platform = "twitch";
                item.display_name = JsonString(ev, "user");
                if (item.display_name.empty()) continue;
                item.id = "eventsub:" + item.display_name + ":" + std::to_string(JsonInt64(ev, "ts_ms"));
                item.supported_at_ms = JsonInt64(ev, "ts_ms");
                item.support_type = "subscriber";
                item.is_gift = (type == "channel.subscription.gift");
                item.is_renewal = (type == "channel.subscription.message");
                if (seen_ids.insert(item.id).second) {
                    out.items.push_back(std::move(item));
                    if (static_cast<int>(out.items.size()) >= limit) break;
                }
            }
        }
    } catch (...) {
        SafeLog(log_, L"[Supporters][Twitch] Failed to read EventSub cache");
    }

    // Use the Helix endpoint only as a fallback/source of current subscribers.
    // Twitch subscriber rows do not give a strong recent-event timestamp, so these are intentionally
    // given supported_at_ms=0 and will sort behind EventSub-backed items.
    const auto client_id_opt = client_id_ ? client_id_() : std::nullopt;
    const std::string client_id = client_id_opt.value_or("");

    if (client_id.empty()) {
        // No reliable client id accessor is wired into this provider yet. We can still return EventSub-backed items.
        out.status.ok = out.status.error.empty();
        out.status.item_count = static_cast<int>(out.items.size());
        if (out.items.empty()) {
            out.status.error = "twitch client id unavailable for Helix subscriber fallback";
        }
        return out;
    }

    std::wstring headers = L"Authorization: Bearer " + ToW(*token) + L"\r\nClient-Id: " + ToW(client_id) + L"\r\n";

    std::string broadcaster_id;
    {
        const std::string user_path = "/helix/users?login=" + UrlEncode(login_);
        HttpResult user_res = WinHttpGet(L"api.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, ToW(user_path), headers, true);
        if (user_res.status >= 200 && user_res.status < 300) {
            try {
                json j = json::parse(user_res.body);
                if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
                    broadcaster_id = j["data"][0].value("id", std::string{});
                }
            } catch (...) {}
        }
    }

    if (!broadcaster_id.empty() && static_cast<int>(out.items.size()) < limit) {
        const std::string subs_path = "/helix/subscriptions?broadcaster_id=" + UrlEncode(broadcaster_id) + "&first=" + std::to_string(limit);
        HttpResult subs_res = WinHttpGet(L"api.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, ToW(subs_path), headers, true);
        if (subs_res.status >= 200 && subs_res.status < 300) {
            try {
                json j = json::parse(subs_res.body);
                if (j.contains("data") && j["data"].is_array()) {
                    for (const auto& row : j["data"]) {
                        supporter::RecentSupporter item;
                        item.platform = "twitch";
                        item.id = row.value("user_id", std::string{});
                        item.display_name = row.value("user_name", row.value("user_login", std::string{}));
                        item.supported_at_ms = 0;
                        item.support_type = "subscriber";
                        item.tier_name = NiceTier(row.value("tier", std::string{}));
                        item.is_gift = row.value("is_gift", false);
                        if (item.display_name.empty()) continue;
                        if (!item.id.empty() && !seen_ids.insert("helix:" + item.id).second) continue;
                        out.items.push_back(std::move(item));
                        if (static_cast<int>(out.items.size()) >= limit) break;
                    }
                }
            } catch (...) {
                SafeLog(log_, L"[Supporters][Twitch] Failed to parse Helix subscriptions response");
            }
        } else if (out.items.empty()) {
            out.status.error = "twitch subscriptions request failed";
        }
    }

    out.status.ok = out.status.error.empty();
    out.status.item_count = static_cast<int>(out.items.size());
    return out;
}

} // namespace twitch
