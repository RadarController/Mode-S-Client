#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <functional>

class TwitchAuth {
public:
    using UiLogFn = void(*)(const std::wstring&);

    // Optional UI logger callback (keeps existing Visual Studio/OutputDebugString logging too).
    static void SetUiLogger(UiLogFn fn);

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

    // ---- Interactive OAuth (one-time login to obtain/replace refresh token) ----
    // Build the URL for the user to open in a browser.
    // The caller must provide the redirect_uri that Twitch is configured to send the user back to.
    // This function stores an internal anti-CSRF "state" value which is verified in HandleOAuthCallback.
    std::string BuildAuthorizeUrl(const std::string& redirect_uri, std::string* out_error = nullptr);

    // Handle the redirect callback query params ("code" and "state"), exchanging the code for tokens.
    // On success this updates in-memory tokens and persists them back to config.json.
    bool HandleOAuthCallback(const std::string& code,
        const std::string& state,
        const std::string& redirect_uri,
        std::string* out_error = nullptr);

    // Called after a successful refresh + validation (tokens in memory are updated).
    std::function<void(const std::string& access_token,
        const std::string& refresh_token,
        const std::string& login)> on_tokens_updated;

private:
    bool ValidateAndLogToken(const std::string& access_token, std::string* out_scope_joined);

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

    // Pending OAuth anti-CSRF state for interactive auth
    mutable std::mutex oauth_mu_;
    std::string pending_state_;

    // Worker
    std::atomic<bool> running_{ false };
    std::thread worker_;
};
