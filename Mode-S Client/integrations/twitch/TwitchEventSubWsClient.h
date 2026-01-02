#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>

struct ChatMessage;

class TwitchEventSubWsClient
{
public:
    using ChatCallback = std::function<void(const ChatMessage&)>;

    TwitchEventSubWsClient();
    ~TwitchEventSubWsClient();

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

private:
    std::string client_id_;
    std::string access_token_;
    std::string broadcaster_id_;

    ChatCallback on_chat_event_;

    std::thread worker_;
    std::atomic<bool> running_{ false };
};
