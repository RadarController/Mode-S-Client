#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class TwitchAuth {
public:
    struct TokenSnapshot {
        std::string access_token;
        std::string refresh_token;
        std::int64_t expires_at_unix = 0; // seconds since epoch (UTC)
        std::string token_type;
        std::string scope_joined; // for logging/diag
    };

    // Call once at app startup.
    // Attempts an initial refresh if needed, then starts a background refresh loop.
    bool Start();

    // Call on shutdown.
    void Stop();

    // Thread-safe: returns current access token if present and not expired.
    std::optional<std::string> GetAccessToken() const;

    // Thread-safe: full token snapshot (useful for /diag)
    std::optional<TokenSnapshot> GetTokenSnapshot() const;

    // Forces an immediate refresh attempt (optional to call from UI/debug endpoints).
    bool RefreshNow(std::string* out_error = nullptr);

private:
    bool LoadFromConfig(std::string* out_error);
    bool SaveToConfig(const TokenSnapshot& snap, std::string* out_error);

    bool NeedsRefresh(std::int64_t now_unix) const;
    bool RefreshWithTwitch(std::string* out_error);

    static std::string HttpPostForm(const std::string& url,
        const std::string& form_body,
        const std::string& content_type,
        long* out_http_status,
        std::string* out_error);

    static std::string UrlEncode(const std::string& s);
    static std::int64_t NowUnixSeconds();

private:
    // Config fields (loaded from config.json)
    std::string client_id_;
    std::string client_secret_;
    std::string refresh_token_cfg_;

    // Current token snapshot
    mutable std::mutex mu_;
    std::optional<TokenSnapshot> current_;

    // Worker
    std::atomic<bool> running_{ false };
    std::thread worker_;
};