#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <algorithm>

struct BotReplyTarget {
    // Lowercase platform key: "twitch", "youtube", "tiktok"
    std::string platform_lc;

    // Optional (leave empty for now; useful later for channel/room/thread targeting)
    std::string channel_id;
};

class BotReplyRouter {
public:
    using SendFn = std::function<bool(const BotReplyTarget& target, const std::string& text)>;

    // Register a sender for a platform (key should be lowercase).
    void Register(std::string platform_lc, SendFn fn) {
        Normalize(platform_lc);
        senders_[platform_lc] = std::move(fn);
    }

    // Send a reply to the origin platform only.
    // Returns false if platform not registered or send failed.
    bool Send(const BotReplyTarget& target, const std::string& text) const {
        auto it = senders_.find(target.platform_lc);
        if (it == senders_.end()) return false;
        return it->second(target, text);
    }

    static std::string ToLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

private:
    static void Normalize(std::string& s) { s = ToLower(std::move(s)); }

    std::unordered_map<std::string, SendFn> senders_;
};
