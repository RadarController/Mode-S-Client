#include "TwitchIrcWsClient.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>
#include "chat/ChatAggregator.h"
// ChatMessage is defined in AppState.h (shared between platform adapters).
// Depending on include order/build layout, ChatAggregator.h may not bring it in.
#include "AppState.h"
#pragma comment(lib, "ws2_32.lib")
// We use WinHTTP WebSocket (built into Windows) to avoid extra libs.
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
static uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}
static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}
static std::string TrimCRLF(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}
static std::unordered_map<std::string, std::string> ParseTags(const std::string& tagsPart) {
    // tagsPart: "badge-info=;badges=;client-nonce=...;display-name=Foo;..."
    std::unordered_map<std::string, std::string> tags;
    size_t start = 0;
    while (start < tagsPart.size()) {
        size_t semi = tagsPart.find(';', start);
        std::string kv = (semi == std::string::npos) ? tagsPart.substr(start) : tagsPart.substr(start, semi - start);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) tags[kv.substr(0, eq)] = kv.substr(eq + 1);
        else tags[kv] = "";
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return tags;
}
TwitchIrcWsClient::TwitchIrcWsClient() {}
TwitchIrcWsClient::~TwitchIrcWsClient() { stop(); }
void TwitchIrcWsClient::stop() {
    // stop() can be called from multiple threads (UI, HTTP routes, token refresh).
    // Make it idempotent and avoid std::terminate from double-join or self-join.
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lk(m_lifecycle_mu);
        m_running.store(false);
        if (m_thread.joinable()) {
            to_join = std::move(m_thread);
        }
    }
    if (to_join.joinable()) {
        if (to_join.get_id() == std::this_thread::get_id()) {
            // Joining yourself throws; detach as a safe escape hatch.
            to_join.detach();
        } else {
            to_join.join();
        }
    }
}
// -----------------------------------------------------------------------------
// Sending helpers
// -----------------------------------------------------------------------------
static std::string SanitizeIrcText(std::string s) {
    // Prevent CRLF injection and keep messages on one IRC line.
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
    // Twitch chat messages have limits; keep conservative.
    const size_t kMax = 450;
    if (s.size() > kMax) s.resize(kMax);
    return s;
}
bool TwitchIrcWsClient::SendRawLine(const std::string& line_no_crlf) {
    std::lock_guard<std::mutex> lk(m_ws_mu);
    if (!m_running.load() || !m_ws) return false;
    HINTERNET ws = (HINTERNET)m_ws;
    std::string data = line_no_crlf;
    data += "\r\n";
    const DWORD rc = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         (PVOID)data.data(), (DWORD)data.size());
    return rc == NO_ERROR;
}
bool TwitchIrcWsClient::SendPrivMsg(const std::string& message_utf8) {
    if (m_channel.empty()) return false;
    return SendPrivMsgTo(m_channel, message_utf8);
}
bool TwitchIrcWsClient::SendPrivMsgTo(const std::string& channel, const std::string& message_utf8) {
    if (channel.empty()) return false;
    const std::string msg = SanitizeIrcText(message_utf8);
    if (msg.empty()) return false;
    return SendRawLine("PRIVMSG #" + channel + " :" + msg);
}
static std::string NormalizeRawAccessToken(std::string tok) {
    auto trim_ws = [](std::string s) -> std::string {
        while (!s.empty() && (s.front()==' ' || s.front()=='\t' || s.front()=='\n' || s.front()=='\r')) s.erase(s.begin());
        while (!s.empty() && (s.back()==' ' || s.back()=='\t' || s.back()=='\n' || s.back()=='\r')) s.pop_back();
        return s;
    };
    tok = trim_ws(tok);
    if (tok.rfind("oauth:", 0) == 0) tok = tok.substr(6);
    if (tok.rfind("Bearer ", 0) == 0) tok = trim_ws(tok.substr(7));
    return tok;
}
bool TwitchIrcWsClient::StartAuthenticated(const std::string& login,
                                          const std::string& access_token,
                                          const std::string& channel)
{
    // Use previously configured aggregator (SetChatAggregator) if present.
    if (m_chat) {
        return StartAuthenticated(login, access_token, channel, *m_chat);
    }
    // Fall back to starting without aggregation.
    std::string ch = channel;
    if (login.empty() || access_token.empty() || ch.empty()) {
        OutputDebugStringA("TWITCH IRC: missing login/token/channel, refusing to start\n");
        return false;
    }
    m_login = login;
    m_access_token = access_token;
    m_channel = ch;
    m_nick = login;
    const std::string raw = NormalizeRawAccessToken(access_token);
    return start(std::string("oauth:") + raw, login, ch,
                 [](const std::string&, const std::string&) {
                     // No sink configured.
                 });
}
bool TwitchIrcWsClient::StartAuthenticated(const std::string& login,
                                          const std::string& access_token,
                                          const std::string& channel,
                                          ChatAggregator& chat)
{
    std::string ch = channel;
    if (login.empty() || access_token.empty() || ch.empty()) {
        OutputDebugStringA("TWITCH IRC: missing login/token/channel, refusing to start\n");
        return false;
    }
    m_login = login;
    m_access_token = access_token;
    m_channel = ch;
    m_nick = login;
    const std::string raw = NormalizeRawAccessToken(access_token);
    return start(std::string("oauth:") + raw, login, ch, chat);
}
bool TwitchIrcWsClient::start(const std::string& oauth_token_with_oauth_prefix,
    const std::string& nick,
    const std::string& channel,
    OnPrivMsg cb) {
    std::lock_guard<std::mutex> lk(m_lifecycle_mu);
    if (m_running.load()) return false;
    if (oauth_token_with_oauth_prefix.empty() || nick.empty() || channel.empty()) return false;
    m_running.store(true);
    m_thread = std::thread(&TwitchIrcWsClient::worker, this,
        oauth_token_with_oauth_prefix, nick, channel, std::move(cb));
    return true;
}
bool TwitchIrcWsClient::start(const std::string& oauth_token_with_oauth_prefix,
    const std::string& nick,
    const std::string& channel,
    ChatAggregator& chat) {
    // Wrap the callback API and push incoming messages into ChatAggregator
    m_chat = &chat;
    {
        std::wstringstream ss;
        ss << L"TWITCH: aggregator ptr=0x" << std::hex << (uintptr_t)m_chat << L"\n";
        OutputDebugStringW(ss.str().c_str());
    }
    return start(oauth_token_with_oauth_prefix, nick, channel,
        [&chat](const std::string& user, const std::string& message) {
            ChatMessage m{};
            m.platform = "twitch";
            m.user = user;
            m.message = message;
            m.ts_ms = NowMs();
            chat.Add(std::move(m));
        });
}
void TwitchIrcWsClient::worker(std::string oauth, std::string nick, std::string channel, OnPrivMsg cb) {
    // Auto-reconnect loop: if the socket drops (e.g. network flap), reconnect until stop() is called.
    int attempt = 0;
    auto logw = [&](const wchar_t* s) {
        if (!s) return;
        OutputDebugStringW(s);
        OutputDebugStringW(L"\n");
    };
    // Normalize token once
    if (oauth.rfind("Bearer ", 0) == 0) oauth = oauth.substr(7);
    while (!oauth.empty() && (oauth.back() == '\r' || oauth.back() == '\n' || oauth.back() == ' ' || oauth.back() == '\t')) oauth.pop_back();
    size_t front = 0;
    while (front < oauth.size() && (oauth[front] == ' ' || oauth[front] == '\t')) front++;
    if (front) oauth = oauth.substr(front);
    if (!oauth.empty() && oauth.rfind("oauth:", 0) != 0) oauth = "oauth:" + oauth;
    while (m_running.load()) {
        bool connected = false;
        // Handles are per-attempt so we can cleanly retry.
        HINTERNET hSession = nullptr;
        HINTERNET hConnect = nullptr;
        HINTERNET hRequest = nullptr;
        HINTERNET hWebSocket = nullptr;
        auto cleanup = [&]() {
            {
                std::lock_guard<std::mutex> lk(m_ws_mu);
                if (m_ws == (void*)hWebSocket) m_ws = nullptr;
            }
            if (hWebSocket) {
                WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
                WinHttpCloseHandle(hWebSocket);
                hWebSocket = nullptr;
            }
            if (hRequest) { WinHttpCloseHandle(hRequest); hRequest = nullptr; }
            if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
            if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
        };
        // Connect to wss://irc-ws.chat.twitch.tv:443
        hSession = WinHttpOpen(L"StreamHub/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            logw(L"[TwitchIRC] WinHttpOpen failed");
            cleanup();
        }
        else {
            hConnect = WinHttpConnect(hSession, L"irc-ws.chat.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!hConnect) {
                logw(L"[TwitchIRC] WinHttpConnect failed");
                cleanup();
            }
            else {
                hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/",
                    nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                if (!hRequest) {
                    logw(L"[TwitchIRC] WinHttpOpenRequest failed");
                    cleanup();
                }
                else {
                    BOOL ok = WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
                    if (!ok) {
                        logw(L"[TwitchIRC] WinHttpSetOption(UPGRADE) failed");
                        cleanup();
                    }
                    else if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
                        !WinHttpReceiveResponse(hRequest, nullptr)) {
                        logw(L"[TwitchIRC] WinHttpSendRequest/ReceiveResponse failed");
                        cleanup();
                    }
                    else {
                        hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
                        WinHttpCloseHandle(hRequest);
                        hRequest = nullptr;
                        if (!hWebSocket) {
                            logw(L"[TwitchIRC] WebSocketCompleteUpgrade failed");
                            cleanup();
                        }
                        else {
                            {
                                std::lock_guard<std::mutex> lk(m_ws_mu);
                                m_ws = (void*)hWebSocket;
                            }
                            auto sendLine = [&](const std::string& line) -> bool {
                                std::string data = line + "\n";
                                return WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    (PVOID)data.data(), (DWORD)data.size()) == NO_ERROR;
                            };
                            // Authenticate + request tags/commands
                            sendLine("CAP REQ :twitch.tv/tags twitch.tv/commands");
                            sendLine("PASS " + oauth);
                            sendLine("NICK " + nick);
                            sendLine("JOIN #" + channel);
                            connected = true;
                            attempt = 0;
                            logw(L"[TwitchIRC] connected");
                            std::string recvBuf;
                            recvBuf.reserve(8192);
                            while (m_running.load()) {
                                BYTE buffer[4096];
                                DWORD bytesRead = 0;
                                WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
                                DWORD r = WinHttpWebSocketReceive(hWebSocket, buffer, sizeof(buffer), &bytesRead, &bufferType);
                                if (r != NO_ERROR) break;
                                if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                                recvBuf.append((const char*)buffer, (size_t)bytesRead);
                                // Process complete IRC lines
                                size_t pos = 0;
                                while (true) {
                                    size_t eol = recvBuf.find("\n", pos);
                                    if (eol == std::string::npos) break;
                                    std::string line = TrimCRLF(recvBuf.substr(pos, eol - pos + 1));
                                    pos = eol + 1;
                                    {
                                        std::wstring w = L"[TWITCH RAW] " + ToW(line);
                                        OutputDebugStringW(w.c_str());
                                    }
                                    if (line.rfind("PING", 0) == 0) {
                                        std::string payload = line.substr(4);
                                        sendLine("PONG" + payload);
                                        continue;
                                    }
                                    std::string tagsPart;
                                    std::string rest = line;
                                    if (!rest.empty() && rest[0] == '@') {
                                        size_t sp = rest.find(' ');
                                        if (sp != std::string::npos) {
                                            tagsPart = rest.substr(1, sp - 1);
                                            rest = rest.substr(sp + 1);
                                        }
                                    }
                                    auto privPos = rest.find(" PRIVMSG ");
                                    if (privPos == std::string::npos) continue;
                                    size_t msgPos = rest.find(" :");
                                    if (msgPos == std::string::npos) continue;
                                    std::string msg = rest.substr(msgPos + 2);
                                    std::string user = "unknown";
                                    std::string userColor;
                                    if (!tagsPart.empty()) {
                                        auto tags = ParseTags(tagsPart);
                                        auto it = tags.find("display-name");
                                        if (it != tags.end() && !it->second.empty()) user = it->second;
                                        auto itc = tags.find("color");
                                        if (itc != tags.end() && !itc->second.empty()) userColor = itc->second;
                                    }
                                    else {
                                        if (!rest.empty() && rest[0] == ':') {
                                            size_t bang = rest.find('!');
                                            if (bang != std::string::npos) user = rest.substr(1, bang - 1);
                                        }
                                    }
                                    if (m_chat) {
                                        ChatMessage m{};
                                        m.platform = "twitch";
                                        m.user = user;
                                        m.message = msg;
                                        m.color = userColor;
                                        m.ts_ms = NowMs();
                                        m_chat->Add(std::move(m));
                                    }
                                    if (cb) cb(user, msg);
                                }
                                if (pos > 0) recvBuf.erase(0, pos);
                            }
                            // Fall-through to cleanup + retry logic.
                            cleanup();
                        }
                    }
                }
            }
        }
        if (!m_running.load()) break;
        if (connected) {
            logw(L"[TwitchIRC] disconnected (will retry)");
        }
        // Exponential backoff, capped at ~30s, with small jitter.
        if (attempt < 8) attempt++;
        DWORD base = 500;
        DWORD delay = base;
        for (int i = 0; i < attempt; ++i) {
            if (delay > 30000 / 2) { delay = 30000; break; }
            delay *= 2;
        }
        if (delay > 30000) delay = 30000;
        DWORD jitter = (DWORD)(GetTickCount() % 250);
        Sleep(delay + jitter);
    }
    m_running.store(false);
}