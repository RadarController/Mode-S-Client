#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <mutex>

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

    // Send a chat message to the currently-joined channel.
    // Returns false if not connected/authenticated.
    bool send_privmsg(const std::string& channel, const std::string& message);

    // Convenience overload: send to the last-joined channel.
    // Returns false if not connected/authenticated.
    bool send_privmsg(const std::string& message);

private:
    void worker(std::string oauth, std::string nick, std::string channel, OnPrivMsg cb);

    ChatAggregator* m_chat = nullptr; // optional sink for chat aggregation
    std::atomic<bool> m_running{ false };
    std::thread m_thread;

    // WebSocket handle (WinHTTP). Stored so we can send PRIVMSG.
    // Access guarded by m_ws_mtx.
    void* m_ws = nullptr;
    std::mutex m_ws_mtx;
    std::string m_channel;
};