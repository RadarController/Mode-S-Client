#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// YouTube / Google OAuth helper for Mode-S Client.
// Mirrors the TwitchAuth pattern: reads/writes config.json (CWD or exe dir), supports
// background refresh, and exposes interactive OAuth helpers for a local callback.
//
// This class ONLY handles auth/token lifecycle. Actual YouTube Data API calls (videos.update,
// liveBroadcasts.update, etc.) should be implemented elsewhere once auth is in place.
class YouTubeAuth {
public:
    using UiLogFn = void(*)(const std::wstring&);

    static void SetUiLogger(UiLogFn fn);

    struct TokenSnapshot {
        std::string access_token;
        std::string refresh_token;
        std::int64_t expires_at_unix = 0; // seconds since epoch (UTC)
        std::string token_type;
        std::string scope_joined; // for logging/diag
    };

    // Call once at app startup. Will load tokens from config.json, refresh if needed,
    // then start a background refresh loop.
    bool Start();

    // Call on shutdown.
    void Stop();

    // Thread-safe accessors
    std::optional<std::string> GetAccessToken() const;
    std::optional<TokenSnapshot> GetTokenSnapshot() const;

    // Forces immediate refresh attempt (useful for debug endpoints).
    bool RefreshNow(std::string* out_error = nullptr);

    // ---- Interactive OAuth (one-time login to obtain/replace refresh token) ----
    // Build the URL for the user to open in a browser.
    // If redirect_uri is empty, defaults to:
    //   http://localhost:17845/auth/youtube/callback
    //
    // NOTE: For Google, include "access_type=offline" and "prompt=consent" to ensure a refresh token is issued.
    std::string BuildAuthorizeUrl(const std::string& redirect_uri, std::string* out_error = nullptr);

    // Handle the redirect callback params ("code" and "state"), exchange code for tokens,
    // validate, then persist tokens back to config.json.
    bool HandleOAuthCallback(const std::string& code,
        const std::string& state,
        const std::string& redirect_uri,
        std::string* out_error = nullptr);

    // Called after a successful refresh + validation (tokens updated).
    std::function<void(const std::string& access_token,
        const std::string& refresh_token,
        const std::string& channel_id)> on_tokens_updated;

    // Minimal scope set for updating YouTube metadata.
    // (If you later need more, update these and re-auth.)
    static const char* RequiredScopeEncoded();
    static const char* RequiredScopeReadable();

private:
    bool ValidateAndLogToken(const std::string& access_token, std::string* out_scope_joined, std::string* out_channel_id);

    bool LoadFromConfig(std::string* out_error);
    bool SaveToConfig(const TokenSnapshot& snap, std::string* out_error);

    bool NeedsRefresh(std::int64_t now_unix) const;
    bool RefreshWithGoogle(std::string* out_error);

    static std::string HttpPostForm(const std::string& url,
        const std::string& form_body,
        const std::initializer_list<std::pair<std::string, std::string>>& headers,
        long* out_http_status,
        std::string* out_error);

    static std::string UrlEncode(const std::string& s);
    static std::string RandomHex(size_t bytes);

private:
    mutable std::mutex mu_;
    TokenSnapshot tokens_;
    std::string client_id_;
    std::string client_secret_;
    std::string channel_id_;

    std::atomic<bool> running_{ false };
    std::thread bg_;

    // OAuth anti-CSRF state
    mutable std::mutex oauth_mu_;
    std::string pending_state_;

    static UiLogFn ui_log_;
};
