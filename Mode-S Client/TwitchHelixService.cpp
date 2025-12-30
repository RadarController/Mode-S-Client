#include "TwitchHelixService.h"
#include "AppState.h"
#include "AppConfig.h"

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <json.hpp>
#include <chrono>

using json = nlohmann::json;

static std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct HttpResult {
    DWORD status = 0;
    std::string body;
    DWORD winerr = 0;
    std::wstring where;
};

static HttpResult WinHttpRequestSimple(
    const wchar_t* host,
    INTERNET_PORT port,
    bool secure,
    const wchar_t* method,
    const std::wstring& path,
    const std::wstring& headers,
    const std::string& body)
{
    HttpResult r{};
    HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;

    auto closeAll = [&]() {
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
        hRequest = hConnect = hSession = nullptr;
    };

    hSession = WinHttpOpen(L"Mode-S Client/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) { r.winerr = GetLastError(); r.where = L"WinHttpOpen"; return r; }

    hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) { r.winerr = GetLastError(); r.where = L"WinHttpConnect"; closeAll(); return r; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(hConnect, method, path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { r.winerr = GetLastError(); r.where = L"WinHttpOpenRequest"; closeAll(); return r; }

    if (!headers.empty()) {
        if (!WinHttpAddRequestHeaders(hRequest, headers.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) {
            r.winerr = GetLastError(); r.where = L"WinHttpAddRequestHeaders"; closeAll(); return r;
        }
    }

    BOOL ok = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        body.empty() ? 0 : (DWORD)body.size(),
        body.empty() ? 0 : (DWORD)body.size(),
        0);

    if (!ok) { r.winerr = GetLastError(); r.where = L"WinHttpSendRequest"; closeAll(); return r; }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        r.winerr = GetLastError(); r.where = L"WinHttpReceiveResponse"; closeAll(); return r;
    }

    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    r.status = status;

    std::string out;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            r.winerr = GetLastError(); r.where = L"WinHttpQueryDataAvailable"; closeAll(); return r;
        }
        if (avail == 0) break;
        size_t old = out.size();
        out.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, out.data() + old, avail, &read)) {
            r.winerr = GetLastError(); r.where = L"WinHttpReadData"; closeAll(); return r;
        }
        out.resize(old + read);
    }

    r.body = std::move(out);
    closeAll();
    return r;
}

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}

void TwitchHelixService::Start(HWND notifyHwnd, AppState* state, AppConfig* config, LogFn log)
{
    if (th_.joinable()) return;
    hwnd_ = notifyHwnd;
    state_ = state;
    config_ = config;
    log_ = std::move(log);

    stop_.store(false);
    refresh_.store(true);

    if (log_) log_(L"TWITCH: starting Helix service");
    th_ = std::thread([this]() { ThreadMain(); });
}

void TwitchHelixService::Stop()
{
    stop_.store(true);
    refresh_.store(true);
    if (th_.joinable()) th_.join();
}

void TwitchHelixService::OnConfigChanged()
{
    refresh_.store(true);
}

std::wstring TwitchHelixService::TrimBodyForLog(const std::string& body, size_t maxBytes)
{
    if (body.size() <= maxBytes) return ToW(body);
    return ToW(body.substr(0, maxBytes)) + L"...";
}

static bool ensure_app_token(TwitchHelixService* self, std::string& tokenOut)
{
    // expects config_ fields to be present
    if (!self) return false;
    // access cached fields
    std::string client_id, client_secret;
    {
        // config is owned by UI thread; read atomically enough (strings copy)
        client_id = self->config_->twitch_client_id;
        client_secret = self->config_->twitch_client_secret;
    }
    if (client_id.empty() || client_secret.empty()) return false;

    std::lock_guard<std::mutex> lock(self->cache_mtx_);
    const auto now = now_ms();
    if (!self->token_.empty() && now < (self->token_expiry_ms_ - 30'000)) {
        tokenOut = self->token_;
        return true;
    }

    std::string body = "client_id=" + client_id + "&client_secret=" + client_secret + "&grant_type=client_credentials";
    std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    auto r = WinHttpRequestSimple(L"id.twitch.tv", 443, true, L"POST", L"/oauth2/token", headers, body);

    if (self->log_) {
        self->log_(L"TWITCH HELIX token: HTTP " + std::to_wstring(r.status) + L" " + self->TrimBodyForLog(r.body));
        if (r.winerr) self->log_(L"TWITCH HELIX token winerr: " + std::to_wstring(r.winerr) + L" @" + r.where);
    }
    if (r.status != 200) return false;

    try {
        auto j = json::parse(r.body);
        self->token_ = j.value("access_token", "");
        int expires = j.value("expires_in", 0);
        self->token_expiry_ms_ = now + (long long)expires * 1000;
        tokenOut = self->token_;
        return !tokenOut.empty();
    } catch (...) {
        return false;
    }
}

static bool ensure_user_id(TwitchHelixService* self, const std::string& token, std::string& userIdOut)
{
    if (!self) return false;
    std::string login = self->config_->twitch_login;
    std::string client_id = self->config_->twitch_client_id;
    if (login.empty() || client_id.empty()) return false;

    std::lock_guard<std::mutex> lock(self->cache_mtx_);
    if (!self->user_id_.empty() && !self->refresh_.load()) {
        userIdOut = self->user_id_;
        return true;
    }

    std::wstring path = L"/helix/users?login=" + ToW(login);
    std::wstring headers = L"Client-Id: " + ToW(client_id) + L"\r\nAuthorization: Bearer " + ToW(token) + L"\r\n";
    auto r = WinHttpRequestSimple(L"api.twitch.tv", 443, true, L"GET", path, headers, "");

    if (self->log_) {
        self->log_(L"TWITCH HELIX users: HTTP " + std::to_wstring(r.status) + L" " + self->TrimBodyForLog(r.body));
        if (r.winerr) self->log_(L"TWITCH HELIX users winerr: " + std::to_wstring(r.winerr) + L" @" + r.where);
    }
    if (r.status != 200) return false;

    try {
        auto j = json::parse(r.body);
        if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) return false;
        self->user_id_ = j["data"][0].value("id", "");
        userIdOut = self->user_id_;
        return !userIdOut.empty();
    } catch (...) {
        return false;
    }
}

static void poll_once(TwitchHelixService* self)
{
    if (!self || !self->state_ || !self->config_) return;

    std::string login = self->config_->twitch_login;
    if (login.empty() || self->config_->twitch_client_id.empty() || self->config_->twitch_client_secret.empty()) {
        if (self->log_) self->log_(L"TWITCH: skipped (missing twitch_login/client_id/client_secret)");
        return;
    }

    std::string token;
    if (!ensure_app_token(self, token)) {
        if (self->log_) self->log_(L"TWITCH: failed to obtain app token");
        return;
    }

    std::string user_id;
    if (!ensure_user_id(self, token, user_id)) {
        if (self->log_) self->log_(L"TWITCH: failed to resolve user id");
        return;
    }

    // Streams (viewers/live)
    {
        std::wstring path = L"/helix/streams?user_login=" + ToW(login);
        std::wstring headers = L"Client-Id: " + ToW(self->config_->twitch_client_id) + L"\r\nAuthorization: Bearer " + ToW(token) + L"\r\n";
        auto r = WinHttpRequestSimple(L"api.twitch.tv", 443, true, L"GET", path, headers, "");
        if (self->log_) {
            self->log_(L"TWITCH HELIX streams: HTTP " + std::to_wstring(r.status) + L" " + self->TrimBodyForLog(r.body));
            if (r.winerr) self->log_(L"TWITCH HELIX streams winerr: " + std::to_wstring(r.winerr) + L" @" + r.where);
        }
        if (r.status == 200) {
            try {
                auto j = json::parse(r.body);
                int viewers = 0;
                if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
                    viewers = j["data"][0].value("viewer_count", 0);
                }
                self->state_->set_twitch_viewers(viewers);
            } catch (...) {}
        }
    }

    // Followers
    {
        std::wstring path = L"/helix/channels/followers?broadcaster_id=" + ToW(user_id);
        std::wstring headers = L"Client-Id: " + ToW(self->config_->twitch_client_id) + L"\r\nAuthorization: Bearer " + ToW(token) + L"\r\n";
        auto r = WinHttpRequestSimple(L"api.twitch.tv", 443, true, L"GET", path, headers, "");
        if (self->log_) {
            self->log_(L"TWITCH HELIX followers: HTTP " + std::to_wstring(r.status) + L" " + self->TrimBodyForLog(r.body));
            if (r.winerr) self->log_(L"TWITCH HELIX followers winerr: " + std::to_wstring(r.winerr) + L" @" + r.where);
        }
        if (r.status == 200) {
            try {
                auto j = json::parse(r.body);
                int total = j.value("total", 0);
                self->state_->set_twitch_followers(total);
            } catch (...) {}
        }
    }

    // Notify UI to redraw sooner than timer
    if (self->hwnd_) PostMessageW(self->hwnd_, WM_APP + 41, 0, 0);
}

void TwitchHelixService::ThreadMain()
{
    if (log_) log_(L"TWITCH: helix poller thread started");
    while (!stop_.load()) {
        if (refresh_.exchange(false)) {
            std::lock_guard<std::mutex> lock(cache_mtx_);
            token_.clear(); token_expiry_ms_ = 0; user_id_.clear();
        }
        poll_once(this);
        for (int i = 0; i < 30 && !stop_.load(); ++i) Sleep(500); // ~15s
    }
    if (log_) log_(L"TWITCH: helix poller thread exiting");
}
