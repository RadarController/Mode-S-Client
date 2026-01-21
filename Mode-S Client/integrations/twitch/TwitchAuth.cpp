#include "TwitchAuth.h"

// This translation unit uses cpp-httplib + nlohmann::json.
// Keep these includes near the top so early functions compile regardless of include order.
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

// DebugLog is implemented later in this file; forward declare for early callers.
static void DebugLog(const std::string& msg);


bool TwitchAuth::ValidateAndLogToken(const std::string& access_token, std::string* out_scope_joined) {
    if (out_scope_joined) out_scope_joined->clear();
    if (access_token.empty()) return false;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient cli("id.twitch.tv", 443);
    cli.set_follow_location(true);
#else
    DebugLog("validate FAILED (CPPHTTPLIB_OPENSSL_SUPPORT not enabled)");
    return false;
#endif

    httplib::Headers headers;
    headers.emplace("Authorization", std::string("OAuth ") + access_token);

    auto res = cli.Get("/oauth2/validate", headers);
    if (!res) { DebugLog("validate FAILED (no response)"); return false; }
    if (res->status != 200) {
        DebugLog("validate FAILED status=" + std::to_string(res->status));
        return false;
    }

    try {
        json j = json::parse(res->body);
        const std::string login = j.value("login", "");

        std::string scopes_joined;
        if (j.contains("scopes") && j["scopes"].is_array()) {
            bool first = true;
            for (auto& s : j["scopes"]) {
                if (!s.is_string()) continue;
                if (!first) scopes_joined += ",";
                scopes_joined += s.get<std::string>();
                first = false;
            }
        }

        if (out_scope_joined) *out_scope_joined = scopes_joined;
        DebugLog(std::string("validate OK; login=") + login + (scopes_joined.empty() ? "" : (", scopes=" + scopes_joined)));

        const bool has_chat_read = (scopes_joined.find("chat:read") != std::string::npos);
        const bool has_chat_write = (scopes_joined.find("chat:edit") != std::string::npos) || (scopes_joined.find("chat:write") != std::string::npos);
        if (!has_chat_read || !has_chat_write) {
            DebugLog("WARNING: token missing chat:read and/or chat:edit scopes; IRC may auth anonymously / send will fail.");
        }
        return true;
    } catch (...) {
        DebugLog("validate FAILED (json parse)");
        return false;
    }
}

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <vector>
#include <cstdio>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

// (httplib/json includes and json alias are declared at top of file)

// Hardcoded scope list (already URL-encoded).
// NOTE: Keep this encoded string as-is so we don't double-encode it.
static const char* kTwitchScopeEncoded =
"moderator%3Aread%3Afollowers+"
"channel%3Aread%3Ahype_train+"
"channel%3Aread%3Aredemptions+"
"channel%3Aread%3Asubscriptions+"
"chat%3Aread+"
"chat%3Aedit";

// Human-readable equivalent for diagnostics/logging.
static const char* kTwitchScopeReadable =
"moderator:read:followers "
"channel:read:hype_train "
"channel:read:redemptions "
"channel:read:subscriptions "
"chat:read "
"chat:edit";

static std::string MaskToken(const std::string& t) {
    if (t.empty()) return "(empty)";
    const auto n = t.size();
    if (n <= 8) return "(len=" + std::to_string(n) + ")";
    return t.substr(0, 4) + "..." + t.substr(n - 4) + " (len=" + std::to_string(n) + ")";
}

static void DebugLog(const std::string& msg) {
    const std::string line = "TWITCHAUTH: " + msg + "\n";
#ifdef _WIN32
    OutputDebugStringA(line.c_str());
#endif
    std::fprintf(stderr, "%s", line.c_str());
    std::fflush(stderr);
}

static std::string TrimAscii(std::string s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && is_ws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws((unsigned char)s.back())) s.pop_back();
    return s;
}

static std::string StripPrefix(std::string s, const char* prefix) {
    if (!prefix) return s;
    const std::string p(prefix);
    if (s.rfind(p, 0) == 0) return s.substr(p.size());
    return s;
}

static std::string NormalizeAccessTokenForValidate(std::string tok) {
    tok = TrimAscii(std::move(tok));
    tok = StripPrefix(std::move(tok), "oauth:");
    // If someone stored "Bearer <token>" from Helix, strip it.
    const std::string bearer = "Bearer ";
    if (tok.rfind(bearer, 0) == 0) tok = tok.substr(bearer.size());
    return TrimAscii(std::move(tok));
}

static std::string RandomHex(std::size_t bytes) {
    static thread_local std::mt19937_64 rng{ std::random_device{}() };
    static const char* kHex = "0123456789abcdef";
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const int v = dist(rng);
        out.push_back(kHex[(v >> 4) & 0xF]);
        out.push_back(kHex[v & 0xF]);
    }
    return out;
}

static bool TwitchValidateAccessToken(const std::string& access_token_raw,
                                     long* out_http_status,
                                     std::string* out_login,
                                     std::string* out_error) {
    const std::string tok = NormalizeAccessTokenForValidate(access_token_raw);
    if (tok.empty()) {
        if (out_error) *out_error = "empty access token";
        if (out_http_status) *out_http_status = 0;
        return false;
    }

    // https://id.twitch.tv/oauth2/validate
    const char* host = "id.twitch.tv";
    const int port = 443;
    const char* path = "/oauth2/validate";

    httplib::Headers headers;
    headers.emplace("Authorization", std::string("OAuth ") + tok);

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient cli(host, port);
    cli.set_follow_location(true);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);
    cli.set_write_timeout(10);

    auto res = cli.Get(path, headers);
    if (!res) {
        if (out_error) *out_error = "validate: failed to connect";
        if (out_http_status) *out_http_status = 0;
        return false;
    }
    if (out_http_status) *out_http_status = res->status;

    if (res->status < 200 || res->status >= 300) {
        if (out_error) {
            std::string body_preview = res->body;
            if (body_preview.size() > 300) body_preview.resize(300);
            *out_error = "validate: HTTP " + std::to_string(res->status) + " body=" + body_preview;
        }
        return false;
    }

    try {
        auto jr = json::parse(res->body);
        if (out_login) *out_login = jr.value("login", "");
    } catch (const std::exception& e) {
        if (out_error) *out_error = std::string("validate: JSON parse failed: ") + e.what();
        return false;
    }
    return true;
#else
    if (out_error) *out_error = "validate: CPPHTTPLIB_OPENSSL_SUPPORT not enabled";
    if (out_http_status) *out_http_status = 0;
    return false;
#endif
}

namespace {
    // ---- config path: CWD first, then exe dir ----
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

std::string TwitchAuth::BuildAuthorizeUrl(const std::string& redirect_uri, std::string* out_error) {
    if (out_error) out_error->clear();
    if (redirect_uri.empty()) {
        if (out_error) *out_error = "redirect_uri is empty";
        return {};
    }

    // Ensure we have client id/secret loaded.
    std::string err;
    if (client_id_.empty() || client_secret_.empty()) {
        (void)LoadFromConfig(&err); // best-effort; err returned below if still missing
    }
    if (client_id_.empty()) {
        if (out_error) *out_error = err.empty() ? "Missing twitch_client_id in config.json" : err;
        return {};
    }

    const std::string st = RandomHex(16);
    {
        std::lock_guard<std::mutex> lock(oauth_mu_);
        pending_state_ = st;
    }

    // Scope is already URL-encoded in kTwitchScopeEncoded.
    std::string url = "https://id.twitch.tv/oauth2/authorize?";
    url += "response_type=code";
    url += "&client_id=" + UrlEncode(client_id_);
    url += "&redirect_uri=" + UrlEncode(redirect_uri);
    url += "&scope=" + std::string(kTwitchScopeEncoded);
    url += "&state=" + UrlEncode(st);
    url += "&force_verify=true";

    DebugLog("built authorize URL (state=" + st + ", redirect_uri=" + redirect_uri + ")");
    return url;
}

bool TwitchAuth::HandleOAuthCallback(const std::string& code,
                                    const std::string& state,
                                    const std::string& redirect_uri,
                                    std::string* out_error) {
    if (out_error) out_error->clear();
    if (code.empty()) {
        if (out_error) *out_error = "missing 'code'";
        return false;
    }
    if (redirect_uri.empty()) {
        if (out_error) *out_error = "redirect_uri is empty";
        return false;
    }

    // Validate state if we have one.
    {
        std::lock_guard<std::mutex> lock(oauth_mu_);
        if (!pending_state_.empty() && state != pending_state_) {
            if (out_error) *out_error = "state mismatch";
            DebugLog("OAuth callback rejected: state mismatch (got=" + state + ", expected=" + pending_state_ + ")");
            return false;
        }
        // Clear it to avoid reuse.
        pending_state_.clear();
    }

    std::string cfg_err;
    if (client_id_.empty() || client_secret_.empty()) {
        if (!LoadFromConfig(&cfg_err)) {
            if (out_error) *out_error = cfg_err;
            return false;
        }
    }
    if (client_id_.empty() || client_secret_.empty()) {
        if (out_error) *out_error = "Missing twitch_client_id or twitch_client_secret in config.json";
        return false;
    }

    const std::string url = "https://id.twitch.tv/oauth2/token";
    const std::string body =
        "grant_type=authorization_code"
        "&code=" + UrlEncode(code) +
        "&client_id=" + UrlEncode(client_id_) +
        "&client_secret=" + UrlEncode(client_secret_) +
        "&redirect_uri=" + UrlEncode(redirect_uri) +
        "&scope=" + std::string(kTwitchScopeEncoded);

    long http = 0;
    std::string http_err;
    auto resp = HttpPostForm(url, body, "application/x-www-form-urlencoded", &http, &http_err);
    DebugLog("oauth exchange HTTP status=" + std::to_string(http) + ", response bytes=" + std::to_string(resp.size()));
    if (resp.empty()) {
        if (out_error) *out_error = http_err.empty() ? "Empty response from Twitch token endpoint" : http_err;
        return false;
    }
    if (http < 200 || http >= 300) {
        if (out_error) *out_error = "Twitch token endpoint returned HTTP " + std::to_string(http) + " body=" + resp;
        return false;
    }

    TokenSnapshot snap;
    int expires_in = 0;
    try {
        auto jr = json::parse(resp);
        snap.access_token = jr.value("access_token", "");
        snap.refresh_token = jr.value("refresh_token", "");
        snap.token_type = jr.value("token_type", "");
        expires_in = jr.value("expires_in", 0);

        // Record scopes for logging/diag.
        std::string validated_scopes;
        if (ValidateAndLogToken(snap.access_token, &validated_scopes) && !validated_scopes.empty()) {
            snap.scope_joined = validated_scopes;
        } else {
            snap.scope_joined = kTwitchScopeReadable;
        }
    } catch (const std::exception& e) {
        if (out_error) *out_error = std::string("Failed to parse token response: ") + e.what();
        return false;
    }

    if (snap.access_token.empty() || snap.refresh_token.empty() || expires_in <= 0) {
        if (out_error) *out_error = "Token exchange response missing fields: " + resp;
        return false;
    }
    snap.expires_at_unix = NowUnixSeconds() + expires_in;

    // Update in-memory + config.
    refresh_token_cfg_ = snap.refresh_token;
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_ = snap;
    }

    std::string save_err;
    if (!SaveToConfig(snap, &save_err)) {
        if (out_error) *out_error = save_err;
        return false;
    }

    // Notify listeners that tokens were replaced via interactive OAuth.
if (on_tokens_updated) {
    long vhttp = 0;
    std::string vlogin;
    std::string verr;
    if (!TwitchValidateAccessToken(snap.access_token, &vhttp, &vlogin, &verr)) {
        vlogin.clear();
    }
    on_tokens_updated(snap.access_token, snap.refresh_token, vlogin);
}

DebugLog("oauth exchange OK; saved new refresh token " + MaskToken(snap.refresh_token));
return true;
}

bool TwitchAuth::LoadFromConfig(std::string* out_error) {
    auto path = FindConfigPath();
    DebugLog(std::string("using config path: ") + path.string());
    json j;
    if (!ReadJsonFile(path, &j, out_error)) return false;

    // Matches your sample config:
    // twitch_client_id (top-level)
    // twitch_client_secret (top-level)
    // twitch.user_access_token / twitch.user_refresh_token (nested)
    client_id_ = j.value("twitch_client_id", "");
    client_secret_ = j.value("twitch_client_secret", "");

    DebugLog("loaded client_id len=" + std::to_string(client_id_.size()) + ", client_secret len=" + std::to_string(client_secret_.size()));
    if (!j.contains("twitch") || !j["twitch"].is_object()) {
        if (out_error) *out_error = "Missing 'twitch' object in config.json";
        return false;
    }

    const auto& tj = j["twitch"];
    std::string access = tj.value("user_access_token", "");
    std::string refresh = tj.value("user_refresh_token", "");

    refresh_token_cfg_ = refresh;

    DebugLog(std::string("loaded access_token ") + MaskToken(access) + ", refresh_token " + MaskToken(refresh));

    // Best-effort validate any existing access token (helps diagnose "Login unsuccessful" quickly).
    if (!access.empty()) {
        long vhttp = 0;
        std::string vlogin;
        std::string verr;
        if (TwitchValidateAccessToken(access, &vhttp, &vlogin, &verr)) {
            DebugLog(std::string("validate OK; login=") + (vlogin.empty() ? "(unknown)" : vlogin));
        } else {
            DebugLog(std::string("validate FAILED; ") + verr);
        }
    }
    if (client_id_.empty() || client_secret_.empty() || refresh_token_cfg_.empty()) {
        DebugLog("missing required config fields; cannot refresh.");
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

    DebugLog("persisted refreshed tokens successfully.");
    return true;
}

bool TwitchAuth::SaveToConfig(const TokenSnapshot& snap, std::string* out_error) {
    auto path = FindConfigPath();
    DebugLog(std::string("saving tokens to config path: ") + path.string());
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
    DebugLog("starting token refresh (refresh_token " + MaskToken(refresh_token_cfg_) + ", client_id len=" + std::to_string(client_id_.size()) + ")");
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
    DebugLog("token endpoint HTTP status=" + std::to_string(http) + ", response bytes=" + std::to_string(resp.size()));
    if (resp.empty()) {
        DebugLog(std::string("empty response from token endpoint; error=") + http_err);
        if (out_error) *out_error = http_err.empty() ? "Empty response from Twitch token endpoint" : http_err;
        return false;
    }
    if (http < 200 || http >= 300) {
        std::string body_preview = resp;
        if (body_preview.size() > 500) body_preview.resize(500);
        DebugLog("refresh failed; body preview: " + body_preview);
        if (out_error) *out_error = "Twitch token endpoint returned HTTP " + std::to_string(http) + " body=" + resp;
        return false;
    }

    TokenSnapshot snap;
    std::string validated_login;
    try {
        auto jr = json::parse(resp);

        snap.access_token = jr.value("access_token", "");
        snap.refresh_token = jr.value("refresh_token", "");
        snap.token_type = jr.value("token_type", "");
        int expires_in = jr.value("expires_in", 0);

        // Record scopes for logging/diag. Prefer /oauth2/validate (ground truth), fallback to expected scopes.
        {
            std::string validated_scopes;
            if (ValidateAndLogToken(snap.access_token, &validated_scopes) && !validated_scopes.empty()) {
                snap.scope_joined = validated_scopes;
            } else {
                snap.scope_joined = kTwitchScopeReadable;
            }
        }

        if (snap.access_token.empty() || snap.refresh_token.empty() || expires_in <= 0) {
            if (out_error) *out_error = "Token refresh response missing fields: " + resp;
            return false;
        }

        const auto now = NowUnixSeconds();
        snap.expires_at_unix = now + expires_in;
        DebugLog("refresh OK; access_token " + MaskToken(snap.access_token) + ", expires_in=" + std::to_string(expires_in) + "s, new refresh_token " + MaskToken(snap.refresh_token));

        // Validate immediately so we can surface invalid tokens early and avoid IRC "Login unsuccessful".
        {
            long vhttp = 0;
            std::string vlogin;
            std::string verr;
            if (TwitchValidateAccessToken(snap.access_token, &vhttp, &vlogin, &verr)) {
                DebugLog(std::string("validate OK; login=") + (vlogin.empty() ? "(unknown)" : vlogin));
                validated_login = vlogin;
            } else {
                DebugLog(std::string("validate FAILED after refresh; ") + verr);
                if (out_error) *out_error = std::string("Refreshed token is invalid: ") + verr;
                return false;
            }
        }

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
        DebugLog("FAILED to persist refreshed tokens: " + save_err);
        // Keep token in memory, but report persistence failure
        if (out_error) *out_error = "Token refreshed but failed to persist to config.json: " + save_err;
        return false;
    }


// Notify listeners (e.g., IRC/EventSub) that a fresh token is available.
if (on_tokens_updated) {
    // If validate didn't return a login (rare), pass empty string.
    on_tokens_updated(snap.access_token, snap.refresh_token, validated_login);
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
    DebugLog("Start() called");
    std::string err;
    if (!LoadFromConfig(&err)) {
        return false;
    }

    // Always attempt refresh at startup (since sample config doesn't store expiry)
    {
        std::string refresh_err;
            if (!RefreshWithTwitch(&refresh_err)) {
                DebugLog("token refresh FAILED: " + refresh_err);
            } else {
                DebugLog("token refresh succeeded");
            }
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
            if (!RefreshWithTwitch(&refresh_err)) {
                DebugLog("token refresh FAILED: " + refresh_err);
            } else {
                DebugLog("token refresh succeeded");
            }
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