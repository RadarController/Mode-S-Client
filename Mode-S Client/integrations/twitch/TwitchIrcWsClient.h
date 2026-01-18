#pragma once
#include <atomic>
#include <functional>
#include <mutex>
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

    // Send a PRIVMSG to the currently-joined channel.
    // Returns false if not connected/ready.
    bool send_privmsg(const std::string& message);

private:
    void worker(std::string oauth, std::string nick, std::string channel, OnPrivMsg cb);

    // WinHTTP WebSocket handle (owned by worker thread); protected by m_ws_mu.
    // Use void* here to avoid including winhttp.h in this header.
    using WsHandle = void*;
    bool send_raw_line_locked(const std::string& line);

    ChatAggregator* m_chat = nullptr; // optional sink for chat aggregation
    std::atomic<bool> m_running{ false };
    std::thread m_thread;

    std::mutex m_ws_mu;
    WsHandle m_ws = nullptr;
    std::string m_channel; // without '#'
    std::string m_nick;
};