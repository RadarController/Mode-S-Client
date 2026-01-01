#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

class ChatAggregator;

// Polls YouTube Live Chat for the channel handle (e.g. "@SomeChannel").
// Implementation: scrapes /@handle/live, then /live_chat, then polls youtubei live_chat/get_live_chat.
class YouTubeLiveChatService {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    YouTubeLiveChatService();
    ~YouTubeLiveChatService();

    bool start(const std::string& youtube_handle_or_channel,
        ChatAggregator& chat,
        LogFn log);

    void stop();
    bool running() const { return running_.load(); }

private:
    void worker(std::string handle, ChatAggregator* chat, LogFn log);

    std::atomic<bool> running_{ false };
    std::thread thread_;
};