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

static void DebugLog(const std::wstring& w)
{
    OutputDebugStringW((L"[TwitchEventSub] " + w + L"\n").c_str());
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
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);

    // Note: WinHttpCrackUrl expects a scheme like "wss://"
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
        return false;

    if (uc.dwHostNameLength == 0)
        return false;

    hostOut.assign(uc.lpszHostName, uc.dwHostNameLength);

    // Include extra info (query) if present
    std::wstring p;
    if (uc.dwUrlPathLength > 0)
        p.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (p.empty())
        p = L"/ws";
    pathOut = p;
    return true;
}

} // namespace

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

    client_id_ = clientId;
    access_token_ = userAccessToken;
    broadcaster_id_ = broadcasterId;
    broadcaster_user_id_.clear();

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

void TwitchEventSubWsClient::Stopvoid TwitchEventSubWsClient::Stop()
{
    running_ = false;

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

    if (worker_.joinable())
        worker_.join();

    {
        std::lock_guard<std::mutex> lk(status_mu_);
        ws_state_ = "stopped";
        connected_ = false;
        subscribed_ = false;
        session_id_.clear();
    }
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

void TwitchEventSubWsClient::RequestReconnectvoid TwitchEventSubWsClient::RequestReconnect(const std::wstring& wssUrl)
{
    std::wstring host, path;
    if (!ParseWssUrl(wssUrl, host, path)) {
        DebugLog(L"session_reconnect: failed to parse reconnect_url");
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
    while (running_)
    {
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
        }
        EmitStatus();

        HINTERNET hSession = nullptr;
        HINTERNET hConnect = nullptr;
        HINTERNET hRequest = nullptr;
        HINTERNET hWebSocket = nullptr;

        hSession = WinHttpOpen(
            L"ModeS-Twitch-EventSub/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);

        if (!hSession) {
            DebugLog(L"WinHttpOpen failed");
            break;
        }

        hConnect = WinHttpConnect(
            hSession,
            host.c_str(),
            INTERNET_DEFAULT_HTTPS_PORT,
            0);

        if (!hConnect) {
            DebugLog(L"WinHttpConnect failed");
            WinHttpCloseHandle(hSession);
            break;
        }

        hRequest = WinHttpOpenRequest(
            hConnect,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);

        if (!hRequest) {
            DebugLog(L"WinHttpOpenRequest failed");
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            break;
        }

        if (!WinHttpSendRequest(
            hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0))
        {
            DebugLog(L"WinHttpSendRequest failed");
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            break;
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            DebugLog(L"WinHttpReceiveResponse failed");
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            break;
        }

        hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
        hRequest = nullptr;

        if (!hWebSocket) {
            DebugLog(L"WinHttpWebSocketCompleteUpgrade failed");
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            break;
        }

        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            ws_handle_ = hWebSocket;
        }

        DebugLog(L"connected");
        {
            std::lock_guard<std::mutex> lk(status_mu_);
            ws_state_ = "connected";
            connected_ = true;
        }
        EmitStatus();
        ReceiveLoop(hWebSocket);

        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_handle_ == hWebSocket)
                ws_handle_ = nullptr;
        }

        WinHttpCloseHandle(hWebSocket);
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);

        if (!running_)
            break;

        // If not explicitly asked to reconnect, back off a touch.
        bool wantsReconnect = false;
        {
            std::lock_guard<std::mutex> lk(reconnect_mu_);
            wantsReconnect = reconnect_requested_;
        }
        if (!wantsReconnect)
            Sleep(750);
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
                subscribed_ = false;
                subscriptions_ = json::array();
                last_error_.clear();
            }
            EmitStatus();

            DebugLog(L"session_welcome: subscribing");
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
            DebugLog(L"session_reconnect: switching to reconnect_url");
            RequestReconnect(Utf8ToWide(url));
        }
        return;
    }

    if (type == "revocation")
    {
        // Subscription revoked (permissions/expired/etc.). We don't know which ones will still succeed,
        // but we can attempt to resubscribe on the next welcome/reconnect.
        DebugLog(L"revocation received");
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

    if (client_id_.empty() || access_token_.empty() || broadcaster_id_.empty())
        return "";

    // GET /helix/users?login=<login>
    std::wstring path = L"/helix/users?login=" + Utf8ToWide(broadcaster_id_);

    std::wstring headers;
    headers += L"Client-Id: " + Utf8ToWide(client_id_) + L"\r\n";
    headers += L"Authorization: Bearer " + Utf8ToWide(access_token_) + L"\r\n";

    HttpResult r = WinHttpRequest(L"GET", L"api.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, path, headers, "", true);
    if (r.status < 200 || r.status >= 300) {
        DebugLog(L"ResolveBroadcasterUserId failed HTTP " + std::to_wstring(r.status) + L" body=" + Utf8ToWide(r.body));
        return "";
    }

    try {
        json jr = json::parse(r.body);
        if (jr.contains("data") && jr["data"].is_array() && !jr["data"].empty()) {
            broadcaster_user_id_ = jr["data"][0].value("id", "");
        }
    } catch (...) {
        return "";
    }

    return broadcaster_user_id_;
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

    // Track subscription attempts for /api/twitch/eventsub/status
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        json attempt;
        attempt["type"] = type;
        attempt["version"] = version;
        attempt["status"] = r.status;
        attempt["ok"] = (r.status == 202 || (r.status >= 200 && r.status < 300));
        if (!r.body.empty()) attempt["body"] = r.body;
        subscriptions_.push_back(attempt);
        if (attempt["ok"].get<bool>()) {
            last_helix_ok_ms_ = NowMs();
        }
    }
    EmitStatus();
        true);

    if (r.status == 202 || (r.status >= 200 && r.status < 300)) {
        DebugLog(L"Subscribed: " + Utf8ToWide(type) + L" v" + Utf8ToWide(version));
        return true;
    }

    DebugLog(L"Subscribe failed for " + Utf8ToWide(type) + L" HTTP " + std::to_wstring(r.status) + L" body=" + Utf8ToWide(r.body));
    return false;
}

bool TwitchEventSubWsClient::SubscribeAll(const std::string& sessionId)
{
    std::string broadcasterUid = ResolveBroadcasterUserId();
    if (broadcasterUid.empty()) {
        DebugLog(L"SubscribeAll: missing broadcaster user id (check twitch_login, token, client-id)");
        return false;
    }

    // Follow (v2) requires broadcaster_user_id and moderator_user_id, and scope moderator:read:followers.
    // We set moderator_user_id to broadcaster ID; this works if the token is for the broadcaster (or a moderator),
    // and the token user_id matches the moderator_user_id value (per Twitch docs).
    // https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/#channelfollow
    bool okAny = false;
    okAny |= CreateSubscription(
        "channel.follow",
        "2",
        json({ {"broadcaster_user_id", broadcasterUid}, {"moderator_user_id", broadcasterUid} }).dump(),
        sessionId);

    // Subscriptions (v1) require broadcaster_user_id and scope channel:read:subscriptions.
    // https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/#channelsubscribe
    okAny |= CreateSubscription(
        "channel.subscribe",
        "1",
        json({ {"broadcaster_user_id", broadcasterUid} }).dump(),
        sessionId);

    // Gifted subscriptions (v1) require broadcaster_user_id and scope channel:read:subscriptions.
    // https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/#channelsubscriptiongift
    okAny |= CreateSubscription(
        "channel.subscription.gift",
        "1",
        json({ {"broadcaster_user_id", broadcasterUid} }).dump(),
        sessionId);

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

    // Event payload fields differ slightly between types; prefer user_name if present.
    if (subType == "channel.follow")
    {
        msg.user = ev.value("user_name", ev.value("user_login", ""));
        msg.message = "followed";
    }
    else if (subType == "channel.subscribe")
    {
        msg.user = ev.value("user_name", ev.value("user_login", ""));
        msg.message = "subscribed";
    }
    else if (subType == "channel.subscription.gift")
    {
        msg.user = ev.value("user_name", ev.value("user_login", ""));
        int count = ev.value("total", 1);
        msg.message = "gifted " + std::to_string(count) + " subs";
    }
    else
    {
        return;
    }

    msg.ts_ms = NowMs();

    if (on_event_) {
        json evOut;
        evOut["ts_ms"] = msg.ts_ms;
        evOut["platform"] = "twitch";
        evOut["type"] = subType;
        evOut["user"] = msg.user;
        evOut["message"] = msg.message;
        on_event_(evOut);
    }

    if (on_chat_event_)
        on_chat_event_(msg);
}
