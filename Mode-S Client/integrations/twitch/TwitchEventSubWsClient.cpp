#include "twitch/TwitchEventSubWsClient.h"
#include "AppState.h"

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <cctype>
#include <chrono>

#include "json.hpp"

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

static void OutputDebug(const std::wstring& msg)
{
    std::wstring line = L"[TwitchEventSub] " + msg + L"\n";
    ::OutputDebugStringW(line.c_str());
}

static void OutputDebug(const std::string& msgUtf8)
{
    if (msgUtf8.empty()) {
        OutputDebug(std::wstring());
        return;
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, msgUtf8.c_str(), (int)msgUtf8.size(), nullptr, 0);
    std::wstring w;
    w.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, msgUtf8.c_str(), (int)msgUtf8.size(), w.data(), len);
    OutputDebug(w);
}


namespace {

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static std::int64_t NowMs()
{
    using namespace std::chrono;
    return (std::int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct HttpResult {
    int status = 0;
    DWORD winerr = 0;
    std::string body;
};

static HttpResult WinHttpRequest(const std::wstring& method,
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& headers,
    const std::string& body,
    bool secure)
{
    HttpResult r;
    HINTERNET hSession = WinHttpOpen(L"ModeS-Twitch-EventSub/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { r.winerr = GetLastError(); return r; }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { r.winerr = GetLastError(); WinHttpCloseHandle(hSession); return r; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { r.winerr = GetLastError(); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return r; }

    std::wstring hdrs = headers;
    BOOL ok = WinHttpSendRequest(hReq,
        hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
        hdrs.empty() ? 0 : (DWORD)-1L,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0);

    if (!ok) { r.winerr = GetLastError(); WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return r; }

    ok = WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) { r.winerr = GetLastError(); WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return r; }

    DWORD status = 0; DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
    {
        r.status = (int)status;
    }

    std::string out;
    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        std::string buf(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf.data(), avail, &read) || read == 0) break;
        buf.resize(read);
        out += buf;
    }
    r.body = std::move(out);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return r;
}

static bool IsAllDigits(const std::string& s)
{
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

static bool ParseWssUrl(const std::wstring& url, std::wstring& hostOut, std::wstring& pathOut)
{
    // Twitch may provide a reconnect_url with the "wss" scheme. WinHttpCrackUrl
    // is not guaranteed to understand that scheme on all Windows versions.
    // We'll try WinHttpCrackUrl first, then fall back to a manual parse.

    auto manual_parse = [&]() -> bool {
        std::wstring u = url;
        // Trim whitespace
        while (!u.empty() && iswspace(u.front())) u.erase(u.begin());
        while (!u.empty() && iswspace(u.back())) u.pop_back();

        // Normalize leading scheme to lowercase for comparison.
        auto starts_with_i = [&](const std::wstring& prefix) -> bool {
            if (u.size() < prefix.size()) return false;
            for (size_t i = 0; i < prefix.size(); ++i) {
                wchar_t a = (wchar_t)towlower(u[i]);
                wchar_t b = (wchar_t)towlower(prefix[i]);
                if (a != b) return false;
            }
            return true;
        };

        const std::wstring wss = L"wss://";
        const std::wstring https = L"https://";
        const std::wstring http = L"http://";

        size_t off = 0;
        if (starts_with_i(wss)) off = wss.size();
        else if (starts_with_i(https)) off = https.size();
        else if (starts_with_i(http)) off = http.size();
        else {
            // Some providers may omit a scheme; assume wss.
            off = 0;
        }

        // host[:port]/path?query
        size_t slash = u.find(L'/', off);
        size_t qmark = u.find(L'?', off);
        // Avoid Windows min/max macros (NOMINMAX may not be set in this TU).
        const size_t slash_pos = (slash == std::wstring::npos) ? u.size() : slash;
        const size_t qmark_pos = (qmark == std::wstring::npos) ? u.size() : qmark;
        size_t host_end = (slash_pos < qmark_pos) ? slash_pos : qmark_pos;
        if (host_end <= off) return false;

        std::wstring host = u.substr(off, host_end - off);
        // Strip :port if present (we always connect 443 for wss)
        size_t colon = host.find(L':');
        if (colon != std::wstring::npos) host = host.substr(0, colon);
        if (host.empty()) return false;

        std::wstring path;
        if (slash != std::wstring::npos) {
            path = u.substr(slash);
        } else if (qmark != std::wstring::npos) {
            // URL has no slash but has a query - treat as /?query
            path = L"/" + u.substr(qmark);
        } else {
            path = L"/ws";
        }

        hostOut = std::move(host);
        pathOut = std::move(path);
        return true;
    };

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    wchar_t extra[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = (DWORD)std::size(extra);

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        return manual_parse();
    }
    if (uc.dwHostNameLength == 0) {
        return manual_parse();
    }

    hostOut.assign(uc.lpszHostName, uc.dwHostNameLength);

    std::wstring p;
    if (uc.dwUrlPathLength > 0) p.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    std::wstring ex;
    if (uc.dwExtraInfoLength > 0) ex.assign(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (p.empty()) p = L"/ws";
    pathOut = p + ex;
    return true;
}

} // namespace


static std::string NormalizeRawAccessToken(std::string tok)
{
    auto trim_ws = [](std::string s) -> std::string {
        while (!s.empty() && (s.front()==' ' || s.front()=='\t' || s.front()=='\r' || s.front()=='\n')) s.erase(s.begin());
        while (!s.empty() && (s.back()==' ' || s.back()=='\t' || s.back()=='\r' || s.back()=='\n')) s.pop_back();
        return s;
    };

    tok = trim_ws(tok);
    if (tok.rfind("oauth:", 0) == 0) tok = tok.substr(6);
    if (tok.rfind("Bearer ", 0) == 0) tok = trim_ws(tok.substr(7));
    return tok;
}

TwitchEventSubWsClient::TwitchEventSubWsClient() = default;

TwitchEventSubWsClient::~TwitchEventSubWsClient()
{
    Stop();
}

void TwitchEventSubWsClient::Start(
    const std::string& clientId,
    const std::string& userAccessToken,
    const std::string& broadcasterId,
    ChatCallback onChatEvent,
    JsonCallback onEvent,
    JsonCallback onStatus)
{
    Stop();

    std::lock_guard<std::mutex> lk_life(lifecycle_mu_);

    client_id_ = clientId;
    access_token_ = NormalizeRawAccessToken(userAccessToken);

    broadcaster_id_ = broadcasterId;
    broadcaster_user_id_.clear();
    token_user_id_.clear();

    on_chat_event_ = std::move(onChatEvent);
    on_event_ = std::move(onEvent);
    on_status_ = std::move(onStatus);

    // reset reconnect target
    {
        std::lock_guard<std::mutex> lk(reconnect_mu_);
        ws_host_ = L"eventsub.wss.twitch.tv";
        ws_path_ = L"/ws";
        reconnect_requested_ = false;
    }

    // reset status
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        ws_state_ = "connecting";
        connected_ = false;
        subscribed_ = false;
        session_id_.clear();
        last_ws_message_ms_ = 0;
        last_keepalive_ms_ = 0;
        last_helix_ok_ms_ = 0;
        last_error_.clear();
        subscriptions_ = json::array();
    }
    EmitStatus();

    running_ = true;
    worker_ = std::thread(&TwitchEventSubWsClient::Run, this);
}

void TwitchEventSubWsClient::Stop()
{
    std::thread to_join;

    {
        // Make Stop() safe to call from multiple threads (HTTP routes, token refresh, UI).
    
        running_ = false;
        // Bump epoch so any watchdog threads exit promptly.
        run_epoch_.fetch_add(1);

        // Unblock WinHttpWebSocketReceive if it's waiting.
        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_handle_) {
                HINTERNET ws = static_cast<HINTERNET>(ws_handle_);
                // Best-effort close; ignore errors.
                WinHttpWebSocketClose(ws, 1000, nullptr, 0);
                WinHttpCloseHandle(ws);
                ws_handle_ = nullptr;
            }
        }

        // Move the worker thread out under the lifecycle lock. Join outside.
        if (worker_.joinable()) {
            to_join = std::move(worker_);
        }
    }

    if (to_join.joinable()) {
        if (to_join.get_id() == std::this_thread::get_id()) {
            // Never self-join; detach as a last resort.
            to_join.detach();
        }
        else {
            to_join.join();
        }
    }

    {
        std::lock_guard<std::mutex> lk(status_mu_);
        ws_state_ = "stopped";
        connected_ = false;
        subscribed_ = false;
        session_id_.clear();
        keepalive_timeout_sec_ = 0;
    }
    EmitStatus();
}



void TwitchEventSubWsClient::UpdateAccessToken(const std::string& userAccessToken)
{
    const std::string normalized = NormalizeRawAccessToken(userAccessToken);

    {
        std::lock_guard<std::mutex> lk(status_mu_);
        // Don't spam status if nothing changed
        if (normalized == access_token_) return;
        access_token_ = normalized;
        last_error_.clear();
    }

    // Clear cached ids derived from the previous token, and re-subscribe by reconnecting.
    broadcaster_user_id_.clear();
    token_user_id_.clear();

    // Force a reconnect so we get a fresh session and re-create subscriptions with the new token.
    RequestReconnect(L"wss://eventsub.wss.twitch.tv/ws");
    EmitStatus();
}


// ---- status helpers ----
void TwitchEventSubWsClient::SetWsState(const std::string& s)
{
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        ws_state_ = s;
    }
    EmitStatus();
}

void TwitchEventSubWsClient::SetLastError(const std::string& e)
{
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        last_error_ = e;
        ws_state_ = "error";
    }
    EmitStatus();
}

void TwitchEventSubWsClient::EmitStatus(bool helixOkTick)
{
    if (!on_status_) return;

    json out;
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        if (helixOkTick) {
            last_helix_ok_ms_ = NowMs();
        }

        out["ws_state"] = ws_state_;
        out["connected"] = connected_;
        out["session_id"] = session_id_;
        out["subscribed"] = subscribed_;
        out["broadcaster_user_id"] = broadcaster_user_id_;
        out["last_ws_message_ms"] = last_ws_message_ms_;
        out["last_keepalive_ms"] = last_keepalive_ms_;
        out["last_helix_ok_ms"] = last_helix_ok_ms_;
        out["last_error"] = last_error_;
        out["subscriptions"] = subscriptions_;
    }

    on_status_(out);
}

void TwitchEventSubWsClient::RequestReconnect(const std::wstring& wssUrl)
{
    std::wstring host, path;
    if (!ParseWssUrl(wssUrl, host, path)) {
        // Helpful diagnostic: log the URL (trimmed) so we can see what Twitch sent.
        std::wstring u = wssUrl;
        while (!u.empty() && iswspace(u.front())) u.erase(u.begin());
        while (!u.empty() && iswspace(u.back())) u.pop_back();
        if (u.size() > 240) u = u.substr(0, 240) + L"...";
        OutputDebug(L"session_reconnect: failed to parse reconnect_url: " + u);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(reconnect_mu_);
        ws_host_ = host;
        ws_path_ = path;
        reconnect_requested_ = true;
    }

    // Force receive loop to exit quickly.
    {
        std::lock_guard<std::mutex> lk(ws_mu_);
        if (ws_handle_) {
            HINTERNET ws = static_cast<HINTERNET>(ws_handle_);
            WinHttpWebSocketClose(ws, 1001, nullptr, 0);
            WinHttpCloseHandle(ws);
            ws_handle_ = nullptr;
        }
    }
}

void TwitchEventSubWsClient::Run()
{
    int attempt = 0;

    while (running_)
    {
        const std::uint64_t epoch = run_epoch_.load();

        std::wstring host, path;
        {
            std::lock_guard<std::mutex> lk(reconnect_mu_);
            host = ws_host_;
            path = ws_path_;
            reconnect_requested_ = false;
        }

        // (Re)connecting...
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            ws_state_ = "connecting";
            connected_ = false;
            subscribed_ = false;
            session_id_.clear();
            keepalive_timeout_sec_ = 0;
            last_error_.clear();
        }
        EmitStatus();

        HINTERNET hSession = nullptr;
        HINTERNET hConnect = nullptr;
        HINTERNET hRequest = nullptr;
        HINTERNET hWebSocket = nullptr;

        auto cleanup = [&]() {
            {
                std::lock_guard<std::mutex> lk(ws_mu_);
                if (ws_handle_ == hWebSocket) {
                    ws_handle_ = nullptr;
                }
            }
            if (hWebSocket) WinHttpCloseHandle(hWebSocket);
            if (hRequest)   WinHttpCloseHandle(hRequest);
            if (hConnect)   WinHttpCloseHandle(hConnect);
            if (hSession)   WinHttpCloseHandle(hSession);
            hWebSocket = nullptr; hRequest = nullptr; hConnect = nullptr; hSession = nullptr;
        };

        auto fail = [&](const std::string& what) {
            SetLastError(what);
            cleanup();
        };

        bool connected_ok = false;

        // ---- connect attempt (no goto; use connected_ok flag) ----
        hSession = WinHttpOpen(
            L"ModeS-Twitch-EventSub/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);

        if (!hSession) {
            fail("WinHttpOpen failed");
        } else {

            // Ensure modern TLS is enabled (Twitch requires TLS 1.2+).
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
            protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
            WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

            hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!hConnect) {
                fail("WinHttpConnect failed");
            } else {

                hRequest = WinHttpOpenRequest(
                    hConnect,
                    L"GET",
                    path.c_str(),
                    nullptr,
                    WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                    WINHTTP_FLAG_SECURE);

                if (!hRequest) {
                    fail("WinHttpOpenRequest failed");
                } else {

                    // Tell WinHTTP this request will upgrade to a WebSocket.
                    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
                        fail("WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET failed");
                    } else {

                        if (!WinHttpSendRequest(
                            hRequest,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0))
                        {
                            fail("WinHttpSendRequest failed");
                        }
                        else if (!WinHttpReceiveResponse(hRequest, nullptr)) {
                            fail("WinHttpReceiveResponse failed");
                        }
                        else {

                            hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
                            hRequest = nullptr;

                            if (!hWebSocket) {
                                fail("WinHttpWebSocketCompleteUpgrade failed");
                            } else {

                                {
                                    std::lock_guard<std::mutex> lk(ws_mu_);
                                    ws_handle_ = hWebSocket;
                                }

                                OutputDebug(L"connected");
                                {
                                    std::lock_guard<std::mutex> lk(status_mu_);
                                    ws_state_ = "connected";
                                    connected_ = true;
                                    last_ws_message_ms_ = NowMs();
                                    last_keepalive_ms_ = NowMs(); // until welcome gives us the real keepalive schedule
                                }
                                EmitStatus();

                                // Reset backoff on a successful connect.
                                attempt = 0;

                                // Watchdog: if keepalives stop arriving, force a reconnect.
                                std::thread watchdog([this, epoch]() {
                                    while (running_ && run_epoch_.load() == epoch)
                                    {
                                        int timeout_s = 0;
                                        std::int64_t last_keep_ms = 0;
                                        bool is_connected = false;

                                        {
                                            std::lock_guard<std::mutex> lk(status_mu_);
                                            timeout_s = keepalive_timeout_sec_;
                                            last_keep_ms = last_keepalive_ms_;
                                            is_connected = connected_;
                                        }

                                        if (is_connected && timeout_s > 0 && last_keep_ms > 0) {
                                            const std::int64_t now = NowMs();
                                            const std::int64_t grace_ms = 5000; // be generous vs. transient stalls
                                            const std::int64_t limit = (std::int64_t)timeout_s * 1000 + grace_ms;
                                            if ((now - last_keep_ms) > limit) {
                                                SetLastError("keepalive_timeout");
                                                RequestReconnect(L"wss://eventsub.wss.twitch.tv/ws");
                                                break;
                                            }
                                        }

                                        Sleep(1000);
                                    }
                                });

                                connected_ok = true;

                                ReceiveLoop(hWebSocket);

                                // End watchdog for this iteration.
                                run_epoch_.fetch_add(1);
                                if (watchdog.joinable()) watchdog.join();
                            }
                        }
                    }
                }
            }
        }

        cleanup();

        if (!running_) break;

        // ---- reconnect backoff ----
        bool wantsReconnect = false;
        {
            std::lock_guard<std::mutex> lk(reconnect_mu_);
            wantsReconnect = reconnect_requested_;
        }

        if (!wantsReconnect) {
            attempt = (std::min)(attempt + 1, 8);
        } else {
            // Twitch asked us to reconnect; keep it snappy.
            attempt = 0;
        }

        // base delay: 300ms, cap ~30s
        DWORD base = 300;
        DWORD max_delay = 30000;

        DWORD delay = base;
        if (attempt > 0) {
            delay = base * (1u << (std::min)(attempt, 6));
        }
        if (delay > max_delay) delay = max_delay;

        // jitter up to 250ms
        DWORD jitter = (DWORD)(GetTickCount() % 250);

        Sleep(delay + jitter);
    }
}
void TwitchEventSubWsClient::ReceiveLoop(void* ws)
{
    HINTERNET hWebSocket = static_cast<HINTERNET>(ws);
    std::string accum;
    std::vector<BYTE> buffer(16 * 1024);

    while (running_)
    {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;

        if (WinHttpWebSocketReceive(
            hWebSocket,
            buffer.data(),
            (DWORD)buffer.size(),
            &bytesRead,
            &bufferType) != NO_ERROR)
        {
            break;
        }

        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            break;

        // Text message or fragment?
        if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
        {
            accum.append(reinterpret_cast<const char*>(buffer.data()), bytesRead);

            // If it's a full message, handle it.
            if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            {
                HandleMessage(accum);
                accum.clear();
            }
        }
        else
        {
            // ignore binary
        }
    }
}

void TwitchEventSubWsClient::HandleMessage(const std::string& payload)
{
    json j;
    try
    {
        j = json::parse(payload);
    }
    catch (...)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(status_mu_);
        last_ws_message_ms_ = NowMs();
    }
    EmitStatus();

    if (!j.contains("metadata") || !j.contains("payload"))
        return;

    const std::string type = j["metadata"].value("message_type", "");

    if (type == "session_welcome")
    {
        const auto& pl = j["payload"];
        const std::string sessionId = pl.contains("session") ? pl["session"].value("id", "") : "";
        if (!sessionId.empty())
        {
            {
                std::lock_guard<std::mutex> lk(status_mu_);
                session_id_ = sessionId;
                keepalive_timeout_sec_ = pl.contains("session") ? pl["session"].value("keepalive_timeout_seconds", 0) : 0;
                last_keepalive_ms_ = NowMs();
                subscribed_ = false;
                subscriptions_ = json::array();
                last_error_.clear();
            }
            EmitStatus();

            OutputDebug(L"session_welcome: subscribing");
            const bool ok = SubscribeAll(sessionId);
            {
                std::lock_guard<std::mutex> lk(status_mu_);
                subscribed_ = ok;
            }
            EmitStatus(ok /*helix ok tick if any*/);
        }
        return;
    }

    if (type == "session_keepalive")
    {
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            last_keepalive_ms_ = NowMs();
        }
        EmitStatus();
        return;
    }

    if (type == "session_reconnect")
    {
        const auto& pl = j["payload"];
        const std::string url = pl.contains("session") ? pl["session"].value("reconnect_url", "") : "";
        if (!url.empty()) {
            OutputDebug(L"session_reconnect: switching to reconnect_url");
            RequestReconnect(Utf8ToWide(url));
        }
        return;
    }

    if (type == "revocation")
    {
        // Subscription revoked (permissions/expired/etc.). We don't know which ones will still succeed,
        // but we can attempt to resubscribe on the next welcome/reconnect.
        OutputDebug(L"revocation received");
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            last_error_ = "revocation";
        }
        EmitStatus();
        return;
    }

    if (type == "notification")
    {
        HandleNotification(&j["payload"]);
    }
}

std::string TwitchEventSubWsClient::ResolveBroadcasterUserId()
{
    if (!broadcaster_user_id_.empty())
        return broadcaster_user_id_;

    // If the caller already passed a numeric id, accept it.
    if (IsAllDigits(broadcaster_id_)) {
        broadcaster_user_id_ = broadcaster_id_;
        return broadcaster_user_id_;
    }

    if (client_id_.empty() || access_token_.empty() || broadcaster_id_.empty()) {
        std::lock_guard<std::mutex> lk(status_mu_);
        last_error_ = "missing client_id/access_token/broadcaster_id";
        return "";
    }

    // GET /helix/users?login=<login>
    std::wstring path = L"/helix/users?login=" + Utf8ToWide(broadcaster_id_);

    std::wstring headers;
    headers += L"Client-Id: " + Utf8ToWide(client_id_) + L"\r\n";
    headers += L"Authorization: Bearer " + Utf8ToWide(access_token_) + L"\r\n";

    HttpResult r = WinHttpRequest(L"GET", L"api.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, path, headers, "", true);
    if (r.status < 200 || r.status >= 300) {
        OutputDebug(L"ResolveBroadcasterUserId failed HTTP " + std::to_wstring(r.status) + L" body=" + Utf8ToWide(r.body));
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            last_error_ = std::string("helix/users HTTP ") + std::to_string(r.status);
            if (!subscriptions_.is_array()) subscriptions_ = json::array();
            json attempt;
            attempt["type"] = "helix.users";
            attempt["version"] = "1";
            attempt["status"] = r.status;
            attempt["ok"] = false;
            if (!r.body.empty()) attempt["body"] = r.body;
            subscriptions_.push_back(attempt);
        }
        EmitStatus();
        return "";
    }

    try {
        json jr = json::parse(r.body);
        if (jr.contains("data") && jr["data"].is_array() && !jr["data"].empty()) {
            broadcaster_user_id_ = jr["data"][0].value("id", "");
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            last_error_ = "helix/users parse_error";
        }
        EmitStatus();
        return "";
    }

    return broadcaster_user_id_;
}


std::string TwitchEventSubWsClient::ResolveTokenUserId()
{
    if (!token_user_id_.empty())
        return token_user_id_;

    if (client_id_.empty() || access_token_.empty()) {
        std::lock_guard<std::mutex> lk(status_mu_);
        last_error_ = "missing client_id/access_token";
        return "";
    }

    // GET /helix/users  (no params) returns the authenticated user for this token
    std::wstring headers;
    headers += L"Client-Id: " + Utf8ToWide(client_id_) + L"\r\n";
    headers += L"Authorization: Bearer " + Utf8ToWide(access_token_) + L"\r\n";

    HttpResult r = WinHttpRequest(L"GET", L"api.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, L"/helix/users", headers, "", true);
    if (r.status < 200 || r.status >= 300) {
        OutputDebug(L"ResolveTokenUserId failed HTTP " + std::to_wstring(r.status) + L" body=" + Utf8ToWide(r.body));
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            last_error_ = std::string("helix/users(me) HTTP ") + std::to_string(r.status);
            if (!subscriptions_.is_array()) subscriptions_ = json::array();
            json attempt;
            attempt["type"] = "helix.users.me";
            attempt["version"] = "1";
            attempt["status"] = r.status;
            attempt["ok"] = false;
            if (!r.body.empty()) attempt["body"] = r.body;
            subscriptions_.push_back(attempt);
        }
        EmitStatus();
        return "";
    }

    try {
        json jr = json::parse(r.body);
        if (jr.contains("data") && jr["data"].is_array() && !jr["data"].empty()) {
            token_user_id_ = jr["data"][0].value("id", "");
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            last_error_ = "helix/users(me) parse_error";
        }
        EmitStatus();
        return "";
    }

    return token_user_id_;
}



bool TwitchEventSubWsClient::CreateSubscription(const std::string& type,
                                               const std::string& version,
                                               const std::string& conditionJson,
                                               const std::string& sessionId)
{
    if (client_id_.empty() || access_token_.empty() || sessionId.empty())
        return false;

    json body;
    body["type"] = type;
    body["version"] = version;

    // conditionJson is expected to be a JSON object string
    json cond;
    try { cond = json::parse(conditionJson); }
    catch (...) { return false; }

    body["condition"] = cond;
    body["transport"] = json{
        {"method", "websocket"},
        {"session_id", sessionId}
    };

    std::wstring headers;
    headers += L"Content-Type: application/json\r\n";
    headers += L"Client-Id: " + Utf8ToWide(client_id_) + L"\r\n";
    headers += L"Authorization: Bearer " + Utf8ToWide(access_token_) + L"\r\n";

    HttpResult r = WinHttpRequest(
        L"POST",
        L"api.twitch.tv",
        INTERNET_DEFAULT_HTTPS_PORT,
        L"/helix/eventsub/subscriptions",
        headers,
        body.dump(),
        true);

    // Track subscription attempts for /api/twitch/eventsub/status
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        json attempt;
        attempt["type"] = type;
        attempt["version"] = version;
        attempt["status"] = r.status;
        attempt["ok"] = (r.status == 202 || (r.status >= 200 && r.status < 300) || r.status == 409);
        if (!r.body.empty()) attempt["body"] = r.body;
        subscriptions_.push_back(attempt);
        if (attempt["ok"].get<bool>()) {
            last_helix_ok_ms_ = NowMs();
        }
    }
    EmitStatus();

    if (r.status == 202 || (r.status >= 200 && r.status < 300) || r.status == 409) {
        OutputDebug(L"Subscribed: " + Utf8ToWide(type) + L" v" + Utf8ToWide(version));
        return true;
    }

    OutputDebug(L"Subscribe failed for " + Utf8ToWide(type) + L" HTTP " + std::to_wstring(r.status) + L" body=" + Utf8ToWide(r.body));
    return false;
}

bool TwitchEventSubWsClient::SubscribeAll(const std::string& sessionId)
{
    std::string broadcasterUid = ResolveBroadcasterUserId();
    if (broadcasterUid.empty()) {
        OutputDebug(L"SubscribeAll: missing broadcaster user id (check twitch_login, token, client-id)");
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            if (last_error_.empty()) last_error_ = "missing broadcaster_user_id";
        }
        EmitStatus();
        return false;
    }

    // Capture how many subscription attempts we had before this pass (for a clean per-pass summary).
    size_t beforeCount = 0;
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        beforeCount = subscriptions_.size();
    }

    // Follow (v2) requires broadcaster_user_id and moderator_user_id, and scope moderator:read:followers.
    // For v2, Twitch expects moderator_user_id to match the user represented by the token (or a moderator),
    // otherwise subscription creation can fail.
    // https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/#channelfollow
    const std::string tokenUid = ResolveTokenUserId();
    const std::string moderatorUid = !tokenUid.empty() ? tokenUid : broadcasterUid;

    bool okAny = false;

    const bool okFollow = CreateSubscription(
        "channel.follow",
        "2",
        json({ {"broadcaster_user_id", broadcasterUid}, {"moderator_user_id", moderatorUid} }).dump(),
        sessionId);

    okAny |= okFollow;

    if (!okFollow) {
        // Add extra context because follow is the most common “works on my machine” failure:
        // the token user must be the broadcaster or a moderator of the channel.
        OutputDebug(
            L"Follow subscription failed context: broadcaster_user_id=" + Utf8ToWide(broadcasterUid) +
            L" token_user_id=" + Utf8ToWide(tokenUid) +
            L" moderator_user_id(sent)=" + Utf8ToWide(moderatorUid) +
            L" (token user must be broadcaster or moderator; scope moderator:read:followers required)");
    }

    // Subscriptions (v1) require broadcaster_user_id and scope channel:read:subscriptions.
    // https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/#channelsubscribe
    const bool okSub = CreateSubscription(
        "channel.subscribe",
        "1",
        json({ {"broadcaster_user_id", broadcasterUid} }).dump(),
        sessionId);

    okAny |= okSub;

    // Gifted subscriptions (v1) require broadcaster_user_id and scope channel:read:subscriptions.
    // https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/#channelsubscriptiongift
    const bool okGift = CreateSubscription(
        "channel.subscription.gift",
        "1",
        json({ {"broadcaster_user_id", broadcasterUid} }).dump(),
        sessionId);

    okAny |= okGift;

    // Log a short per-pass summary so you can diagnose “some events missing” instantly.
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        size_t afterCount = subscriptions_.size();
        size_t added = (afterCount >= beforeCount) ? (afterCount - beforeCount) : 0;

        int ok = 0, fail = 0;
        std::wstring details;

        for (size_t i = beforeCount; i < afterCount; ++i) {
            const json& a = subscriptions_[i];
            const bool aok = a.value("ok", false);
            if (aok) ok++; else fail++;

            std::string type = a.value("type", "");
            int status = a.value("status", 0);

            details += L"  - " + Utf8ToWide(type) +
                       L" HTTP " + std::to_wstring(status) +
                       (aok ? L" (ok)" : L" (fail)") + L"";
        }

        OutputDebug(L"SubscribeAll summary: attempted=" + std::to_wstring((int)added) +
                    L" ok=" + std::to_wstring(ok) +
                    L" fail=" + std::to_wstring(fail) + L"" + details);
    }

    return okAny;
}

void TwitchEventSubWsClient::HandleNotification(const void* payloadPtr)
{
    const json& payload = *static_cast<const json*>(payloadPtr);

    if (!payload.contains("subscription") || !payload.contains("event"))
        return;

    const std::string subType = payload["subscription"].value("type", "");
    const json& ev = payload["event"];

    ChatMessage msg;
    msg.platform = "twitch";

    std::string user;
    std::string message;

    // Event payload fields differ slightly between types; prefer user_name if present.
    if (subType == "channel.follow")
    {
        user = ev.value("user_name", ev.value("user_login", ""));
        message = "followed";
    }
    else if (subType == "channel.subscribe")
    {
        user = ev.value("user_name", ev.value("user_login", ""));
        message = "subscribed";
    }
    else if (subType == "channel.subscription.gift")
    {
        user = ev.value("user_name", ev.value("user_login", ""));
        int count = ev.value("total", 1);
        message = "gifted " + std::to_string(count) + " subs";
    }
    else if (subType == "channel.raid")
    {
        user = ev.value("from_broadcaster_user_name", ev.value("from_broadcaster_user_login", ""));
        int viewers = ev.value("viewers", 0);
        message = "raided with " + std::to_string(viewers) + " viewers";
    }
    else
    {
        return;
    }

    const auto ts = NowMs();

    if (on_event_) {
        json evOut;
        evOut["ts_ms"] = ts;
        evOut["platform"] = "twitch";
        evOut["type"] = subType;
        evOut["user"] = user;
        evOut["message"] = message;
        on_event_(evOut);
    }

    // Forward EventSub events into chat as well (human-readable).
    if (on_chat_event_) {
        ChatMessage m{};
        m.platform = "twitch";
        m.user = user;
        m.message = BuildHumanReadableMessage(subType, ev);
        m.ts_ms = ts;
        on_chat_event_(m);
    }
}

std::string TwitchEventSubWsClient::BuildHumanReadableMessage(const std::string& subType, const nlohmann::json& ev) const
{
    // Keep this short and readable in the chat overlay.
    if (subType == "channel.follow")
    {
        const std::string user = ev.value("user_name", ev.value("user_login", "someone"));
        return "👋 " + user + " followed";
    }
    if (subType == "channel.subscribe")
    {
        const std::string user = ev.value("user_name", ev.value("user_login", "someone"));
        // Some payloads contain tier/is_gift; keep optional.
        const std::string tier = ev.value("tier", "");
        if (!tier.empty())
        {
            // Tier comes as "1000/2000/3000" for Twitch; make it nicer.
            std::string niceTier = tier;
            if (tier == "1000") niceTier = "Tier 1";
            else if (tier == "2000") niceTier = "Tier 2";
            else if (tier == "3000") niceTier = "Tier 3";
            return "🎉 " + user + " subscribed (" + niceTier + ")";
        }
        return "🎉 " + user + " subscribed";
    }
    if (subType == "channel.subscription.gift")
    {
        const std::string user = ev.value("user_name", ev.value("user_login", "someone"));
        const int total = ev.value("total", 1);
        return "🎁 " + user + " gifted " + std::to_string(total) + (total == 1 ? " sub" : " subs");
    }
    if (subType == "channel.raid")
    {
        const std::string from = ev.value("from_broadcaster_user_name", ev.value("from_broadcaster_user_login", "someone"));
        const int viewers = ev.value("viewers", 0);
        return "🚨 RAID! " + from + " raided with " + std::to_string(viewers) + " viewers";
    }

    // Fallback: use any message field if present.
    const std::string msg = ev.value("message", "");
    if (!msg.empty()) return "📣 " + msg;
    return "📣 Twitch event";
}

