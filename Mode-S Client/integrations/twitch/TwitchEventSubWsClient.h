#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include "json.hpp"

struct ChatMessage;

// Lightweight Twitch EventSub WebSocket client (WinHTTP).
// Connects to EventSub WS, receives session_welcome, then creates Helix subscriptions
// bound to the WebSocket session (transport.method="websocket").
class TwitchEventSubWsClient
{
public:
    using ChatCallback = std::function<void(const ChatMessage&)>;
    using JsonCallback = std::function<void(const nlohmann::json&)>;

    TwitchEventSubWsClient();
    ~TwitchEventSubWsClient();

    // broadcasterId: treated as broadcaster login (e.g. "mychannel") or numeric user id.
    // The client will resolve logins to broadcaster_user_id via Helix /helix/users?login=...
    void Start(
        const std::string& clientId,
        const std::string& userAccessToken,
        const std::string& broadcasterId,
        ChatCallback onChatEvent,
        JsonCallback onEvent = nullptr,
        JsonCallback onStatus = nullptr);

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

    // Status helpers
    void SetWsState(const std::string& s);
    void SetLastError(const std::string& e);
    void EmitStatus(bool helixOkTick = false);

private:
    std::string client_id_;
    std::string access_token_;
    std::string broadcaster_id_;      // login or numeric id (we resolve to user id)
    std::string broadcaster_user_id_; // numeric id

    ChatCallback on_chat_event_;
    JsonCallback on_event_;
    JsonCallback on_status_;

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

    // Status fields (mirrored to /api/twitch/eventsub/status via AppState)
    std::mutex status_mu_;
    std::string ws_state_ = "stopped";
    bool connected_ = false;
    bool subscribed_ = false;
    std::string session_id_;
    std::int64_t last_ws_message_ms_ = 0;
    std::int64_t last_keepalive_ms_ = 0;
    std::int64_t last_helix_ok_ms_ = 0;
    std::string last_error_;
    nlohmann::json subscriptions_ = nlohmann::json::array();
};
