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

    // Optional: provide ChatAggregator sink for incoming PRIVMSG messages
    void SetChatAggregator(ChatAggregator* chat) { m_chat = chat; }
    void SetChatAggregator(ChatAggregator& chat) { m_chat = &chat; }

    

    // Start Twitch IRC with authenticated user credentials (required for sending messages)
    bool StartAuthenticated(const std::string& login,
                            const std::string& access_token_raw,
                            const std::string& channel);
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
    void Stop() { stop(); }
bool running() const { return m_running.load(); }

private:
    void worker(std::string oauth, std::string nick, std::string channel, OnPrivMsg cb);

    ChatAggregator* m_chat = nullptr; // optional sink for chat aggregation
    std::string m_login;
    std::string m_access_token;
    std::string m_channel;
    std::string m_nick;
    std::atomic<bool> m_running{ false };
    std::thread m_thread;
};