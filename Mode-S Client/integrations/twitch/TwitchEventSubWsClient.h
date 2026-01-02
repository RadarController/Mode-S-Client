#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>

struct ChatMessage;

// Lightweight Twitch EventSub WebSocket client (WinHTTP).
// Connects to EventSub WS, receives session_welcome, then creates Helix subscriptions
// bound to the WebSocket session (transport.method="websocket").
class TwitchEventSubWsClient
{
public:
    using ChatCallback = std::function<void(const ChatMessage&)>;

    TwitchEventSubWsClient();
    ~TwitchEventSubWsClient();

    // broadcasterId: treated as broadcaster login (e.g. "mychannel") or numeric user id.
    // The client will resolve logins to broadcaster_user_id via Helix /helix/users?login=...
    void Start(
        const std::string& clientId,
        const std::string& userAccessToken,
        const std::string& broadcasterId,
        ChatCallback onChatEvent);

    void Stop();

private:
    void Run();
    void ReceiveLoop(void* hWebSocket);
    void HandleMessage(const std::string& payload);
    void HandleNotification(const void* payload);

    // Helix helpers
    std::string ResolveBroadcasterUserId();
    bool SubscribeAll(const std::string& sessionId);
    bool CreateSubscription(const std::string& type,
                            const std::string& version,
                            const std::string& conditionJson,
                            const std::string& sessionId);

    // WebSocket control
    void RequestReconnect(const std::wstring& wssUrl);

private:
    std::string client_id_;
    std::string access_token_;
    std::string broadcaster_id_;      // login or numeric id (we resolve to user id)
    std::string broadcaster_user_id_; // numeric id

    ChatCallback on_chat_event_;

    std::thread worker_;
    std::atomic<bool> running_{ false };

    // WebSocket handle so Stop() can unblock WinHttpWebSocketReceive.
    std::mutex ws_mu_;
    void* ws_handle_{ nullptr };

    // reconnect target (host/path) set by session_reconnect message
    std::mutex reconnect_mu_;
    std::wstring ws_host_{ L"eventsub.wss.twitch.tv" };
    std::wstring ws_path_{ L"/ws" };
    bool reconnect_requested_{ false };
};
