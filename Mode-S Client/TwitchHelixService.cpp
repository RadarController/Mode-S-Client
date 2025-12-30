#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "TwitchHelixService.h"

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <string>
#include <sstream>
#include <utility>
#include <cstdint>

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

            const std::string login = config.twitch_login;
            const std::string cid = config.twitch_client_id;
            const std::string secret = config.twitch_client_secret;

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
            if (broadcaster_id.empty() || config.twitch_login != login) {
                broadcaster_id.clear();
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