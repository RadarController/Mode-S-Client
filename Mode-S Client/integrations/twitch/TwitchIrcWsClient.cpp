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
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

bool TwitchIrcWsClient::start(const std::string& oauth_token_with_oauth_prefix,
    const std::string& nick,
    const std::string& channel,
    OnPrivMsg cb) {
    if (m_running.load()) return false;
    if (oauth_token_with_oauth_prefix.empty() || nick.empty() || channel.empty()) return false;
    m_chat = nullptr;
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
    // Connect to wss://irc-ws.chat.twitch.tv:443
    HINTERNET hSession = WinHttpOpen(L"StreamHub/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { m_running.store(false); return; }

    HINTERNET hConnect = WinHttpConnect(hSession, L"irc-ws.chat.twitch.tv",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); m_running.store(false); return; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        m_running.store(false); return;
    }

    // Upgrade to WebSocket
    BOOL ok = WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    if (!ok) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        m_running.store(false); return;
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        m_running.store(false); return;
    }

    HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    WinHttpCloseHandle(hRequest);
    if (!hWebSocket) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        m_running.store(false); return;
    }

    auto sendLine = [&](const std::string& line) -> bool {
        std::string data = line + "\r\n";
        return WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            (PVOID)data.data(), (DWORD)data.size()) == NO_ERROR;
        };

    // Authenticate + request tags
    sendLine("CAP REQ :twitch.tv/tags twitch.tv/commands");
    // Normalize oauth token for IRC: Twitch requires "oauth:<token>"
    if (oauth.rfind("Bearer ", 0) == 0) {
        oauth = oauth.substr(7);
    }
    while (!oauth.empty() && (oauth.back() == '\r' || oauth.back() == '\n' || oauth.back() == ' ' || oauth.back() == '\t')) oauth.pop_back();
    size_t front = 0;
    while (front < oauth.size() && (oauth[front] == ' ' || oauth[front] == '\t')) front++;
    if (front) oauth = oauth.substr(front);
    if (!oauth.empty() && oauth.rfind("oauth:", 0) != 0) {
        oauth = "oauth:" + oauth;
    }

    sendLine("PASS " + oauth);
    sendLine("NICK " + nick);
    sendLine("JOIN #" + channel);

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

            // Keep debugger output if you like
            {
                std::wstring w = L"[TWITCH RAW] " + ToW(line);
                OutputDebugStringW(w.c_str());
            }

            if (line.rfind("PING", 0) == 0) {
                // Respond with PONG
                std::string payload = line.substr(4);
                sendLine("PONG" + payload);
                continue;
            }

            // Example:
            // @badge-info=;badges=;color=...;display-name=User;... :user!user@user.tmi.twitch.tv PRIVMSG #channel :hello
            std::string tagsPart;
            std::string rest = line;

            if (!rest.empty() && rest[0] == '@') {
                size_t sp = rest.find(' ');
                if (sp != std::string::npos) {
                    tagsPart = rest.substr(1, sp - 1);
                    rest = rest.substr(sp + 1);
                }
            }

            // Need PRIVMSG
            auto privPos = rest.find(" PRIVMSG ");
            if (privPos == std::string::npos) continue;

            // Find message after " :"
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
                // fallback: parse nick from prefix
                // :nick!...
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
                m.color = userColor; // may be empty
                m.ts_ms = NowMs();
                m_chat->Add(std::move(m));
            }
            if (cb) cb(user, msg);
        }

        if (pos > 0) recvBuf.erase(0, pos);
    }

    WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    WinHttpCloseHandle(hWebSocket);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    m_running.store(false);
}
