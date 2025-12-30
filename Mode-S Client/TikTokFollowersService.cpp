#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "TikTokFollowersService.h"

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <string>
#include <sstream>
#include <cstdint>
#include <algorithm>

#include "json.hpp"
#include "AppConfig.h"
#include "AppState.h"

using nlohmann::json;

namespace {

struct HttpResult {
    DWORD status = 0;
    DWORD winerr = 0;
    std::string body;
};

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

static std::string Trim(std::string s) {
    auto notspace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

static std::string SanitizeTikTok(std::string s) {
    s = Trim(std::move(s));
    if (!s.empty() && s[0] == '@') s.erase(s.begin());
    // TikTok usernames are case-insensitive
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static HttpResult WinHttpRequest(const std::wstring& method,
                                 const std::wstring& host,
                                 INTERNET_PORT port,
                                 const std::wstring& path,
                                 const std::wstring& headers,
                                 bool secure)
{
    HttpResult r;
    DWORD status = 0; DWORD statusSize = sizeof(status);
    std::string out;

    HINTERNET hSession = WinHttpOpen(L"Mode-S Client/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { r.winerr = GetLastError(); return r; }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { r.winerr = GetLastError(); WinHttpCloseHandle(hSession); return r; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { r.winerr = GetLastError(); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return r; }

    // Reasonable timeouts so shutdown doesn't hang.
    WinHttpSetTimeouts(hRequest, 8000, 8000, 8000, 12000);

    if (!headers.empty()) {
        WinHttpAddRequestHeaders(hRequest, headers.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0,
        0);

    if (!ok) { r.winerr = GetLastError(); goto done; }

    ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) { r.winerr = GetLastError(); goto done; }

    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
    {
        r.status = status;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) { r.winerr = GetLastError(); break; }
        if (avail == 0) break;
        size_t cur = out.size();
        out.resize(cur + avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, out.data() + cur, avail, &read)) { r.winerr = GetLastError(); break; }
        if (read < avail) out.resize(cur + read);
    }
    r.body = std::move(out);

done:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return r;
}

static void SafeCall(const std::function<void(const std::wstring&)>& f, const std::wstring& s)
{
    if (f) f(s);
}
static void SafeCall(const std::function<void(int)>& f, int v)
{
    if (f) f(v);
}

static bool ExtractJsonScript(const std::string& html, const std::string& id, std::string& outJson)
{
    // Find: <script id="ID"...> ... </script>
    std::string key = "id=\"" + id + "\"";
    size_t p = html.find(key);
    if (p == std::string::npos) return false;

    // Find end of opening tag
    p = html.rfind("<script", p);
    if (p == std::string::npos) return false;
    size_t openEnd = html.find('>', p);
    if (openEnd == std::string::npos) return false;

    // Find close tag
    size_t close = html.find("</script>", openEnd + 1);
    if (close == std::string::npos) return false;

    outJson = html.substr(openEnd + 1, close - (openEnd + 1));
    outJson = Trim(std::move(outJson));
    return !outJson.empty();
}

static bool TryParseFollowersFromSigi(const json& j, const std::string& uniqueId, int& outFollowers)
{
    // Common layout:
    //  UserModule: { users: { "<uid>": { uniqueId: "..."} }, stats: { "<uid>": { followerCount: N } } }
    if (!j.is_object()) return false;
    if (!j.contains("UserModule")) return false;
    const auto& um = j["UserModule"];
    if (!um.is_object()) return false;

    if (!um.contains("users") || !um["users"].is_object()) return false;
    const auto& users = um["users"];

    std::string foundId;
    for (auto it = users.begin(); it != users.end(); ++it) {
        if (!it.value().is_object()) continue;
        std::string u = it.value().value("uniqueId", "");
        if (u.empty()) u = it.value().value("unique_id", "");
        u = SanitizeTikTok(u);
        if (u == uniqueId) { foundId = it.key(); break; }
    }
    if (foundId.empty()) return false;

    // follower count can be in stats[foundId].followerCount
    if (um.contains("stats") && um["stats"].is_object()) {
        const auto& stats = um["stats"];
        if (stats.contains(foundId) && stats[foundId].is_object()) {
            const auto& st = stats[foundId];
            if (st.contains("followerCount") && st["followerCount"].is_number_integer()) {
                outFollowers = st["followerCount"].get<int>();
                return true;
            }
            if (st.contains("followerCount") && st["followerCount"].is_number()) {
                outFollowers = (int)st["followerCount"].get<double>();
                return true;
            }
        }
    }

    // sometimes in users[foundId].stats.followerCount
    const auto& uobj = users[foundId];
    if (uobj.contains("stats") && uobj["stats"].is_object()) {
        const auto& st = uobj["stats"];
        if (st.contains("followerCount")) {
            if (st["followerCount"].is_number_integer()) { outFollowers = st["followerCount"].get<int>(); return true; }
            if (st["followerCount"].is_number()) { outFollowers = (int)st["followerCount"].get<double>(); return true; }
        }
    }

    return false;
}

static bool TryExtractFollowerCount(const std::string& html, const std::string& uniqueId, int& outFollowers)
{
    // First choice: SIGI_STATE
    {
        std::string js;
        if (ExtractJsonScript(html, "SIGI_STATE", js)) {
            try {
                auto j = json::parse(js);
                if (TryParseFollowersFromSigi(j, uniqueId, outFollowers)) return true;
            } catch (...) {}
        }
    }

    // Second choice: UNIVERSAL_DATA_FOR_REHYDRATION (newer page bootstraps)
    {
        std::string js;
        if (ExtractJsonScript(html, "__UNIVERSAL_DATA_FOR_REHYDRATION__", js)) {
            try {
                auto j = json::parse(js);
                // Often: { "__DEFAULT_SCOPE__": { "webapp.user-detail": { "userInfo": { "user": {...}, "stats": {...}}}}}
                if (j.contains("__DEFAULT_SCOPE__")) {
                    const auto& scope = j["__DEFAULT_SCOPE__"];
                    // Try a couple of common keys without being too clever.
                    for (const char* k : { "webapp.user-detail", "webapp.user-detail.0", "webapp.user-detail.1" }) {
                        if (!scope.contains(k)) continue;
                        const auto& node = scope[k];
                        if (!node.is_object()) continue;
                        if (!node.contains("userInfo") || !node["userInfo"].is_object()) continue;
                        const auto& ui = node["userInfo"];
                        std::string u = SanitizeTikTok(ui.value("user", json::object()).value("uniqueId", ""));
                        if (!u.empty() && u != uniqueId) continue;
                        if (ui.contains("stats") && ui["stats"].is_object()) {
                            const auto& st = ui["stats"];
                            if (st.contains("followerCount")) {
                                if (st["followerCount"].is_number_integer()) { outFollowers = st["followerCount"].get<int>(); return true; }
                                if (st["followerCount"].is_number()) { outFollowers = (int)st["followerCount"].get<double>(); return true; }
                            }
                        }
                    }
                }
            } catch (...) {}
        }
    }

    return false;
}

} // namespace

std::thread StartTikTokFollowersPoller(
    HWND hwnd,
    AppConfig& config,
    AppState& state,
    std::atomic<bool>& running,
    UINT refresh_msg,
    TikTokFollowersUiCallbacks cb)
{
    return std::thread([=, &config, &state, &running]() mutable {
        SafeCall(cb.log, L"TIKTOK: followers poller thread started");

        std::string lastUser;
        int lastFollowers = -1;

        auto set_status = [&](const std::wstring& s) {
            SafeCall(cb.set_status, s);
            if (hwnd && refresh_msg) PostMessageW(hwnd, refresh_msg, 0, 0);
        };

        while (running) {
            std::string user = SanitizeTikTok(config.tiktok_unique_id);
            if (user.empty()) {
                set_status(L"TikTok: missing username");
                Sleep(3000);
                continue;
            }

            // If username changes, force a refresh.
            if (user != lastUser) {
                lastUser = user;
                lastFollowers = -1;
                set_status(L"TikTok: polling followers…");
            }

            // Fetch TikTok profile page HTML
            // NOTE: This is "best effort" scraping and may break if TikTok changes the page format.
            std::wstring hdr =
                L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
                L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                L"Accept-Language: en-GB,en;q=0.9\r\n";

            std::wstring path = L"/@" + ToW(user);
            HttpResult r = WinHttpRequest(L"GET", L"www.tiktok.com", 443, path, hdr, true);

            if (r.status != 200 || r.body.empty()) {
                std::wstring msg = L"TikTok: HTTP error " + std::to_wstring((unsigned)r.status);
                if (r.winerr) msg += L" (winerr=" + std::to_wstring((unsigned)r.winerr) + L")";
                set_status(msg);
                SafeCall(cb.log, msg);
                Sleep(15000);
                continue;
            }

            int followers = 0;
            if (!TryExtractFollowerCount(r.body, user, followers)) {
                set_status(L"TikTok: follower parse error");
                SafeCall(cb.log, L"TIKTOK: failed to parse followerCount from profile page");
                Sleep(20000);
                continue;
            }

            if (followers != lastFollowers) {
                lastFollowers = followers;
                state.set_tiktok_followers(followers);
                SafeCall(cb.set_followers, followers);
                set_status(L"TikTok: followers ok");
            }

            // Poll interval: 60s. (TikTok is rate-limited; keep this gentle.)
            for (int i = 0; i < 60 && running; ++i) Sleep(1000);
        }
    });
}
