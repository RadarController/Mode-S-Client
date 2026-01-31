#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class ChatAggregator; // forward declaration

// Minimal Twitch IRC-over-WebSocket client for receiving and sending chat messages.
class TwitchIrcWsClient {
public:
    using OnPrivMsg = std::function<void(const std::string& user, const std::string& message)>; // already sanitized

    TwitchIrcWsClient();
    ~TwitchIrcWsClient();

    // Optional: provide ChatAggregator sink for incoming PRIVMSG messages
    void SetChatAggregator(ChatAggregator* chat) { m_chat = chat; }
    void SetChatAggregator(ChatAggregator& chat) { m_chat = &chat; }

    // Start Twitch IRC with authenticated user credentials (required for sending messages)
    // Starts an authenticated IRC session and forwards PRIVMSG into the provided ChatAggregator.
    // access_token may be raw, "oauth:..." or "Bearer ..."; it will be normalized.
    // If you already called SetChatAggregator(), you may use the 3-arg overload.
    bool StartAuthenticated(const std::string& login,
                            const std::string& access_token,
                            const std::string& channel);

    bool StartAuthenticated(const std::string& login,
                            const std::string& access_token,
                            const std::string& channel,
                            ChatAggregator& chat);

    bool start(const std::string& oauth_token_with_oauth_prefix,
        const std::string& nick,
        const std::string& channel, // without '#'
        OnPrivMsg cb);

    // Convenience overload to push received messages straight into ChatAggregator
    bool start(const std::string& oauth_token_with_oauth_prefix,
        const std::string& nick,
        const std::string& channel, // without '#'
        ChatAggregator& chat);

    void stop();
    void Stop() { stop(); }

    bool running() const { return m_running.load(); }

    // ---------------------------------------------------------------------
    // Sending (PRIVMSG)
    // ---------------------------------------------------------------------
    // Send a message to the channel this client joined (m_channel).
    // Returns false if not connected/running or if WinHTTP send fails.
    bool SendPrivMsg(const std::string& message_utf8);

    // Send a message to a specific channel (without '#').
    bool SendPrivMsgTo(const std::string& channel, const std::string& message_utf8);

private:
    void worker(std::string oauth, std::string nick, std::string channel, OnPrivMsg cb);

    // Internal raw send (expects a full IRC line WITHOUT trailing CRLF).
    bool SendRawLine(const std::string& line_no_crlf);

    ChatAggregator* m_chat = nullptr; // optional sink for chat aggregation
    std::string m_login;
    std::string m_access_token;
    std::string m_channel; // joined channel, without '#'
    std::string m_nick;

    // Guards start/stop/join so they are safe if called concurrently (e.g., from HTTP worker + UI).
    std::mutex m_lifecycle_mu;

    std::atomic<bool> m_running{ false };
    std::thread m_thread;

    // WebSocket handle (WinHTTP HINTERNET) stored as void* to avoid WinHTTP headers in this header.
    mutable std::mutex m_ws_mu;
    void* m_ws = nullptr;
};
