#include "HttpServer.h"
#include <Windows.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#include "json.hpp"
#include "../AppState.h"
#include "../chat/ChatAggregator.h"
#include "../../integrations/euroscope/EuroScopeIngestService.h"
#include "../../integrations/twitch/TwitchHelixService.h"
#include "../../integrations/twitch/TwitchAuth.h"
#include "../../integrations/youtube/YouTubeAuth.h"
#include "../AppConfig.h"

namespace {
    inline void SafeOutputLog(std::function<void(const std::wstring&)>& log, const std::wstring& msg) {
        ::OutputDebugStringW((msg + L"\n").c_str());
        try {
            log(msg);
        }
        catch (...) {
            // Never allow logging to break server threads
        }
    }
}

// --------------------------------------------------------------------------------------
// Helpers: stream draft storage + minimal YouTube Data API calls (used by /api/youtube/vod/*)
// NOTE: these are file-scope helpers so route lambdas can call them reliably.
// --------------------------------------------------------------------------------------
static std::filesystem::path FindStreaminfoPath() {
    std::vector<std::filesystem::path> candidates;
    try { candidates.push_back(std::filesystem::current_path() / "twitch_streaminfo.json"); } catch (...) {}
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0) {
        std::filesystem::path exe = std::filesystem::path(buf).parent_path();
        candidates.push_back(exe / "twitch_streaminfo.json");
    }
#endif
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec) return p;
    }
    return std::filesystem::path("twitch_streaminfo.json");
}

static bool ReadStreaminfoJson(nlohmann::json* out) {
    const auto p = FindStreaminfoPath();
    std::ifstream f(p, std::ios::in | std::ios::binary);
    if (!f) { *out = nlohmann::json::object(); return false; }
    std::stringstream ss; ss << f.rdbuf();
    try { *out = nlohmann::json::parse(ss.str()); return true; }
    catch (...) { *out = nlohmann::json::object(); return false; }
}

static bool WriteStreaminfoJson(const nlohmann::json& j) {
    const auto p = FindStreaminfoPath();
    std::ofstream f(p, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2);
    return true;
}

static bool YouTubeApiGet(const std::string& path_with_query,
                          const std::string& access_token,
                          long* out_status,
                          std::string* out_body) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient cli("www.googleapis.com", 443);
    cli.set_follow_location(true);
    httplib::Headers h;
    h.emplace("Authorization", std::string("Bearer ") + access_token);
    auto res = cli.Get(path_with_query.c_str(), h);
    if (!res) { if (out_status) *out_status = 0; if (out_body) *out_body = ""; return false; }
    if (out_status) *out_status = res->status;
    if (out_body) *out_body = res->body;
    return true;
#else
    if (out_status) *out_status = 0;
    if (out_body) *out_body = "openssl_not_enabled";
    return false;
#endif
}

static bool YouTubeApiPutJson(const std::string& path_with_query,
                              const std::string& access_token,
                              const std::string& body_json,
                              long* out_status,
                              std::string* out_body) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient cli("www.googleapis.com", 443);
    cli.set_follow_location(true);
    httplib::Headers h;
    h.emplace("Authorization", std::string("Bearer ") + access_token);
    h.emplace("Content-Type", "application/json; charset=utf-8");
    auto res = cli.Put(path_with_query.c_str(), h, body_json, "application/json; charset=utf-8");
    if (!res) { if (out_status) *out_status = 0; if (out_body) *out_body = ""; return false; }
    if (out_status) *out_status = res->status;
    if (out_body) *out_body = res->body;
    return true;
#else
    if (out_status) *out_status = 0;
    if (out_body) *out_body = "openssl_not_enabled";
    return false;
#endif
}




using json = nlohmann::json;

static std::string Trim(const std::string& s) {
    auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t a = 0, b = s.size();
    while (a < b && is_space((unsigned char)s[a])) a++;
    while (b > a && is_space((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static bool ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return false;
    bool changed = false;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
    }
    return changed;
}

static std::string ReadFileUtf8(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static const char* ContentTypeFor(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "js")   return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "webp") return "image/webp";
    return "application/octet-stream";
}

static std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out;
    out.resize((size_t)needed);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out.data(), needed, nullptr, nullptr);
    return out;
}

HttpServer::HttpServer(AppState& state,
    ChatAggregator& chat,
    EuroScopeIngestService& euroscope,
    AppConfig& config,
    Options options,
    LogFn log)
    : state_(state),
    chat_(chat),
    euroscope_(euroscope),
    config_(config),
    opt_(std::move(options)),
    log_(std::move(log)) {
    if (!log_) {
        log_ = [](const std::wstring&) {};
    }
}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::Start() {
    if (svr_) return;

    svr_ = std::make_unique<httplib::Server>();
    RegisterRoutes();

    thread_ = std::thread([this]() {
        try {
            SafeOutputLog(log_, L"HTTP: listening on http://127.0.0.1:" + std::to_wstring(opt_.port));
            svr_->listen(opt_.bind_host.c_str(), opt_.port);
        }
        catch (...) {
            SafeOutputLog(log_, L"HTTP: server thread crashed");
        }
        });
}

void HttpServer::Stop() {
    if (!svr_) return;
    try {
        svr_->stop();
    }
    catch (...) {}

    // Join the server thread so the process exits cleanly.
    // (listen() will return after stop()).
    if (thread_.joinable()) {
        if (std::this_thread::get_id() == thread_.get_id()) {
            // Defensive: if Stop() is ever called from the server thread itself.
            thread_.detach();
        }
        else {
            thread_.join();
        }
    }

    svr_.reset();
}

// Inserts `insert` right after the first occurrence of `needle`.
// Returns true if inserted.
static bool InsertAfterFirst(std::string& s, const std::string& needle, const std::string& insert)
{
    const size_t pos = s.find(needle);
    if (pos == std::string::npos) return false;
    s.insert(pos + needle.size(), insert);
    return true;
}

// Inserts `insert` right before the first occurrence of `needle`.
// Returns true if inserted.
static bool InsertBeforeFirst(std::string& s, const std::string& needle, const std::string& insert)
{
    const size_t pos = s.find(needle);
    if (pos == std::string::npos) return false;
    s.insert(pos, insert);
    return true;
}

void HttpServer::ApplyOverlayTokens(std::string& html)
{
    // Default font: Inter (can be made configurable later)
    const std::string googleLink =
        "<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">"
        "<link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>"
        "<link href=\"https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700&display=swap\" rel=\"stylesheet\">";

    const std::string fontStack =
        "Inter, system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif";

    const std::string shadow =
        (config_.overlay_text_shadow ? "text-shadow: 0 2px 12px rgba(0,0,0,.65);" : "");

    // 1) Ensure the Google font link exists (insert it once if missing)
    // Try common insertion points.
    if (html.find("fonts.googleapis.com") == std::string::npos)
    {
        if (!InsertAfterFirst(html, "<head>", googleLink))
        {
            // Fallback: try before </head>
            InsertBeforeFirst(html, "</head>", googleLink);
        }
    }

    // 2) Token replacement (support both token naming schemes) — NO RECURSION
    // Add/adjust tokens to match what your overlay uses.
    bool changed = false;

    // Font stack tokens (example: old + new names)
    changed |= ReplaceAll(html, "{{FONT_STACK}}", fontStack);
    changed |= ReplaceAll(html, "{FONT_STACK}", fontStack);

    // Text shadow style tokens
    changed |= ReplaceAll(html, "{{TEXT_SHADOW_STYLE}}", shadow);
    changed |= ReplaceAll(html, "{TEXT_SHADOW_STYLE}", shadow);

    // If you also have placeholders like --font-stack in CSS, you can do:
    // changed |= ReplaceAll(html, "%%FONT_STACK%%", fontStack);

    // Overlay header tokens (instant render before JS polling kicks in)
    {
        const auto hdr = state_.overlay_header_snapshot();

        auto html_escape = [](const std::string& s) {
            std::string o;
            o.reserve(s.size());
            for (char c : s) {
                switch (c) {
                case '&': o += "&amp;"; break;
                case '<': o += "&lt;"; break;
                case '>': o += "&gt;"; break;
                case '"': o += "&quot;"; break;
                case '\'': o += "&#39;"; break;
                default: o += c; break;
                }
            }
            return o;
            };

        const std::string t = html_escape(hdr.title);
        const std::string s = html_escape(hdr.subtitle);

        ReplaceAll(html, "{{HEADER_TITLE}}", t);
        ReplaceAll(html, "{HEADER_TITLE}", t);
        ReplaceAll(html, "{{HEADER_SUBTITLE}}", s);
        ReplaceAll(html, "{HEADER_SUBTITLE}", s);
    }

    (void)changed; // keep if you don't use it; helps debugging if you later add a loop
}

void HttpServer::RegisterRoutes() {
    auto& svr = *svr_;

    // --- API: log (for Web UI) ---
    svr.Get("/api/log", [&](const httplib::Request& req, httplib::Response& res) {
        std::uint64_t since = 0;
        int limit = 200;

        if (req.has_param("since")) {
            try { since = (std::uint64_t)std::stoull(req.get_param_value("since")); }
            catch (...) {}
        }
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); }
            catch (...) {}
        }

        auto j = state_.log_json(since, limit);
        res.set_content(j.dump(), "application/json; charset=utf-8");
        });

    // --- API: metrics ---
    svr.Get("/api/metrics", [&](const httplib::Request&, httplib::Response& res) {
        auto j = state_.metrics_json();

        const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // Merge EuroScope ingest snapshot into the metrics payload
        j.update(euroscope_.Metrics(now_ms));

        res.set_content(j.dump(2), "application/json; charset=utf-8");
        });

    // --- API: Twitch EventSub diagnostics ---
    
    // --- API: Twitch category lookup (for typeahead in UI) ---
    // GET /api/twitch/categories?q=<text>
    svr.Get("/api/twitch/categories", [&](const httplib::Request& req, httplib::Response& res) {
        std::string q;
        if (req.has_param("q")) q = req.get_param_value("q");
        else if (req.has_param("query")) q = req.get_param_value("query");

        // Trim
        q.erase(q.begin(), std::find_if(q.begin(), q.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        q.erase(std::find_if(q.rbegin(), q.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), q.end());

        if (q.size() < 2) {
            res.set_content("[]", "application/json");
            res.status = 200;
            return;
        }

        std::vector<TwitchCategory> cats;
        std::string err;
        if (!TwitchHelixSearchCategories(config_, q, cats, &err)) {
            json jerr = { {"ok", false}, {"error", err} };
            res.set_content(jerr.dump(), "application/json");
            res.status = 500;
            return;
        }

        json out = json::array();
        for (const auto& c : cats) {
            out.push_back({ {"id", c.id}, {"name", c.name} });
        }
        res.set_content(out.dump(), "application/json");
        res.status = 200;
    });



    // --- API: Twitch Stream Info draft (used by /app/twitch_stream.html) ---
    // GET /api/twitch/streaminfo
    svr.Get("/api/twitch/streaminfo", [&](const httplib::Request&, httplib::Response& res) {
        auto j = state_.twitch_stream_draft_json();
        res.set_content(j.dump(2), "application/json; charset=utf-8");
        res.status = 200;
    });

    // POST /api/twitch/streaminfo (save draft)
    svr.Post("/api/twitch/streaminfo", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = nlohmann::json::parse(req.body);

            AppState::TwitchStreamDraft d;

            d.title = j.value("title", "");
            d.description = j.value("description", "");

            // Display name (safe to keep)
            d.category_name = j.value("category_name", j.value("category", ""));

            // IMPORTANT: persist the ID (Helix needs this)
            d.category_id = j.value("category_id", j.value("game_id", ""));

            state_.set_twitch_stream_draft(d);

            nlohmann::json out = { {"ok", true} };
            res.set_content(out.dump(), "application/json; charset=utf-8");
            res.status = 200;
        }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
        }
    });

    // POST /api/twitch/streaminfo/apply (apply to Twitch now)
    svr.Post("/api/twitch/streaminfo/apply", [&](const httplib::Request&, httplib::Response& res) {
        const auto d = state_.twitch_stream_draft_snapshot();

        SafeOutputLog(log_,
            L"TWITCH APPLY: title=" + std::wstring(d.title.begin(), d.title.end()) +
            L" category_name=" + std::wstring(d.category_name.begin(), d.category_name.end()) +
            L" category_id=" + std::wstring(d.category_id.begin(), d.category_id.end())
        );

        std::string err;
        if (!TwitchHelixUpdateChannelInfo(
            config_,
            d.title,
            d.category_id,   // <-- MUST be ID
            &err))
        {
            nlohmann::json out = { {"ok", false}, {"error", err} };
            res.status = 500;
            res.set_content(out.dump(), "application/json; charset=utf-8");
            return;
        }

        nlohmann::json out = { {"ok", true} };
        res.set_content(out.dump(), "application/json; charset=utf-8");
        res.status = 200;
    });

svr.Get("/api/twitch/eventsub/status", [&](const httplib::Request&, httplib::Response& res) {
        auto j = state_.twitch_eventsub_status_json();
        res.set_content(j.dump(2), "application/json; charset=utf-8");
        });


    svr.Get("/api/twitch/eventsub/errors", [&](const httplib::Request& req, httplib::Response& res) {
        int limit = 50;
        if (req.has_param("limit")) {
            try { limit = std::max(1, std::min(1000, std::stoi(req.get_param_value("limit")))); }
            catch (...) {}
        }
        auto j = state_.twitch_eventsub_errors_json(limit);
        res.set_content(j.dump(2), "application/json; charset=utf-8");
    });

    svr.Get("/api/twitch/eventsub/events", [&](const httplib::Request& req, httplib::Response& res) {
        int limit = 200;
        if (req.has_param("limit")) {
            try { limit = std::max(1, std::min(1000, std::stoi(req.get_param_value("limit")))); }
            catch (...) {}
        }
        auto j = state_.twitch_eventsub_events_json(limit);
        res.set_content(j.dump(2), "application/json; charset=utf-8");
        });

    // --- API: YouTube VOD draft (stored in twitch_streaminfo.json for now) ---
    // GET /api/youtube/vod/draft
    svr.Get("/api/youtube/vod/draft", [&](const httplib::Request&, httplib::Response& res) {
        nlohmann::json j;
        ReadStreaminfoJson(&j);

        nlohmann::json out;
        out["ok"] = true;
        out["title"] = j.value("youtube_vod_title", "");
        out["description"] = j.value("youtube_vod_description", "");
        res.status = 200;
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        });

    // POST /api/youtube/vod/draft
    svr.Post("/api/youtube/vod/draft", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto in = nlohmann::json::parse(req.body);
            nlohmann::json j;
            ReadStreaminfoJson(&j);

            j["youtube_vod_title"] = in.value("title", "");
            j["youtube_vod_description"] = in.value("description", "");

            if (!WriteStreaminfoJson(j)) {
                res.status = 500;
                res.set_content(R"({"ok":false,"error":"write_failed"})", "application/json; charset=utf-8");
                return;
            }

            res.status = 200;
            res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
        }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
        }
        });

    // POST /api/youtube/vod/apply
    svr.Post("/api/youtube/vod/apply", [&](const httplib::Request&, httplib::Response& res) {
        if (!opt_.youtube_get_access_token) {
            res.status = 500;
            res.set_content(R"({"ok":false,"error":"youtube_token_provider_missing"})", "application/json; charset=utf-8");
            return;
        }

        const auto tok = opt_.youtube_get_access_token();
        if (!tok.has_value() || tok->empty()) {
            res.status = 401;
            res.set_content(R"({"ok":false,"error":"youtube_not_connected"})", "application/json; charset=utf-8");
            return;
        }
        const std::string access_token = *tok;

        // Load draft
        nlohmann::json jdraft;
        ReadStreaminfoJson(&jdraft);
        const std::string new_title = jdraft.value("youtube_vod_title", "");
        const std::string new_desc = jdraft.value("youtube_vod_description", "");

        // Determine target video id
        std::string video_id;
        std::string mode;

        long st = 0;
        std::string body;

        // 1) Active broadcast (live now)
        if (YouTubeApiGet("/youtube/v3/liveBroadcasts?part=id&broadcastStatus=active&mine=true&maxResults=1",
            access_token, &st, &body) && st == 200) {
            try {
                auto jb = nlohmann::json::parse(body);
                if (jb.contains("items") && jb["items"].is_array() && !jb["items"].empty()) {
                    video_id = jb["items"][0].value("id", "");
                    if (!video_id.empty()) mode = "live";
                }
            }
            catch (...) {}
        }

        // 2) Fallback: latest completed livestream
        if (video_id.empty()) {
            if (YouTubeApiGet("/youtube/v3/search?part=id&forMine=true&type=video&eventType=completed&order=date&maxResults=1",
                access_token, &st, &body) && st == 200) {
                try {
                    auto js = nlohmann::json::parse(body);
                    if (js.contains("items") && js["items"].is_array() && !js["items"].empty()) {
                        const auto& it = js["items"][0];
                        if (it.contains("id") && it["id"].is_object()) {
                            video_id = it["id"].value("videoId", "");
                            if (!video_id.empty()) mode = "latest_completed";
                        }
                    }
                }
                catch (...) {}
            }
        }

        if (video_id.empty()) {
            res.status = 404;
            res.set_content(R"({"ok":false,"error":"no_target_video_found"})", "application/json; charset=utf-8");
            return;
        }

        // Fetch current categoryId (required for videos.update snippet)
        std::string category_id;
        if (YouTubeApiGet(("/youtube/v3/videos?part=snippet&id=" + video_id),
            access_token, &st, &body) && st == 200) {
            try {
                auto jv = nlohmann::json::parse(body);
                if (jv.contains("items") && jv["items"].is_array() && !jv["items"].empty()) {
                    category_id = jv["items"][0]["snippet"].value("categoryId", "");
                }
            }
            catch (...) {}
        }

        if (category_id.empty()) {
            res.status = 500;
            res.set_content(R"({"ok":false,"error":"missing_category_id"})", "application/json; charset=utf-8");
            return;
        }

        // Update video snippet
        nlohmann::json upd;
        upd["id"] = video_id;
        upd["snippet"] = {
            {"title", new_title},
            {"description", new_desc},
            {"categoryId", category_id}
        };

        const std::string upd_body = upd.dump();

        if (!YouTubeApiPutJson("/youtube/v3/videos?part=snippet", access_token, upd_body, &st, &body) || st < 200 || st >= 300) {
            nlohmann::json out = { {"ok", false}, {"error", "update_failed"}, {"http_status", st}, {"body", body} };
            res.status = 502;
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }

        nlohmann::json out = { {"ok", true}, {"video_id", video_id}, {"mode", mode} };
        res.status = 200;
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        });


    // --- Twitch OAuth (interactive) ---
    // Enables one-time auth to obtain a refresh token with additional scopes (e.g. chat:read/chat:edit).
    // Requires the main app to wire opt_.twitch_auth_build_authorize_url and opt_.twitch_auth_handle_callback.
    //
    // Start here:
    //   http://localhost:17845/auth/twitch/start
    // Callback (handled automatically by browser):
    //   http://localhost:17845/auth/twitch/callback
    
    // --- API: Twitch OAuth info (for Settings UI) ---
    // GET /api/twitch/auth/info
    svr.Get("/api/twitch/auth/info", [&](const httplib::Request& /*req*/, httplib::Response& res) {
        nlohmann::json j;
        j["ok"] = true;
        j["start_url"] = "/auth/twitch/start";
        j["oauth_routes_wired"] = (bool)opt_.twitch_auth_build_authorize_url && (bool)opt_.twitch_auth_handle_callback;
        j["scopes_readable"] = std::string(TwitchAuth::RequiredScopeReadable());
        j["scopes_encoded"] = std::string(TwitchAuth::RequiredScopeEncoded());
        res.status = 200;
        res.set_content(j.dump(2), "application/json; charset=utf-8");
    });

svr.Get("/auth/twitch/start", [&](const httplib::Request& req, httplib::Response& res) {
        if (!opt_.twitch_auth_build_authorize_url) {
            SafeOutputLog(log_, L"HTTP: Twitch OAuth routes NOT enabled (callbacks not wired)");
            res.status = 404;
            res.set_content("not wired", "text/plain; charset=utf-8");
            return;
        }

        // Canonical redirect URI: always use localhost to match Twitch dev console exactly.
        const std::string redirect_uri =
            std::string("http://localhost:") + std::to_string(opt_.port) + "/auth/twitch/callback";

        std::string err;
        const std::string url = opt_.twitch_auth_build_authorize_url(redirect_uri, &err);
        if (url.empty()) {
            SafeOutputLog(log_, L"HTTP: /auth/twitch/start failed to build authorize URL");
            res.status = 500;
            res.set_content(std::string("BuildAuthorizeUrl failed: ") + err, "text/plain; charset=utf-8");
            return;
        }

        // Redirect the browser to Twitch.
        res.status = 302;
        res.set_header("Location", url);
    });

    svr.Get("/auth/twitch/callback", [&](const httplib::Request& req, httplib::Response& res) {
        if (!opt_.twitch_auth_handle_callback) {
            SafeOutputLog(log_, L"HTTP: Twitch OAuth routes NOT enabled (callbacks not wired)");
            res.status = 404;
            res.set_content("not wired", "text/plain; charset=utf-8");
            return;
        }

        const std::string code = req.get_param_value("code");
        const std::string state = req.get_param_value("state");

        std::string host = req.get_header_value("Host");
        if (host.empty()) host = "localhost:" + std::to_string(opt_.port);
        const std::string redirect_uri = std::string("http://") + host + "/auth/twitch/callback";

        std::string err;
        const bool ok = opt_.twitch_auth_handle_callback(code, state, redirect_uri, &err);
        if (!ok) {
            SafeOutputLog(log_, L"HTTP: /auth/twitch/callback token exchange failed");
            res.status = 500;
            res.set_content(std::string("OAuth callback failed: ") + err, "text/plain; charset=utf-8");
            return;
        }

        res.set_content("OK - Twitch auth completed. You can close this tab.", "text/plain; charset=utf-8");
    });

    // --- API: YouTube OAuth info (for Settings UI) ---
    // GET /api/youtube/auth/info
    svr.Get("/api/youtube/auth/info", [&](const httplib::Request& /*req*/, httplib::Response& res) {
        // If main app provided a detailed status JSON, prefer that.
        if (opt_.youtube_auth_info_json) {
            res.status = 200;
            res.set_content(opt_.youtube_auth_info_json(), "application/json; charset=utf-8");
            return;
        }

        // Fallback: basic wiring/scopes/start URL only.
        nlohmann::json j;
        j["ok"] = true;
        j["start_url"] = "/auth/youtube/start";
        j["oauth_routes_wired"] = (bool)opt_.youtube_auth_build_authorize_url && (bool)opt_.youtube_auth_handle_callback;
        j["scopes_readable"] = std::string(YouTubeAuth::RequiredScopeReadable());
        j["scopes_encoded"] = std::string(YouTubeAuth::RequiredScopeEncoded());
        res.status = 200;
        res.set_content(j.dump(2), "application/json; charset=utf-8");
    });

    // --- YouTube OAuth (interactive) ---
    // Start:
    //   http://localhost:17845/auth/youtube/start
    // Callback:
    //   http://localhost:17845/auth/youtube/callback
    svr.Get("/auth/youtube/start", [&](const httplib::Request& /*req*/, httplib::Response& res) {
        if (!opt_.youtube_auth_build_authorize_url) {
            SafeOutputLog(log_, L"HTTP: YouTube OAuth routes NOT enabled (callbacks not wired)");
            res.status = 404;
            res.set_content("not wired", "text/plain; charset=utf-8");
            return;
        }

        // Canonical redirect URI: localhost + port.
        const std::string redirect_uri =
            std::string("http://localhost:") + std::to_string(opt_.port) + "/auth/youtube/callback";

        std::string err;
        const std::string url = opt_.youtube_auth_build_authorize_url(redirect_uri, &err);
        if (url.empty()) {
            SafeOutputLog(log_, L"HTTP: /auth/youtube/start failed to build authorize URL");
            res.status = 500;
            res.set_content(std::string("BuildAuthorizeUrl failed: ") + err, "text/plain; charset=utf-8");
            return;
        }

        res.status = 302;
        res.set_header("Location", url);
    });

    svr.Get("/auth/youtube/callback", [&](const httplib::Request& req, httplib::Response& res) {
        if (!opt_.youtube_auth_handle_callback) {
            SafeOutputLog(log_, L"HTTP: YouTube OAuth routes NOT enabled (callbacks not wired)");
            res.status = 404;
            res.set_content("not wired", "text/plain; charset=utf-8");
            return;
        }

        const std::string code = req.get_param_value("code");
        const std::string state = req.get_param_value("state");

        std::string host = req.get_header_value("Host");
        if (host.empty()) host = "localhost:" + std::to_string(opt_.port);
        const std::string redirect_uri = std::string("http://") + host + "/auth/youtube/callback";

        std::string err;
        const bool ok = opt_.youtube_auth_handle_callback(code, state, redirect_uri, &err);
        if (!ok) {
            SafeOutputLog(log_, L"HTTP: /auth/youtube/callback token exchange failed");
            res.status = 500;
            res.set_content(std::string("OAuth callback failed: ") + err, "text/plain; charset=utf-8");
            return;
        }

        res.set_content("OK - YouTube auth completed. You can close this tab.", "text/plain; charset=utf-8");
    });


    // --- API: settings save (used by /app UI) ---
    // Accept both legacy and newer paths.
    auto handle_settings_save = [&](const httplib::Request& req, httplib::Response& res) {
        // If the UI sends a JSON body, apply it to config_ before saving.
        if (!req.body.empty()) {
            try {
                auto j = json::parse(req.body);

                if (j.contains("tiktok_unique_id"))      config_.tiktok_unique_id = j.value("tiktok_unique_id", config_.tiktok_unique_id);
                if (j.contains("twitch_login"))          config_.twitch_login = j.value("twitch_login", config_.twitch_login);
                if (j.contains("twitch_client_id"))      config_.twitch_client_id = j.value("twitch_client_id", config_.twitch_client_id);
                if (j.contains("twitch_client_secret"))  config_.twitch_client_secret = j.value("twitch_client_secret", config_.twitch_client_secret);
                if (j.contains("youtube_handle"))        config_.youtube_handle = j.value("youtube_handle", config_.youtube_handle);

                // TikTok cookie/session fields (optional)
                if (j.contains("tiktok_sessionid"))      config_.tiktok_sessionid = j.value("tiktok_sessionid", config_.tiktok_sessionid);
                if (j.contains("tiktok_sessionid_ss"))   config_.tiktok_sessionid_ss = j.value("tiktok_sessionid_ss", config_.tiktok_sessionid_ss);
                if (j.contains("tiktok_tt_target_idc"))  config_.tiktok_tt_target_idc = j.value("tiktok_tt_target_idc", config_.tiktok_tt_target_idc);

                // Overlay styling fields (optional)
                if (j.contains("overlay_font_family"))   config_.overlay_font_family = j.value("overlay_font_family", config_.overlay_font_family);
                if (j.contains("overlay_font_size"))     config_.overlay_font_size = j.value("overlay_font_size", config_.overlay_font_size);
                if (j.contains("overlay_text_shadow"))   config_.overlay_text_shadow = j.value("overlay_text_shadow", config_.overlay_text_shadow);

                if (j.contains("metrics_json_path"))     config_.metrics_json_path = j.value("metrics_json_path", config_.metrics_json_path);
            }
            catch (...) {
                res.status = 400;
                res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
                return;
            }
        }

        const std::wstring cfg_path_w = AppConfig::ConfigPath();
        const std::string  cfg_path = WideToUtf8(cfg_path_w);

        if (!config_.Save()) {
            SafeOutputLog(log_, L"settingssave: FAILED writing " + cfg_path_w);
            res.status = 500;
            json out;
            out["ok"] = false;
            out["error"] = "save_failed";
            out["path"] = cfg_path;
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }

        SafeOutputLog(log_, L"settingssave: wrote " + cfg_path_w);

        json out;
        out["ok"] = true;
        out["path"] = cfg_path;
        res.set_header("X-Config-Path", cfg_path.c_str());
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        };

    svr.Post("/api/settingssave", handle_settings_save);
    svr.Post("/api/settings/save", handle_settings_save);

    // --- API: settings (read) ---
    // The /app UI calls this to populate the username fields on load.
    // Keep the payload intentionally small (do not expose secrets by default).
    svr.Get("/api/settings", [&](const httplib::Request&, httplib::Response& res) {
        const std::wstring cfg_path_w = AppConfig::ConfigPath();
        const std::string  cfg_path = WideToUtf8(cfg_path_w);

        json out;
        out["ok"] = true;
        out["config_path"] = cfg_path;

        // Username / channel identifiers (safe to expose)
        out["tiktok_unique_id"] = config_.tiktok_unique_id;
        out["twitch_login"] = config_.twitch_login;
        out["youtube_handle"] = config_.youtube_handle;

        res.set_header("X-Config-Path", cfg_path.c_str());
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        });

    // --- API: EuroScope ingest ---

        // EuroScope plugin ingest endpoint (expects JSON with ts_ms)
    svr.Post("/api/euroscope", [&](const httplib::Request& req, httplib::Response& res) {
        std::string err;
        if (!euroscope_.Ingest(req.body, err)) {
            res.status = 400;
            res.set_content(std::string(R"({"ok":false,"error":")") + err + R"("})",
                "application/json; charset=utf-8");
            return;
        }
        res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
        });

    
    // EuroScope traffic snapshot (derived from last ingest)
    // Returns the "euroscope" object from /api/metrics as a standalone payload.
    svr.Get("/api/euroscope/traffic", [&](const httplib::Request&, httplib::Response& res) {
        const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        nlohmann::json j = euroscope_.Metrics(now_ms);
        if (!j.is_object() || !j.contains("euroscope")) {
            res.set_content(R"({"ts_ms":0,"error":"no euroscope data"})", "application/json; charset=utf-8");
            return;
        }

        res.set_content(j["euroscope"].dump(2), "application/json; charset=utf-8");
        });
// --- API: chat (stable shape) ---
    auto handle_chat_recent = [&](const httplib::Request& req, httplib::Response& res) {
        int limit = 200;
        if (req.has_param("limit")) {
            try { limit = std::max(1, std::min(1000, std::stoi(req.get_param_value("limit")))); }
            catch (...) {}
        }

        // Prefer aggregator as source of truth
        json msgs = chat_.RecentJson(limit);

        // If aggregator is empty (adapters write into AppState), fall back to AppState chat JSON.
        if (!msgs.is_array() || msgs.empty()) {
            try {
                json s = state_.chat_json();
                // s is an array of chat messages (oldest->newest). Return only the last `limit` messages.
                if (s.is_array()) {
                    if ((int)s.size() > limit) {
                        json slice = json::array();
                        for (size_t i = s.size() - limit; i < s.size(); ++i) slice.push_back(s[i]);
                        msgs = std::move(slice);
                    }
                    else {
                        msgs = std::move(s);
                    }
                }
            }
            catch (...) {
                // ignore and keep msgs as-is
            }
        }

        json out;
        out["ts_ms"] = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        out["messages"] = std::move(msgs);

        res.set_content(out.dump(2), "application/json; charset=utf-8");
        };

    svr.Get("/api/chat/recent", handle_chat_recent);
    svr.Get("/api/chat", handle_chat_recent);

    // --- Safety: restrict bot mutation routes to localhost only ---
    // Even if the server is later bound to 0.0.0.0 or exposed via tunnels/port-forwarding,
    // we do NOT want remote clients to be able to modify bot commands/settings or inject test chat.
    auto is_local_request = [](const httplib::Request& req) -> bool {
        // cpp-httplib provides the peer address in remote_addr.
        // Accept IPv4 loopback and IPv6 loopback.
        if (req.remote_addr == "127.0.0.1" || req.remote_addr == "::1") return true;
        // Some builds may report other 127/8 loopback values; allow them too.
        if (req.remote_addr.rfind("127.", 0) == 0) return true;
        return false;
    };

    auto require_local = [&](const httplib::Request& req, httplib::Response& res) -> bool {
        if (is_local_request(req)) return true;
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"forbidden"})", "application/json; charset=utf-8");
        return false;
    };

    // --- API: bot commands ---
    // GET  /api/bot/commands  -> current command list
    // POST /api/bot/commands  -> replace command list
    // POST /api/bot/commands/upsert -> add/update a single command
    // DELETE /api/bot/commands/<command> -> delete a single command
    // DELETE /api/bot/commands?command=<command> -> delete a single command (fallback)
    // Body can be either: {"commands":[...]} or a raw array [...]
    svr.Get("/api/bot/commands", [&](const httplib::Request&, httplib::Response& res) {
        json out;
        out["ts_ms"] = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out["commands"] = state_.bot_commands_json();
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
    });

    // --- API: bot settings ---
    // GET  /api/bot/settings  -> current safety settings (loaded from bot_settings.json)
    // POST /api/bot/settings  -> replace/update settings
    svr.Get("/api/bot/settings", [&](const httplib::Request&, httplib::Response& res) {
        json out;
        out["ts_ms"] = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out["settings"] = state_.bot_settings_json();
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
    });

    svr.Post("/api/bot/settings", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_local(req, res)) return;
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        json settings = body;
        if (body.is_object() && body.contains("settings")) settings = body["settings"];

        std::string err;
        if (!state_.set_bot_settings(settings, &err)) {
            res.status = 400;
            json out;
            out["ok"] = false;
            out["error"] = err.empty() ? "invalid_settings" : err;
            out["settings"] = state_.bot_settings_json();
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }

        json out;
        out["ok"] = true;
        out["settings"] = state_.bot_settings_json();
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
    });

    svr.Post("/api/bot/commands", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_local(req, res)) return;
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        json commands = body;
        if (body.is_object() && body.contains("commands")) commands = body["commands"];

        state_.set_bot_commands(commands);

        json out;
        out["ok"] = true;
        out["commands"] = state_.bot_commands_json();
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
    });

    auto url_decode = [](const std::string& in) -> std::string {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            char c = in[i];
            if (c == '%' && i + 2 < in.size()) {
                auto hex = [](char h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
                    if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
                    return -1;
                };
                int hi = hex(in[i + 1]);
                int lo = hex(in[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back((char)((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            if (c == '+') { out.push_back(' '); continue; }
            out.push_back(c);
        }
        return out;
    };

    // --- API: overlay header ---
// GET  /api/overlay/header -> {"ok":true,"title":"...","subtitle":"..."}
// POST /api/overlay/header -> set {"title":"...","subtitle":"..."}
    svr.Get("/api/overlay/header", [&](const httplib::Request&, httplib::Response& res) {
        nlohmann::json out;
        out["ok"] = true;
        out["header"] = state_.overlay_header_json();

        // Also provide flat fields for convenience
        out["title"] = out["header"].value("title", "");
        out["subtitle"] = out["header"].value("subtitle", "");

        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        });

    svr.Post("/api/overlay/header", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_local(req, res)) return;

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        // Accept either {"title":"..","subtitle":".."} OR {"header":{...}}
        nlohmann::json header = body;
        if (body.is_object() && body.contains("header")) header = body["header"];

        std::string err;
        if (!state_.set_overlay_header(header, &err)) {
            res.status = 400;
            nlohmann::json out;
            out["ok"] = false;
            out["error"] = err.empty() ? "invalid_header" : err;
            out["header"] = state_.overlay_header_json();
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }

        nlohmann::json out;
        out["ok"] = true;
        out["header"] = state_.overlay_header_json();
        out["title"] = out["header"].value("title", "");
        out["subtitle"] = out["header"].value("subtitle", "");

        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        });

    // Upsert a single command without replacing the full list.
    // Body: {"command":"help","response":"...","enabled":true,"cooldown_ms":3000,"scope":"all"}
    svr.Post("/api/bot/commands/upsert", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_local(req, res)) return;
        json body;
        try {
            body = json::parse(req.body);
        }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        std::string err;
        if (!state_.bot_upsert_command(body, &err)) {
            res.status = 400;
            json out;
            out["ok"] = false;
            out["error"] = err.empty() ? "invalid_command" : err;
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }

        json out;
        out["ok"] = true;
        out["commands"] = state_.bot_commands_json();
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
    });

    // Delete a single command.
    // Supports both:
    //  - DELETE /api/bot/commands/<command>
    //  - DELETE /api/bot/commands?command=<command>
    auto handle_bot_delete = [&](const std::string& cmd_raw, httplib::Response& res) {
        const std::string cmd = url_decode(cmd_raw);
        bool removed = state_.bot_delete_command(cmd);

        json out;
        out["ok"] = removed;
        out["removed"] = removed;
        out["command"] = cmd;
        out["commands"] = state_.bot_commands_json();
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
    };

    svr.Delete(R"(/api/bot/commands/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_local(req, res)) return;
        if (req.matches.size() >= 2) {
            handle_bot_delete(req.matches[1].str(), res);
        }
        else {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"missing_command"})", "application/json; charset=utf-8");
        }
    });

    svr.Delete("/api/bot/commands", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_local(req, res)) return;
        if (!req.has_param("command")) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"missing_command"})", "application/json; charset=utf-8");
            return;
        }
        handle_bot_delete(req.get_param_value("command"), res);
    });

    // --- API: bot test inject ---
    // POST /api/bot/test
    // Body: {"platform":"twitch","user":"TestUser","message":"!help"}
    // Injects ONLY the user message into ChatAggregator so it follows the real chat flow.
    // The normal bot pipeline (already in your app) should create the single bot reply.
    svr.Post("/api/bot/test", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_local(req, res)) return;
        json body;
        try {
            body = json::parse(req.body);
        }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        std::string platform = body.value("platform", "test");
        std::string user = body.value("user", "TestUser");
        std::string message = body.value("message", "");

        auto now_ms_ll = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        auto to_lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            return s;
            };

        auto replace_all = [](std::string s, const std::string& from, const std::string& to) {
            if (from.empty()) return s;
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
            };

        // Prepare response JSON (best-effort preview of what the bot WILL say)
        json out;
        out["ok"] = true;
        out["ts_ms"] = now_ms_ll;

        // Determine if it looks like a command and (optionally) preview reply from current state_
        std::string cmd_lc;
        if (!message.empty() && message.size() >= 2 && message[0] == '!') {
            size_t start = 1;
            while (start < message.size() && std::isspace((unsigned char)message[start])) start++;
            size_t end = start;
            while (end < message.size() && !std::isspace((unsigned char)message[end])) end++;
            if (end > start) cmd_lc = to_lower(message.substr(start, end - start));
        }

        if (cmd_lc.empty()) {
            out["matched"] = false;
            out["note"] = "not_a_command";
        }
        else {
            // For test endpoint, allow role simulation via request body
            bool is_mod = body.value("is_mod", false);
            bool is_broadcaster = body.value("is_broadcaster", false);

            const AppState::BotSettings bs = state_.bot_settings_snapshot();
            if (bs.silent_mode) {
                out["silent_mode"] = true;
            }

            // Rule-aware lookup (enforces enabled/cooldown/scope).
            // Returns empty string if blocked or no match.
            // Preview only: do NOT consume cooldown timers.
            std::string template_reply = state_.bot_peek_response(cmd_lc, is_mod, is_broadcaster, now_ms_ll);
            if (template_reply.empty()) {
                out["matched"] = false;
                out["command"] = cmd_lc;
                out["note"] = "blocked_or_no_match";
            }
            else {
                std::string reply = template_reply;
                reply = replace_all(reply, "{user}", user);
                reply = replace_all(reply, "{platform}", to_lower(platform));

                if (bs.max_reply_len == 0) {
                    out["matched"] = false;
                    out["command"] = cmd_lc;
                    out["note"] = bs.silent_mode ? "silent_mode" : "max_reply_len_zero";
                }
                else {
                    if (bs.max_reply_len > 0 && reply.size() > bs.max_reply_len) {
                        reply.resize(bs.max_reply_len);
                    }
                out["matched"] = true;
                out["command"] = cmd_lc;
                out["reply"] = reply;
                out["note"] = bs.silent_mode ? "silent_mode_reply_preview" : "reply_preview_only"; // actual injection happens via normal pipeline
                }
            }
        }

        // Inject ONLY the user message (this should trigger your existing bot logic exactly once)
        if (!message.empty()) {
            ChatMessage m{};
            m.platform = platform;
            m.user = user;
            m.message = message;
            m.ts_ms = now_ms_ll;
            chat_.Add(std::move(m));
        }

        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        });

    // --- API: chat diagnostics ---
    // Returns address of the ChatAggregator instance and current buffered count.
    svr.Get("/api/chat/diag", [&](const httplib::Request&, httplib::Response& res) {
        json out;
        out["chat_ptr"] = (uint64_t)(uintptr_t)(&chat_);
        out["count"] = (long long)chat_.Size();
        // Include AppState chat count as well (some adapters write into AppState)
        try {
            auto v = state_.recent_chat();
            out["state_count"] = (long long)v.size();
        }
        catch (...) {
            out["state_count"] = 0;
        }
        out["ts_ms"] = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        });

    // --- API: chat test inject (debug) ---
// Example:
//   /api/chat/test?platform=twitch&user=Test&message=Hello
    svr.Get("/api/chat/test", [&](const httplib::Request& req, httplib::Response& res) {
        ChatMessage m{};
        m.platform = req.has_param("platform") ? req.get_param_value("platform") : "test";
        m.user = req.has_param("user") ? req.get_param_value("user") : "Test";
        m.message = req.has_param("message") ? req.get_param_value("message") : "Hello";

        m.ts_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // Add into aggregator for overlays that read it
        chat_.Add(m);
        // Backward AppState sink removed: ChatAggregator is now the source-of-truth for overlays.

        res.set_content(R"({"ok":true})", "application/json; charset=utf-8");
        });

    // --- API: TikTok events ---
    svr.Get("/api/tiktok/events", [&](const httplib::Request& req, httplib::Response& res) {
        int limit = 200;
        if (req.has_param("limit")) {
            try {
                limit = std::max(1, std::min(1000, std::stoi(req.get_param_value("limit"))));
            }
            catch (...) {
                // keep default
            }
        }

        auto j = state_.tiktok_events_json(static_cast<size_t>(limit));
        res.set_content(j.dump(2), "application/json; charset=utf-8");
        });

    // --- API: YouTube events ---
    svr.Get("/api/youtube/events", [&](const httplib::Request& req, httplib::Response& res) {
        int limit = 200;
        if (req.has_param("limit")) {
            try { limit = std::max(1, std::min(1000, std::stoi(req.get_param_value("limit")))); }
            catch (...) {}
        }

        json out;
        out["ts_ms"] = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        out["events"] = state_.youtube_events_json((size_t)limit);

        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        });

    // --- Overlay: special chat.html injection ---
    svr.Get("/overlay/chat.html", [&](const httplib::Request&, httplib::Response& res) {
        std::filesystem::path htmlPath = opt_.overlay_root / "common" / "chat.html";
        auto html = ReadFileUtf8(htmlPath);
        if (html.empty()) {
            res.status = 404;
            res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
            res.set_header("Pragma", "no-cache");
            res.set_content("chat.html not found", "text/plain");
            return;
        }

        std::string fontFamily = Trim(config_.overlay_font_family);
        std::string fontStack = "sans-serif";
        std::string googleLink;

        if (!fontFamily.empty()) {
            fontStack = "'" + fontFamily + "', sans-serif";
            // Optional Google Fonts link injection if you use placeholders
            googleLink = "<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n"
                "<link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>\n"
                "<link href=\"https://fonts.googleapis.com/css2?family=" + fontFamily + ":wght@300;400;600;700&display=swap\" rel=\"stylesheet\">\n";
        }

        std::string shadow = config_.overlay_text_shadow ? "0 2px 10px rgba(0,0,0,.55)" : "none";
        ApplyOverlayTokens(html);

        res.set_content(html, "text/html; charset=utf-8");
        });

    // /overlay or /overlay/ -> default (chat)
    svr.Get("/overlay", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/overlay/chat.html");
        });
    svr.Get("/overlay/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/overlay/chat.html");
        });

    // Serve /overlay/<anything> from overlay root (except chat.html special case)
    svr.Get(R"(/overlay/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string rel = req.matches[1].str();
        if (rel.empty()) rel = "index.html";

        // alias for convenience
        if (rel == "chat.html") {
            res.set_redirect("/overlay/chat.html");
            return;
        }

        if (rel.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("bad path", "text/plain");
            return;
        }

        std::filesystem::path p = opt_.overlay_root / rel;
        if (!std::filesystem::exists(p) || std::filesystem::is_directory(p)) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }

        auto bytes = ReadFileUtf8(p);
        // Apply overlay token substitution for HTML files
        if (rel.size() >= 5 && rel.substr(rel.size() - 5) == ".html") {
            ApplyOverlayTokens(bytes);
        }
        res.set_content(std::move(bytes), ContentTypeFor(rel));
        });

    // --- Static assets: /assets/... ---
    // Your floating chat lives at /assets/overlay/chat.html.
    // Map this URL space to the on-disk "assets" directory next to the executable.
    // We can infer the assets root from overlay_root: <exe_dir>/assets/overlay -> <exe_dir>/assets
    const std::filesystem::path assetsRoot = opt_.overlay_root.parent_path();

    // --- Modern App UI: /app and /app/* ---
    // Serves files from <assetsRoot>/app (e.g. assets/app/index.html)
    svr.Get("/app", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/app/index.html");
        });
    svr.Get("/app/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/app/index.html");
        });

    svr.Get(R"(/app/(.*))", [&, assetsRoot](const httplib::Request& req, httplib::Response& res) {
        std::string rel = req.matches[1].str();
        if (rel.empty() || rel == "/") rel = "index.html";

        if (rel.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("bad path", "text/plain");
            return;
        }

        std::filesystem::path p = assetsRoot / "app" / rel;
        if (!std::filesystem::exists(p) || std::filesystem::is_directory(p)) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }

        auto bytes = ReadFileUtf8(p);
        if (rel.size() >= 5 && rel.substr(rel.size() - 5) == ".html") {
            // Allow token substitution for app pages too.
            ApplyOverlayTokens(bytes);
        }
        res.set_content(std::move(bytes), ContentTypeFor(rel));
        });

    svr.Get(R"(/assets/(.*))", [&, assetsRoot](const httplib::Request& req, httplib::Response& res) {
        std::string rel = req.matches[1].str();
        if (rel.empty()) rel = "index.html";

        if (rel.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("bad path", "text/plain");
            return;
        }

        std::filesystem::path p = assetsRoot / rel;
        if (!std::filesystem::exists(p) || std::filesystem::is_directory(p)) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }

        auto bytes = ReadFileUtf8(p);
        if (rel.size() >= 5 && rel.substr(rel.size() - 5) == ".html") {
            // Reuse the same token logic for app pages (lets you apply font/shadow tokens if desired).
            ApplyOverlayTokens(bytes);
        }
        res.set_content(std::move(bytes), ContentTypeFor(rel));
        });


    // --- API: platform control (start/stop) ---
    // Called by the modern Web UI (e.g. "Start All" button)
    // POST /api/platform/{tiktok|twitch|youtube}/{start|stop}
    svr.Post(R"(/api/platform/(tiktok|twitch|youtube)/(start|stop))",
        [&](const httplib::Request& req, httplib::Response& res) {

            const std::string platform = req.matches[1];
            const std::string action = req.matches[2];

            std::function<bool(void)> fn;

            if (platform == "tiktok") {
                fn = (action == "start") ? opt_.start_tiktok : opt_.stop_tiktok;
            }
            else if (platform == "twitch") {
                fn = (action == "start") ? opt_.start_twitch : opt_.stop_twitch;
            }
            else if (platform == "youtube") {
                fn = (action == "start") ? opt_.start_youtube : opt_.stop_youtube;
            }

            if (!fn) {
                res.status = 404;
                res.set_content(R"({"ok":false,"error":"not_implemented"})", "application/json");
                return;
            }

            bool ok = false;
            try {
                ok = fn();
            }
            catch (...) {
                res.status = 500;
                res.set_content(R"({"ok":false,"error":"exception"})", "application/json");
                return;
            }

            if (!ok) {
                res.status = 500;
                res.set_content(R"({"ok":false,"error":"failed"})", "application/json");
                return;
            }

            const std::string state = (action == "start") ? "started" : "stopped";
            std::string body = std::string(R"({"ok":true,"platform":")") + platform +
                R"(","action":")" + action +
                R"(","state":")" + state +
                R"("})";

            res.set_content(body, "application/json");
        });

    // Root -> overlay
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/overlay/");
        });
}