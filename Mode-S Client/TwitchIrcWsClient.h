#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

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

    void stop();
    bool running() const { return m_running.load(); }

private:
    void worker(std::string oauth, std::string nick, std::string channel, OnPrivMsg cb);

    std::atomic<bool> m_running{ false };
    std::thread m_thread;
};
