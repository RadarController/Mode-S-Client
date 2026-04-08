#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

class ChatAggregator;
class AppState;
class YouTubeAuth;

// Polls YouTube Live Chat for the channel handle (e.g. "@SomeChannel").
// Implementation: scrapes /@handle/live, then /live_chat, then polls youtubei live_chat/get_live_chat.
class YouTubeLiveChatService {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    YouTubeLiveChatService();
    ~YouTubeLiveChatService();

    bool start(const std::string& youtube_handle_or_channel,
        ChatAggregator& chat,
        LogFn log,
        AppState* state = nullptr);

    void SetReplyAuth(YouTubeAuth* auth);
    bool send_chat(const std::string& text, std::string* out_error = nullptr);

    void stop();
    bool running() const { return running_.load(); }

private:
    void worker(std::string handle, ChatAggregator* chat, AppState* state, LogFn log);

    std::atomic<bool> running_{ false };
    std::thread thread_;

    YouTubeAuth* reply_auth_ = nullptr;
    std::mutex reply_mu_;
    std::string cached_reply_live_chat_id_;
    std::uint64_t cached_reply_live_chat_id_ms_ = 0;
};