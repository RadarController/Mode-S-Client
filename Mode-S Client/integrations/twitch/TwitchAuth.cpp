#include "TwitchAuth.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

// Hardcoded scope list (already URL-encoded).
// NOTE: Keep this encoded string as-is so we don't double-encode it.
static const char* kTwitchScopeEncoded =
"moderator%3Aread%3Afollowers+"
"channel%3Aread%3Ahype_train+"
"channel%3Aread%3Aredemptions+"
"channel%3Aread%3Asubscriptions";

// Human-readable equivalent for diagnostics/logging.
static const char* kTwitchScopeReadable =
"moderator:read:followers "
"channel:read:hype_train "
"channel:read:redemptions "
"channel:read:subscriptions";

namespace {
    // ---- config path (best-effort search; align with your project if you already have a shared helper) ----
    std::filesystem::path FindConfigPath() {
        std::vector<std::filesystem::path> candidates;

        // current working dir
        try { candidates.push_back(std::filesystem::current_path() / "config.json"); }
        catch (...) {}

#ifdef _WIN32
        // alongside executable (best effort)
        wchar_t buf[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len > 0) {
            std::filesystem::path exe = std::filesystem::path(buf).parent_path();
            candidates.push_back(exe / "config.json");
        }
#endif

        // common dev layouts
        candidates.push_back(std::filesystem::path("..") / "config.json");
        candidates.push_back(std::filesystem::path("..") / ".." / "config.json");

        for (const auto& p : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(p, ec) && !ec) return p;
        }
        return std::filesystem::path("config.json");
    }

    bool ReadJsonFile(const std::filesystem::path& p, json* out, std::string* out_error) {
        std::ifstream f(p, std::ios::in | std::ios::binary);
        if (!f) {
            if (out_error) *out_error = "Failed to open config: " + p.string();
            return false;
        }
        try {
            f >> *out;
            return true;
        }
        catch (const std::exception& e) {
            if (out_error) *out_error = std::string("Failed to parse config JSON: ") + e.what();
            return false;
        }
    }

    bool WriteJsonFileAtomic(const std::filesystem::path& p, const json& j, std::string* out_error) {
        std::error_code ec;
        if (!p.parent_path().empty()) std::filesystem::create_directories(p.parent_path(), ec);

        auto tmp = p;
        tmp += ".tmp";

        {
            std::ofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!f) {
                if (out_error) *out_error = "Failed to open temp config for write: " + tmp.string();
                return false;
            }
            f << j.dump(2);
            f.flush();
            if (!f) {
                if (out_error) *out_error = "Failed while writing temp config: " + tmp.string();
                return false;
            }
        }

        std::filesystem::rename(tmp, p, ec);
        if (ec) {
            // Windows rename over existing can fail; fallback: remove then rename
            std::filesystem::remove(p, ec);
            ec.clear();
            std::filesystem::rename(tmp, p, ec);
        }
        if (ec) {
            if (out_error) *out_error = "Failed to replace config atomically: " + ec.message();
            return false;
        }
        return true;
    }
} // namespace

std::int64_t TwitchAuth::NowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string TwitchAuth::UrlEncode(const std::string& s) {
    std::ostringstream oss;
    oss.fill('0');
    oss << std::hex << std::uppercase;

    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        }
        else if (c == ' ') {
            oss << '+';
        }
        else {
            oss << '%' << std::setw(2) << int(c);
        }
    }
    return oss.str();
}

std::string TwitchAuth::HttpPostForm(const std::string& url,
                                     const std::string& form_body,
                                     const std::string& content_type,
                                     long* out_http_status,
                                     std::string* out_error) {
    // Use cpp-httplib (already vendored in external/) instead of libcurl.
    // Supports HTTPS via SSLClient when CPPHTTPLIB_OPENSSL_SUPPORT is enabled (as elsewhere in this project).

    // Minimal URL parse (we only call this with https://id.twitch.tv/oauth2/token today).
    std::string scheme, host, path;
    int port = 0;

    if (url.rfind("https://", 0) == 0) {
        scheme = "https";
        port = 443;
        auto rest = url.substr(std::string("https://").size());
        auto slash = rest.find('/');
        host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    } else if (url.rfind("http://", 0) == 0) {
        scheme = "http";
        port = 80;
        auto rest = url.substr(std::string("http://").size());
        auto slash = rest.find('/');
        host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    } else {
        if (out_error) *out_error = "Unsupported URL (expected http/https): " + url;
        return {};
    }

    // If host contains an explicit :port, split it.
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        try {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        } catch (...) {
            if (out_error) *out_error = "Invalid port in URL: " + url;
            return {};
        }
    }

    httplib::Headers headers = { { "Content-Type", content_type } };

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (scheme == "https") {
        httplib::SSLClient cli(host.c_str(), port);
        cli.set_follow_location(true);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(10);
        cli.set_write_timeout(10);

        auto res = cli.Post(path.c_str(), headers, form_body, content_type.c_str());
        if (!res) {
            if (out_error) *out_error = "Failed to connect to " + host;
            return {};
        }
        if (out_http_status) *out_http_status = res->status;
        return res->body;
    }
#endif

    // HTTP fallback (or HTTPS without OpenSSL support).
    if (scheme == "https") {
        if (out_error) *out_error = "HTTPS requested but CPPHTTPLIB_OPENSSL_SUPPORT is not enabled";
        return {};
    }

    httplib::Client cli(host.c_str(), port);
    cli.set_follow_location(true);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);
    cli.set_write_timeout(10);

    auto res = cli.Post(path.c_str(), headers, form_body, content_type.c_str());
    if (!res) {
        if (out_error) *out_error = "Failed to connect to " + host;
        return {};
    }
    if (out_http_status) *out_http_status = res->status;
    return res->body;
}

bool TwitchAuth::LoadFromConfig(std::string* out_error) {
    auto path = FindConfigPath();
    json j;
    if (!ReadJsonFile(path, &j, out_error)) return false;

    // Matches your sample config:
    // twitch_client_id (top-level)
    // twitch_client_secret (top-level)
    // twitch.user_access_token / twitch.user_refresh_token (nested)
    client_id_ = j.value("twitch_client_id", "");
    client_secret_ = j.value("twitch_client_secret", "");

    if (!j.contains("twitch") || !j["twitch"].is_object()) {
        if (out_error) *out_error = "Missing 'twitch' object in config.json";
        return false;
    }

    const auto& tj = j["twitch"];
    std::string access = tj.value("user_access_token", "");
    std::string refresh = tj.value("user_refresh_token", "");

    refresh_token_cfg_ = refresh;

    if (client_id_.empty() || client_secret_.empty() || refresh_token_cfg_.empty()) {
        if (out_error) {
            *out_error =
                "Missing twitch_client_id / twitch_client_secret / twitch.user_refresh_token in config.json. "
                "Silent refresh requires a stored refresh token (one-time login).";
        }
        return false;
    }

    // Optional: if you ever decide to persist expiry, you can add a field later.
    // For now, we will always treat missing expiry as “needs refresh”.
    TokenSnapshot snap;
    snap.access_token = access;
    snap.refresh_token = refresh_token_cfg_;
    snap.expires_at_unix = 0;
    snap.token_type = "";
    snap.scope_joined = "";

    {
        std::lock_guard<std::mutex> lock(mu_);
        // Only store if access token exists; expiry is unknown so NeedsRefresh() will refresh anyway.
        if (!snap.access_token.empty()) current_ = snap;
    }

    return true;
}

bool TwitchAuth::SaveToConfig(const TokenSnapshot& snap, std::string* out_error) {
    auto path = FindConfigPath();
    json j;
    if (!ReadJsonFile(path, &j, out_error)) return false;

    // Ensure twitch object exists
    if (!j.contains("twitch") || !j["twitch"].is_object()) {
        j["twitch"] = json::object();
    }

    // Matches your sample config structure
    j["twitch"]["user_access_token"] = snap.access_token;
    j["twitch"]["user_refresh_token"] = snap.refresh_token;

    // NOTE: your sample config does not currently include expiry fields.
    // We intentionally do not add new fields here to keep config stable.
    // (If you want expiry persisted, we can add twitch.token_expires_at_unix etc. later.)

    return WriteJsonFileAtomic(path, j, out_error);
}

bool TwitchAuth::NeedsRefresh(std::int64_t now_unix) const {
    (void)now_unix;

    std::lock_guard<std::mutex> lock(mu_);
    if (!current_.has_value()) return true;

    const auto& s = *current_;
    if (s.access_token.empty()) return true;

    // If we don't have expiry tracking in config, refresh on startup and then on a fixed cadence.
    // This returns true when expiry is unknown OR close.
    if (s.expires_at_unix <= 0) return true;

    return (s.expires_at_unix - NowUnixSeconds()) <= 300;
}

bool TwitchAuth::RefreshWithTwitch(std::string* out_error) {
    // https://id.twitch.tv/oauth2/token
    const std::string url = "https://id.twitch.tv/oauth2/token";
    const std::string body =
        "grant_type=refresh_token"
        "&refresh_token=" + UrlEncode(refresh_token_cfg_) +
        "&client_id=" + UrlEncode(client_id_) +
        "&client_secret=" + UrlEncode(client_secret_) +
        "&scope=" + std::string(kTwitchScopeEncoded);

    long http = 0;
    std::string http_err;
    auto resp = HttpPostForm(url, body, "application/x-www-form-urlencoded", &http, &http_err);
    if (resp.empty()) {
        if (out_error) *out_error = http_err.empty() ? "Empty response from Twitch token endpoint" : http_err;
        return false;
    }
    if (http < 200 || http >= 300) {
        if (out_error) *out_error = "Twitch token endpoint returned HTTP " + std::to_string(http) + " body=" + resp;
        return false;
    }

    TokenSnapshot snap;
    try {
        auto jr = json::parse(resp);

        snap.access_token = jr.value("access_token", "");
        snap.refresh_token = jr.value("refresh_token", "");
        snap.token_type = jr.value("token_type", "");
        int expires_in = jr.value("expires_in", 0);

        // Hardcode expected scopes for now (even if Twitch returns a scope array).
        snap.scope_joined = kTwitchScopeReadable;

        if (snap.access_token.empty() || snap.refresh_token.empty() || expires_in <= 0) {
            if (out_error) *out_error = "Token refresh response missing fields: " + resp;
            return false;
        }

        const auto now = NowUnixSeconds();
        snap.expires_at_unix = now + expires_in;

    }
    catch (const std::exception& e) {
        if (out_error) *out_error = std::string("Failed to parse token JSON: ") + e.what();
        return false;
    }

    // Update in-memory + config, and also update the cfg refresh token (rotation)
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_ = snap;
        refresh_token_cfg_ = snap.refresh_token;
    }

    std::string save_err;
    if (!SaveToConfig(snap, &save_err)) {
        // Keep token in memory, but report persistence failure
        if (out_error) *out_error = "Token refreshed but failed to persist to config.json: " + save_err;
        return false;
    }

    return true;
}

bool TwitchAuth::RefreshNow(std::string* out_error) {
    static std::atomic<bool> loaded{ false };
    if (!loaded.load()) {
        std::string err;
        if (!LoadFromConfig(&err)) {
            if (out_error) *out_error = err;
            return false;
        }
        loaded.store(true);
    }
    return RefreshWithTwitch(out_error);
}

bool TwitchAuth::Start() {
    std::string err;
    if (!LoadFromConfig(&err)) {
        return false;
    }

    // Always attempt refresh at startup (since sample config doesn't store expiry)
    {
        std::string refresh_err;
        (void)RefreshWithTwitch(&refresh_err);
    }

    running_.store(true);
    worker_ = std::thread([this]() {
        // Without persisted expiry, refresh on a safe cadence (e.g., every 45 minutes).
        // Twitch access tokens are typically hours-long; 45m is conservative and lightweight.
        int seconds_until_refresh = 45 * 60;

        while (running_.load()) {
            for (int i = 0; i < seconds_until_refresh && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!running_.load()) break;

            std::string refresh_err;
            (void)RefreshWithTwitch(&refresh_err);
        }
        });

    return true;
}

void TwitchAuth::Stop() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
}

std::optional<std::string> TwitchAuth::GetAccessToken() const {
    const auto now = NowUnixSeconds();
    std::lock_guard<std::mutex> lock(mu_);
    if (!current_.has_value()) return std::nullopt;
    const auto& s = *current_;
    if (s.access_token.empty()) return std::nullopt;
    if (s.expires_at_unix > 0 && s.expires_at_unix <= now) return std::nullopt;
    return s.access_token;
}

std::optional<TwitchAuth::TokenSnapshot> TwitchAuth::GetTokenSnapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!current_.has_value()) return std::nullopt;
    return *current_;
}