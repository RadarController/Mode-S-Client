#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

class ChatAggregator; // forward declaration

// Minimal Twitch IRC-over-WebSocket client for receiving chat messages.
class TwitchIrcWsClient {
public:
    using OnPrivMsg = std::function<void(const std::string& user, const std::string& message)>; // already sanitized

    TwitchIrcWsClient();
    ~TwitchIrcWsClient();

    bool start(const std::string& oauth_token_with_oauth_prefix,
        const std::string& nick,
        const std::string& channel, // without '#'
        OnPrivMsg cb);

    // NEW: convenience overload to push received messages straight into ChatAggregator
    bool start(const std::string& oauth_token_with_oauth_prefix,
        const std::string& nick,
        const std::string& channel, // without '#'
        ChatAggregator& chat);

    void stop();
    bool running() const { return m_running.load(); }

private:
    void worker(std::string oauth, std::string nick, std::string channel, OnPrivMsg cb);

    ChatAggregator* m_chat = nullptr; // optional sink for chat aggregation
    std::atomic<bool> m_running{ false };
    std::thread m_thread;
};