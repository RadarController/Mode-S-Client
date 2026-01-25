#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "TwitchHelixService.h"

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <string>
#include <sstream>
#include <utility>
#include <cstdint>
#include <fstream>
#include <filesystem>

#include "json.hpp"
#include "AppConfig.h"
#include "AppState.h"

using nlohmann::json;

namespace {


static void DebugLog(const std::wstring& msg)
{
    ::OutputDebugStringW((L"[ModeS] " + msg + L"\n").c_str());
}
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

    static std::string UrlEncode(const std::string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size() * 3);
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~')
            {
                out.push_back((char)c);
            }
            else {
                out.push_back('%');
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            }
        }
        return out;
    }

    static std::string Trim(const std::string& s)
    {
        auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        size_t a = 0, b = s.size();
        while (a < b && is_space((unsigned char)s[a])) a++;
        while (b > a && is_space((unsigned char)s[b - 1])) b--;
        return s.substr(a, b - a);
    }

    static HttpResult WinHttpRequest(const std::wstring& method,
        const std::wstring& host,
        INTERNET_PORT port,
        const std::wstring& path,
        const std::wstring& headers,
        const std::string& body,
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

        // Reasonable timeouts so the UI doesn't hang on shutdown.
        WinHttpSetTimeouts(hRequest, 8000, 8000, 8000, 12000);

        BOOL ok = WinHttpSendRequest(hRequest,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            headers.empty() ? 0 : (DWORD)headers.size(),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
            body.empty() ? 0 : (DWORD)body.size(),
            body.empty() ? 0 : (DWORD)body.size(),
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
    static void SafeCall(const std::function<void(bool)>& f, bool v)
    {
        if (f) f(v);
    }
    static void SafeCall(const std::function<void(int)>& f, int v)
    {
        if (f) f(v);
    }

    // Fallback: if AppConfig fields are empty (e.g., mapping mismatch), try reading the JSON directly.
    static bool TryReadTwitchFromConfigJson(std::string& login, std::string& cid, std::string& secret)
    {
        try {
            const auto path = std::filesystem::absolute("config.json");
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (s.empty()) return false;
            auto j = json::parse(s);

            // Prefer nested object if present.
            if (j.contains("twitch") && j["twitch"].is_object()) {
                const auto& t = j["twitch"];
                if (login.empty())  login  = t.value("login", "");
                if (cid.empty())    cid    = t.value("client_id", "");
                if (secret.empty()) secret = t.value("client_secret", "");
            }

            // Back-compat with flat keys.
            if (login.empty())  login  = j.value("twitch_login", "");
            if (cid.empty())    cid    = j.value("twitch_client_id", "");
            if (secret.empty()) secret = j.value("twitch_client_secret", "");

            return !(login.empty() || cid.empty() || secret.empty());
        }
        catch (...) {
            return false;
        }
    }


    static bool TryReadTwitchClientAndAccessTokenFromConfigJson(std::string& cid, std::string& access_token)
    {
        try {
            const auto path = std::filesystem::absolute("config.json");
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (s.empty()) return false;
            auto j = json::parse(s);

            // Prefer nested object if present.
            if (j.contains("twitch") && j["twitch"].is_object()) {
                const auto& t = j["twitch"];
                if (cid.empty())         cid         = t.value("client_id", "");
                if (access_token.empty()) access_token = t.value("user_access_token", "");
                if (access_token.empty()) access_token = t.value("access_token", "");
            }

            // Back-compat with flat keys.
            if (cid.empty())          cid          = j.value("twitch_client_id", "");
            if (access_token.empty()) access_token = j.value("twitch_access_token", "");

            // Additional common aliases (defensive).
            if (access_token.empty()) access_token = j.value("twitch_oauth_access_token", "");
            if (access_token.empty()) access_token = j.value("twitch_helix_access_token", "");

            return !(cid.empty() || access_token.empty());
        }
        catch (...) {
            return false;
        }
    }

} // namespace

std::thread StartTwitchHelixPoller(
    HWND hwnd,
    AppConfig& config,
    AppState& state,
    std::atomic<bool>& running,
    UINT refresh_msg,
    TwitchHelixUiCallbacks cb)
{
    return std::thread([=, &config, &state, &running]() mutable {
        SafeCall(cb.log, L"TWITCH: helix poller thread started");

        bool firstLoop = true;
        std::string token;
        std::string broadcaster_id;
        std::string last_login;
        std::int64_t token_expiry_ms = 0;

        auto log_http = [&](const char* what, const HttpResult& r) {
            std::string msg = std::string("TWITCH HELIX ") + what + ": HTTP " + std::to_string(r.status);
            if (r.winerr) msg += " winerr=" + std::to_string((unsigned)r.winerr);
            if (!r.body.empty()) {
                std::string b = r.body;
                if (b.size() > 800) b.resize(800);
                msg += " body=" + b;
            }
            SafeCall(cb.log, ToW(msg));
            };

        auto set_status = [&](const std::wstring& s) {
            SafeCall(cb.set_status, s);
            if (hwnd && refresh_msg) PostMessageW(hwnd, refresh_msg, 0, 0);
            };

        while (running) {
            if (firstLoop) {
                SafeCall(cb.log, L"TWITCH: poll loop entered");
                firstLoop = false;
            }

            std::string login = config.twitch_login;
            std::string cid = config.twitch_client_id;
            std::string secret = config.twitch_client_secret;

            // If AppConfig hasn't been populated (or uses different JSON keys), try reading config.json directly.
            if (login.empty() || cid.empty() || secret.empty()) {
                (void)TryReadTwitchFromConfigJson(login, cid, secret);
            }

            if (login.empty() || cid.empty() || secret.empty()) {
                SafeCall(cb.log, L"TWITCH: skipped (missing login/client_id/client_secret)");
                set_status(L"Helix: missing login/client id/secret");

                // Ensure metrics don't show stale values when config is missing.
                state.set_twitch_viewers(0);
                state.set_twitch_live(false);
                SafeCall(cb.set_viewers, 0);
                SafeCall(cb.set_live, false);

                Sleep(1500);
                continue;
            }

            const std::int64_t now = (std::int64_t)GetTickCount64();
            if (token.empty() || now + 30000 > token_expiry_ms) {
                std::string path =
                    "/oauth2/token?client_id=" + UrlEncode(cid) +
                    "&client_secret=" + UrlEncode(secret) +
                    "&grant_type=client_credentials";

                HttpResult r = WinHttpRequest(L"POST", L"id.twitch.tv", 443, ToW(path), L"", "", true);
                if (r.status != 200) {
        DebugLog(L"TWITCH: SearchCategories HTTP status=" + std::to_wstring((int)r.status));
                    set_status(L"Helix: token error (see log)");
                    log_http("token", r);

                    // Avoid stale metrics if auth fails.
                    state.set_twitch_viewers(0);
                    state.set_twitch_live(false);
                    SafeCall(cb.set_viewers, 0);
                    SafeCall(cb.set_live, false);

                    Sleep(5000);
                    continue;
                }
                try {
                    auto j = json::parse(r.body);
                    token = j.value("access_token", "");
                    int expires = j.value("expires_in", 0);
                    token_expiry_ms = now + (std::int64_t)expires * 1000;
                    if (token.empty()) {
                        set_status(L"Helix: token parse error");
                        log_http("token-empty", r);

                        state.set_twitch_viewers(0);
                        state.set_twitch_live(false);
                        SafeCall(cb.set_viewers, 0);
                        SafeCall(cb.set_live, false);

                        Sleep(5000);
                        continue;
                    }
                    SafeCall(cb.log, L"TWITCH: helix token ok");
                }
                catch (...) {
                    set_status(L"Helix: token parse exception");
                    log_http("token-parse", r);

                    state.set_twitch_viewers(0);
                    state.set_twitch_live(false);
                    SafeCall(cb.set_viewers, 0);
                    SafeCall(cb.set_live, false);

                    Sleep(5000);
                    continue;
                }
            }

            // Resolve broadcaster id once (and re-resolve if login changes).
            if (last_login != login) {
                broadcaster_id.clear();
                last_login = login;
                SafeCall(cb.log, L"TWITCH: helix poller rebound to login=" + ToW(login));
            }

            if (broadcaster_id.empty()) {
                std::wstring hdr = L"Client-Id: " + ToW(cid) + L"\r\nAuthorization: Bearer " + ToW(token) + L"\r\n";
                std::string path = "/helix/users?login=" + UrlEncode(login);

                HttpResult r = WinHttpRequest(L"GET", L"api.twitch.tv", 443, ToW(path), hdr, "", true);
                if (r.status != 200) {
                    set_status(L"Helix: users error (see log)");
                    log_http("users", r);

                    state.set_twitch_viewers(0);
                    state.set_twitch_live(false);
                    SafeCall(cb.set_viewers, 0);
                    SafeCall(cb.set_live, false);

                    Sleep(5000);
                    continue;
                }
                try {
                    auto j = json::parse(r.body);
                    if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
                        broadcaster_id = j["data"][0].value("id", "");
                    }
                    if (broadcaster_id.empty()) {
                        set_status(L"Helix: user id not found");
                        log_http("users-empty", r);

                        state.set_twitch_viewers(0);
                        state.set_twitch_live(false);
                        SafeCall(cb.set_viewers, 0);
                        SafeCall(cb.set_live, false);

                        Sleep(5000);
                        continue;
                    }
                }
                catch (...) {
                    set_status(L"Helix: users parse exception");
                    log_http("users-parse", r);

                    state.set_twitch_viewers(0);
                    state.set_twitch_live(false);
                    SafeCall(cb.set_viewers, 0);
                    SafeCall(cb.set_live, false);

                    Sleep(5000);
                    continue;
                }
            }

            // Streams (live + viewers)
            {
                std::wstring hdr = L"Client-Id: " + ToW(cid) + L"\r\nAuthorization: Bearer " + ToW(token) + L"\r\n";
                std::string path = "/helix/streams?user_login=" + UrlEncode(login);
                HttpResult r = WinHttpRequest(L"GET", L"api.twitch.tv", 443, ToW(path), hdr, "", true);
                if (r.status != 200) {
                    set_status(L"Helix: streams error (see log)");
                    log_http("streams", r);

                    // Avoid stale viewers/live if streams fetch fails.
                    state.set_twitch_viewers(0);
                    state.set_twitch_live(false);
                    SafeCall(cb.set_viewers, 0);
                    SafeCall(cb.set_live, false);
                }
                else {
                    try {
                        auto j = json::parse(r.body);
                        bool live = j.contains("data") && j["data"].is_array() && !j["data"].empty();
                        int viewers = 0;
                        if (live) viewers = j["data"][0].value("viewer_count", 0);

                        state.set_twitch_viewers(viewers);
                        state.set_twitch_live(live);

                        SafeCall(cb.set_viewers, viewers);
                        SafeCall(cb.set_live, live);
                    }
                    catch (...) {
                        set_status(L"Helix: streams parse exception");
                        log_http("streams-parse", r);

                        state.set_twitch_viewers(0);
                        state.set_twitch_live(false);
                        SafeCall(cb.set_viewers, 0);
                        SafeCall(cb.set_live, false);
                    }
                }
            }

            // Followers total
            {
                std::wstring hdr = L"Client-Id: " + ToW(cid) + L"\r\nAuthorization: Bearer " + ToW(token) + L"\r\n";
                std::string path = "/helix/channels/followers?broadcaster_id=" + UrlEncode(broadcaster_id);
                HttpResult r = WinHttpRequest(L"GET", L"api.twitch.tv", 443, ToW(path), hdr, "", true);
                if (r.status != 200) {
                    set_status(L"Helix: followers error (see log)");
                    log_http("followers", r);
                }
                else {
                    try {
                        auto j = json::parse(r.body);
                        int total = j.value("total", 0);

                        state.set_twitch_followers(total);
                        SafeCall(cb.set_followers, total);

                        set_status(L"Helix: OK");
                    }
                    catch (...) {
                        set_status(L"Helix: followers parse exception");
                        log_http("followers-parse", r);
                    }
                }
            }

            if (hwnd && refresh_msg) PostMessageW(hwnd, refresh_msg, 0, 0);
            Sleep(15000);
        }

        SafeCall(cb.log, L"TWITCH: helix poller thread exiting");
        });
}


bool TwitchHelixSearchCategories(
    AppConfig& /*config*/,
    const std::string& query,
    std::vector<TwitchCategory>& out,
    std::string* out_error)
{
    out.clear();


DebugLog(L"TWITCH: SearchCategories query='" + ToW(query) + L"'");
    std::string cid, tok;
    if (!TryReadTwitchClientAndAccessTokenFromConfigJson(cid, tok)) {
        DebugLog(L"TWITCH: SearchCategories failed: missing credentials in config.json");
        if (out_error) *out_error = "Missing twitch client_id/access_token in config.json";
        return false;
    }

    // Build query string (basic URL encoding for spaces and symbols)
    std::ostringstream enc;
    for (unsigned char c : query) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            enc << c;
        } else {
            enc << '%' << std::uppercase << std::hex << (int)c << std::nouppercase << std::dec;
        }
    }

    std::wstring path = L"/helix/search/categories?first=20&query=" + ToW(enc.str());

    std::wstringstream hdr;
    hdr << L"Client-ID: " << ToW(cid) << L"\r\n";
    hdr << L"Authorization: Bearer " << ToW(tok) << L"\r\n";
    hdr << L"Accept: application/json\r\n";

    auto r = WinHttpRequest(L"GET", L"api.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, path,
                            hdr.str(), "", true);

    if (r.winerr != 0) {
        DebugLog(L"TWITCH: SearchCategories WinHTTP error=" + std::to_wstring((int)r.winerr));
        if (out_error) *out_error = "WinHTTP error " + std::to_string((int)r.winerr);
        return false;
    }
    if (r.status != 200) {
        if (out_error) *out_error = "Twitch Helix HTTP " + std::to_string((int)r.status);
        return false;
    }

    try {
        auto j = json::parse(r.body);
        if (!j.contains("data") || !j["data"].is_array()) {
            DebugLog(L"TWITCH: SearchCategories OK but JSON missing data array");
            return true; // valid response shape we can't read; treat as empty
        }

        for (const auto& it : j["data"]) {
            TwitchCategory c;
            c.id = it.value("id", "");
            c.name = it.value("name", "");
            if (!c.id.empty() && !c.name.empty())
                out.push_back(std::move(c));
        }
        return true;
    }
    catch (...) {
        DebugLog(L"TWITCH: SearchCategories failed: JSON parse");
        if (out_error) *out_error = "Failed to parse Twitch Helix JSON response";
        return false;
    }
}



bool TwitchHelixUpdateChannelInfo(
    AppConfig& config,
    const std::string& title,
    const std::string& game_id,
    std::string* out_error)
{
    try {
        std::string login = config.twitch_login;
        std::string cid = config.twitch_client_id;
        // Access token is now stored under config.json -> twitch.user_access_token.
        // AppConfig may not expose a flat twitch_access_token field anymore, so read from disk.
        std::string tok;

        // Fall back to config.json if AppConfig wasn't populated.
        if (cid.empty() || tok.empty()) {
            (void)TryReadTwitchClientAndAccessTokenFromConfigJson(cid, tok);
        }
        if (login.empty()) {
            // Try reading login from config.json too (best-effort).
            std::string cid2, secret2;
            (void)TryReadTwitchFromConfigJson(login, cid2, secret2);
            if (cid.empty()) cid = cid2;
        }

        if (cid.empty() || tok.empty()) {
            if (out_error) *out_error = "Missing Twitch client_id/access_token";
            return false;
        }
        if (login.empty()) {
            if (out_error) *out_error = "Missing Twitch login";
            return false;
        }

        const std::wstring headers =
            L"Client-Id: " + ToW(cid) + L"\r\n" +
            L"Authorization: Bearer " + ToW(tok) + L"\r\n";

        // 1) Resolve broadcaster_id
        const std::string usersPath = "/helix/users?login=" + UrlEncode(login);
        HttpResult u = WinHttpRequest(L"GET", L"api.twitch.tv", 443, ToW(usersPath), headers, "", true);
        if (u.status != 200) {
            if (out_error) *out_error = "Helix users lookup failed (HTTP " + std::to_string((int)u.status) + ")";
            return false;
        }

        auto uj = json::parse(u.body);
        if (!uj.contains("data") || !uj["data"].is_array() || uj["data"].empty()) {
            if (out_error) *out_error = "Helix users lookup returned no data";
            return false;
        }

        const std::string broadcaster_id = uj["data"][0].value("id", "");
        if (broadcaster_id.empty()) {
            if (out_error) *out_error = "Helix users lookup missing broadcaster id";
            return false;
        }

        // 2) PATCH channel info
        json body;
        body["title"] = title;
        if (!game_id.empty()) body["game_id"] = game_id;

        std::wstring headers2 = headers + L"Content-Type: application/json\r\n";
        const std::string path = "/helix/channels?broadcaster_id=" + UrlEncode(broadcaster_id);

        HttpResult p = WinHttpRequest(L"PATCH", L"api.twitch.tv", 443, ToW(path), headers2, body.dump(), true);

        // Helix returns 204 No Content on success for PATCH /channels
        if (p.status != 204 && p.status != 200) {
            if (out_error) {
                std::string snippet = p.body;
                snippet = Trim(snippet);          // see helper below
                if (snippet.size() > 300) snippet.resize(300);

                *out_error = "Helix update failed (HTTP " + std::to_string((int)p.status) + ")";
                if (!snippet.empty()) {
                    *out_error += ": ";
                    *out_error += snippet;
                }
                if (p.status == 401 || p.status == 403) {
                    *out_error += " (check token scopes: channel:manage:broadcast)";
                }
            }
            return false;
        }

        return true;
    }
    catch (...) {
        if (out_error) *out_error = "Exception updating Twitch channel";
        return false;
    }
}
