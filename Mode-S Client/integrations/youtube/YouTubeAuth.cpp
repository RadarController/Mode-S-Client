#include "YouTubeAuth.h"

// This translation unit uses cpp-httplib + nlohmann::json.
#include "httplib.h"
#include "json.hpp"

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

using json = nlohmann::json;

// Forward declare for early callers.
static void DebugLog(const std::string& msg);

YouTubeAuth::UiLogFn YouTubeAuth::ui_log_ = nullptr;

void YouTubeAuth::SetUiLogger(UiLogFn fn) { ui_log_ = fn; }


// ---- helpers ----
static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w; w.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}


namespace {




// config path: CWD first, then exe dir (same approach as TwitchAuth)
std::filesystem::path FindConfigPath() {
    std::vector<std::filesystem::path> candidates;
    try { candidates.push_back(std::filesystem::current_path() / "config.json"); } catch (...) {}

#ifdef _WIN32
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
    if (out_error) out_error->clear();
    std::ifstream f(p, std::ios::in | std::ios::binary);
    if (!f) {
        if (out_error) *out_error = "Could not open " + p.string();
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        *out = json::parse(ss.str());
        return true;
    }
    catch (const std::exception& e) {
        if (out_error) *out_error = std::string("JSON parse failed: ") + e.what();
        return false;
    }
}

bool WriteJsonFile(const std::filesystem::path& p, const json& j, std::string* out_error) {
    if (out_error) out_error->clear();
    std::ofstream f(p, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f) {
        if (out_error) *out_error = "Could not write " + p.string();
        return false;
    }
    f << j.dump(2);
    return true;
}

std::int64_t NowUnix() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace



void YouTubeAuth::LogUi(const std::string& msg) {
    if (!ui_log_) return;
    ui_log_(ToW(msg));
}
// ---- scopes ----
const char* YouTubeAuth::RequiredScopeReadable() {
    // Minimum required for updating YouTube metadata (videos.update, liveBroadcasts.update, etc.)
    return "youtube.force-ssl";
}
const char* YouTubeAuth::RequiredScopeEncoded() {
    // URL-encoded space-delimited scopes. (Single scope here.)
    return "https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fyoutube.force-ssl";
}

// ---- lifecycle ----
bool YouTubeAuth::Start() {
    Stop();
    running_.store(true);

    std::string err;
    if (!LoadFromConfig(&err)) {
        DebugLog("YTAUTH: load config failed: " + err);
        // Still start background loop so user can complete OAuth later.
    }
    // Best-effort: if we already have a refresh token, refresh immediately if needed.
    (void)RefreshNow(&err);

    bg_ = std::thread([this]() {
        DebugLog("YTAUTH: background refresh loop started");
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const auto now = NowUnix();
            bool need = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                need = NeedsRefresh(now);
            }
            if (need) {
                std::string e;
                if (!RefreshWithGoogle(&e)) {
                    DebugLog("YTAUTH: refresh failed: " + e);
                }
            }
        }
        DebugLog("YTAUTH: background refresh loop stopped");
    });

    return true;
}

void YouTubeAuth::Stop() {
    running_.store(false);
    if (bg_.joinable()) bg_.join();
}

std::optional<std::string> YouTubeAuth::GetAccessToken() const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = NowUnix();
    if (tokens_.access_token.empty()) return std::nullopt;
    if (tokens_.expires_at_unix != 0 && (now + 30) >= tokens_.expires_at_unix) return std::nullopt;
    return tokens_.access_token;
}

std::optional<YouTubeAuth::TokenSnapshot> YouTubeAuth::GetTokenSnapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (tokens_.access_token.empty() && tokens_.refresh_token.empty()) return std::nullopt;
    return tokens_;
}

bool YouTubeAuth::RefreshNow(std::string* out_error) {
    if (out_error) out_error->clear();
    const auto now = NowUnix();
    bool need = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        need = NeedsRefresh(now);
    }
    if (!need) return true;
    return RefreshWithGoogle(out_error);
}

bool YouTubeAuth::NeedsRefresh(std::int64_t now_unix) const {
    // Refresh if:
    // - no access token but we have refresh token
    // - token expires within 60 seconds
    if (!tokens_.refresh_token.empty() && tokens_.access_token.empty()) return true;
    if (!tokens_.refresh_token.empty() && tokens_.expires_at_unix != 0 && (now_unix + 60) >= tokens_.expires_at_unix) return true;
    return false;
}

// ---- interactive OAuth ----
std::string YouTubeAuth::BuildAuthorizeUrl(const std::string& redirect_uri, std::string* out_error) {
    if (out_error) out_error->clear();
    const char* kDefault = "http://localhost:17845/auth/youtube/callback";
    const std::string eff_redirect = redirect_uri.empty() ? kDefault : redirect_uri;

    std::string err;
    if (client_id_.empty() || client_secret_.empty()) {
        (void)LoadFromConfig(&err);
    }
    if (client_id_.empty()) {
        if (out_error) *out_error = err.empty() ? "Missing youtube.client_id in config.json" : err;
        return {};
    }

    const std::string st = RandomHex(16);
    {
        std::lock_guard<std::mutex> lock(oauth_mu_);
        pending_state_ = st;
    }

    // Google OAuth v2 endpoint
    std::string url = "https://accounts.google.com/o/oauth2/v2/auth?";
    url += "response_type=code";
    url += "&client_id=" + UrlEncode(client_id_);
    url += "&redirect_uri=" + UrlEncode(eff_redirect);
    url += "&scope=" + std::string(RequiredScopeEncoded());
    url += "&access_type=offline";
    url += "&prompt=consent";
    url += "&include_granted_scopes=true";
    url += "&state=" + UrlEncode(st);
    return url;
}

bool YouTubeAuth::HandleOAuthCallback(const std::string& code,
    const std::string& state,
    const std::string& redirect_uri,
    std::string* out_error)
{
    if (out_error) out_error->clear();
    const char* kDefault = "http://localhost:17845/auth/youtube/callback";
    const std::string eff_redirect = redirect_uri.empty() ? kDefault : redirect_uri;

    std::string st;
    {
        std::lock_guard<std::mutex> lock(oauth_mu_);
        st = pending_state_;
    }
    if (st.empty() || state != st) {
        if (out_error) *out_error = "Invalid OAuth state (CSRF check failed)";
        return false;
    }
    if (code.empty()) {
        if (out_error) *out_error = "Missing OAuth code";
        return false;
    }

    std::string err;
    if (client_id_.empty() || client_secret_.empty()) {
        (void)LoadFromConfig(&err);
    }
    if (client_id_.empty() || client_secret_.empty()) {
        if (out_error) *out_error = err.empty() ? "Missing youtube.client_id/client_secret in config.json" : err;
        return false;
    }

    // Exchange code for tokens
    std::string body;
    body += "code=" + UrlEncode(code);
    body += "&client_id=" + UrlEncode(client_id_);
    body += "&client_secret=" + UrlEncode(client_secret_);
    body += "&redirect_uri=" + UrlEncode(eff_redirect);
    body += "&grant_type=authorization_code";

    long status = 0;
    std::string resp = HttpPostForm("https://oauth2.googleapis.com/token", body,
        { {"Content-Type", "application/x-www-form-urlencoded"} },
        &status, &err);

    if (resp.empty() || status < 200 || status >= 300) {
        if (out_error) *out_error = err.empty() ? ("token exchange failed http=" + std::to_string(status)) : err;
        return false;
    }

    try {
        json j = json::parse(resp);
        TokenSnapshot snap;
        snap.access_token = j.value("access_token", "");
        snap.refresh_token = j.value("refresh_token", ""); // may be empty if Google didn't issue one
        snap.token_type = j.value("token_type", "");
        const int expires_in = j.value("expires_in", 0);
        snap.expires_at_unix = NowUnix() + expires_in;
        snap.scope_joined = j.value("scope", "");

        if (snap.access_token.empty()) {
            if (out_error) *out_error = "token exchange returned empty access_token";
            return false;
        }

        // If refresh_token wasn't returned (common if previously consented), keep existing refresh_token.
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (snap.refresh_token.empty()) snap.refresh_token = tokens_.refresh_token;
            tokens_ = snap;
        }

        std::string scopes, channel;
        if (!ValidateAndLogToken(snap.access_token, &scopes, &channel)) {
            if (out_error) *out_error = "token validation failed";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            tokens_.scope_joined = scopes;
            channel_id_ = channel;
        }

        if (!SaveToConfig(tokens_, out_error)) return false;

        if (on_tokens_updated) on_tokens_updated(tokens_.access_token, tokens_.refresh_token, channel_id_);

        DebugLog("YTAUTH: OAuth callback success (channel_id=" + channel_id_ + ")");
        return true;
    }
    catch (const std::exception& e) {
        if (out_error) *out_error = std::string("token JSON parse failed: ") + e.what();
        return false;
    }
}

// ---- refresh ----
bool YouTubeAuth::RefreshWithGoogle(std::string* out_error) {
    if (out_error) out_error->clear();

    std::string refresh;
    {
        std::lock_guard<std::mutex> lock(mu_);
        refresh = tokens_.refresh_token;
    }
    if (refresh.empty()) {
        if (out_error) *out_error = "No refresh token (complete OAuth first)";
        return false;
    }

    std::string err;
    if (client_id_.empty() || client_secret_.empty()) {
        (void)LoadFromConfig(&err);
    }
    if (client_id_.empty() || client_secret_.empty()) {
        if (out_error) *out_error = err.empty() ? "Missing youtube.client_id/client_secret in config.json" : err;
        return false;
    }

    std::string body;
    body += "client_id=" + UrlEncode(client_id_);
    body += "&client_secret=" + UrlEncode(client_secret_);
    body += "&refresh_token=" + UrlEncode(refresh);
    body += "&grant_type=refresh_token";

    long status = 0;
    std::string resp = HttpPostForm("https://oauth2.googleapis.com/token", body,
        { {"Content-Type", "application/x-www-form-urlencoded"} },
        &status, &err);

    if (resp.empty() || status < 200 || status >= 300) {
        if (out_error) *out_error = err.empty() ? ("refresh failed http=" + std::to_string(status)) : err;
        return false;
    }

    try {
        json j = json::parse(resp);
        const std::string access = j.value("access_token", "");
        const int expires_in = j.value("expires_in", 0);
        const std::string token_type = j.value("token_type", "");
        const std::string scope = j.value("scope", "");

        if (access.empty()) {
            if (out_error) *out_error = "refresh returned empty access_token";
            return false;
        }

        TokenSnapshot snap;
        {
            std::lock_guard<std::mutex> lock(mu_);
            snap = tokens_;
            snap.access_token = access;
            snap.token_type = token_type;
            snap.expires_at_unix = NowUnix() + expires_in;
            if (!scope.empty()) snap.scope_joined = scope;
        }

        std::string scopes, channel;
        (void)ValidateAndLogToken(snap.access_token, &scopes, &channel);

        {
            std::lock_guard<std::mutex> lock(mu_);
            tokens_ = snap;
            if (!scopes.empty()) tokens_.scope_joined = scopes;
            if (!channel.empty()) channel_id_ = channel;
        }

        if (!SaveToConfig(tokens_, out_error)) return false;

        if (on_tokens_updated) on_tokens_updated(tokens_.access_token, tokens_.refresh_token, channel_id_);

        DebugLog("YTAUTH: refresh ok (expires_at=" + std::to_string(tokens_.expires_at_unix) + ")");
        return true;
    }
    catch (const std::exception& e) {
        if (out_error) *out_error = std::string("refresh JSON parse failed: ") + e.what();
        return false;
    }
}

// ---- validate ----
bool YouTubeAuth::ValidateAndLogToken(const std::string& access_token, std::string* out_scope_joined, std::string* out_channel_id) {
    if (out_scope_joined) out_scope_joined->clear();
    if (out_channel_id) out_channel_id->clear();
    if (access_token.empty()) return false;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient cli("www.googleapis.com", 443);
    cli.set_follow_location(true);
#else
    DebugLog("YTAUTH: validate FAILED (CPPHTTPLIB_OPENSSL_SUPPORT not enabled)");
    return false;
#endif

    // tokeninfo gives scopes + expiry, but not channel id. We'll also call "channels?mine=true".
    httplib::Headers headers;
    headers.emplace("Authorization", std::string("Bearer ") + access_token);

    auto res = cli.Get("/oauth2/v3/tokeninfo?access_token=" + UrlEncode(access_token), headers);
    if (!res || res->status != 200) {
        DebugLog("YTAUTH: tokeninfo failed");
        // continue anyway; token may still work for API calls
    } else {
        try {
            json j = json::parse(res->body);
            const std::string scope = j.value("scope", "");
            if (out_scope_joined) *out_scope_joined = scope;
        } catch (...) {}
    }

    // Try to resolve channel id (best-effort)
    httplib::SSLClient api("www.googleapis.com", 443);
    api.set_follow_location(true);
    auto res2 = api.Get("/youtube/v3/channels?part=id&mine=true", headers);
    if (res2 && res2->status == 200) {
        try {
            json j = json::parse(res2->body);
            if (j.contains("items") && j["items"].is_array() && !j["items"].empty()) {
                const auto& it = j["items"][0];
                std::string id = it.value("id", "");
                if (out_channel_id) *out_channel_id = id;
            }
        } catch (...) {}
    }

    return true;
}

// ---- config io ----
bool YouTubeAuth::LoadFromConfig(std::string* out_error) {
    if (out_error) out_error->clear();
    const auto p = FindConfigPath();
    json root;
    std::string err;
    if (!ReadJsonFile(p, &root, &err)) {
        if (out_error) *out_error = err;
        return false;
    }

    // Expected structure:
    // {
    //   "youtube": {
    //     "client_id": "...",
    //     "client_secret": "...",
    //     "access_token": "...",
    //     "refresh_token": "...",
    //     "expires_at_unix": 123,
    //     "scope": "..."
    //   }
    // }
    const json yt = root.value("youtube", json::object());

    std::lock_guard<std::mutex> lock(mu_);
    client_id_ = yt.value("client_id", "");
    client_secret_ = yt.value("client_secret", "");
    tokens_.access_token = yt.value("access_token", "");
    tokens_.refresh_token = yt.value("refresh_token", "");
    tokens_.expires_at_unix = yt.value("expires_at_unix", (std::int64_t)0);
    tokens_.token_type = yt.value("token_type", "");
    tokens_.scope_joined = yt.value("scope", "");
    channel_id_ = yt.value("channel_id", "");
    return true;
}

bool YouTubeAuth::SaveToConfig(const TokenSnapshot& snap, std::string* out_error) {
    if (out_error) out_error->clear();
    const auto p = FindConfigPath();
    json root;
    std::string err;
    if (!ReadJsonFile(p, &root, &err)) {
        // If config doesn't exist, we can create a new one.
        root = json::object();
    }

    json yt = root.value("youtube", json::object());
    {
        std::lock_guard<std::mutex> lock(mu_);
        yt["client_id"] = client_id_;
        yt["client_secret"] = client_secret_;
        yt["access_token"] = snap.access_token;
        yt["refresh_token"] = snap.refresh_token;
        yt["expires_at_unix"] = snap.expires_at_unix;
        yt["token_type"] = snap.token_type;
        yt["scope"] = snap.scope_joined;
        if (!channel_id_.empty()) yt["channel_id"] = channel_id_;
    }
    root["youtube"] = yt;

    if (!WriteJsonFile(p, root, &err)) {
        if (out_error) *out_error = err;
        return false;
    }
    DebugLog("YTAUTH: saved tokens to " + p.string());
    return true;
}

// ---- HTTP helpers ----
std::string YouTubeAuth::HttpPostForm(const std::string& url,
    const std::string& form_body,
    const std::initializer_list<std::pair<std::string, std::string>>& headers,
    long* out_http_status,
    std::string* out_error)
{
    if (out_http_status) *out_http_status = 0;
    if (out_error) out_error->clear();

    // Minimal URL parsing
    std::string scheme, host, path;
    {
        auto pos = url.find("://");
        scheme = (pos == std::string::npos) ? "https" : url.substr(0, pos);
        auto rest = (pos == std::string::npos) ? url : url.substr(pos + 3);
        auto slash = rest.find('/');
        host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    }

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient cli(host.c_str(), 443);
    cli.set_follow_location(true);
#else
    if (out_error) *out_error = "CPPHTTPLIB_OPENSSL_SUPPORT not enabled";
    return {};
#endif

    httplib::Headers h;
    for (auto& kv : headers) h.emplace(kv.first, kv.second);

    auto res = cli.Post(path.c_str(), h, form_body, "application/x-www-form-urlencoded");
    if (!res) {
        if (out_error) *out_error = "no response";
        return {};
    }
    if (out_http_status) *out_http_status = res->status;
    if (res->status < 200 || res->status >= 300) {
        if (out_error) *out_error = "HTTP " + std::to_string(res->status) + " body=" + res->body;
    }
    return res->body;
}

std::string YouTubeAuth::UrlEncode(const std::string& s) {
    std::ostringstream out;
    out.fill('0');
    out << std::hex;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else if (c == ' ') {
            out << "%20";
        } else {
            out << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
        }
    }
    return out.str();
}

std::string YouTubeAuth::RandomHex(size_t bytes) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    std::ostringstream out;
    out << std::hex;
    for (size_t i = 0; i < bytes; ++i) {
        int v = dist(gen);
        out << std::setw(2) << std::setfill('0') << (v & 0xff);
    }
    return out.str();
}

// ---- debug log ----
static void DebugLog(const std::string& msg) {
#ifdef _WIN32
    OutputDebugStringW((L"YTAUTH: " + ToW(msg) + L"\n").c_str());
#endif
    YouTubeAuth::LogUi(msg);
}
