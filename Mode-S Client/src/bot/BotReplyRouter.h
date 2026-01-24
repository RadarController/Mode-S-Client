#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <algorithm>

struct BotReplyTarget {
    // Platform key (any case). Examples: "twitch", "youtube", "tiktok".
    std::string platform_lc;

    // Optional (useful for channel/room/thread targeting)
    // For Twitch IRC, this is typically the channel name without '#', e.g. "radarcontroller".
    std::string channel_id;
};

class BotReplyRouter {
public:
    using SendFn = std::function<bool(const BotReplyTarget& target, const std::string& text)>;
    using LogFn = std::function<void(const std::string&)>;

    // Optional: receive debug messages when a send fails.
    void SetLogger(LogFn fn) { logger_ = std::move(fn); }

    // Register a sender for a platform (key can be any case; stored lowercased).
    void Register(std::string platform_key, SendFn fn) {
        Normalize(platform_key);
        senders_[platform_key] = std::move(fn);
        Log("BotReplyRouter: registered sender for '" + platform_key + "'");
    }

    // Register an alias so different upstream platform labels resolve to the same sender.
    // Example: RegisterAlias("twitch_irc", "twitch");
    void RegisterAlias(std::string alias_key, std::string canonical_key) {
        Normalize(alias_key);
        Normalize(canonical_key);
        aliases_[alias_key] = canonical_key;
        Log("BotReplyRouter: registered alias '" + alias_key + "' -> '" + canonical_key + "'");
    }

    // Send a reply to the origin platform only.
    // Returns false if platform not registered or send failed.
    bool Send(const BotReplyTarget& target, const std::string& text) const {
        // Normalize platform key from target (fixes "Twitch" vs "twitch" issues)
        std::string key = ToLower(target.platform_lc);

        // Resolve alias if present
        auto ali = aliases_.find(key);
        if (ali != aliases_.end()) key = ali->second;

        auto it = senders_.find(key);
        if (it == senders_.end()) {
            Log("BotReplyRouter: no sender registered for platform '" + key +
                "' (original='" + target.platform_lc + "')");
            return false;
        }

        const bool ok = it->second(target, text);
        if (!ok) {
            Log("BotReplyRouter: sender for '" + key + "' reported failure. "
                "channel_id='" + target.channel_id + "', text_len=" + std::to_string(text.size()));
        }
        return ok;
    }

    static std::string ToLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        // trim whitespace (common source of key mismatches)
        while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        return s;
    }

private:
    static void Normalize(std::string& s) { s = ToLower(std::move(s)); }

    void Log(const std::string& msg) const {
        if (logger_) logger_(msg);
    }

    std::unordered_map<std::string, SendFn> senders_;
    std::unordered_map<std::string, std::string> aliases_;
    LogFn logger_;
};