#include "HttpServer.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <cstring>

#include "json.hpp"
#include "../AppState.h"
#include "../chat/ChatAggregator.h"
#include "../../integrations/euroscope/EuroScopeIngestService.h"
#include "../AppConfig.h"

using json = nlohmann::json;

static std::string Trim(const std::string& s) {
    auto is_space = [](unsigned char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; };
    size_t a = 0, b = s.size();
    while (a < b && is_space((unsigned char)s[a])) a++;
    while (b > a && is_space((unsigned char)s[b-1])) b--;
    return s.substr(a, b-a);
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
      log_(std::move(log)) {}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::Start() {
    if (svr_) return;

    svr_ = std::make_unique<httplib::Server>();
    RegisterRoutes();

    thread_ = std::thread([this]() {
        try {
            if (log_) {
                log_(L"HTTP: listening on http://127.0.0.1:" + std::to_wstring(opt_.port));
            }
            svr_->listen(opt_.bind_host.c_str(), opt_.port);
        } catch (...) {
            if (log_) log_(L"HTTP: server thread crashed");
        }
    });
}

void HttpServer::Stop() {
    if (!svr_) return;
    try {
        svr_->stop();
    } catch (...) {}

    // Join the server thread so the process exits cleanly.
    // (listen() will return after stop()).
    if (thread_.joinable()) {
        if (std::this_thread::get_id() == thread_.get_id()) {
            // Defensive: if Stop() is ever called from the server thread itself.
            thread_.detach();
        } else {
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

    (void)changed; // keep if you don't use it; helps debugging if you later add a loop
}

void HttpServer::RegisterRoutes() {
    auto& svr = *svr_;

    // Safe logger: never throws into the HTTP thread (prevents std::bad_function_call / other exceptions from bubbling).
    auto SafeLog = [&](const std::wstring& msg) {
        try {
            if (log_) log_(msg);
        } catch (...) {
            // logging must never crash request handling
        }
    };


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


// --- API: settings save (used by /app UI) ---
// Accept both legacy and newer paths.
auto handle_settings_save = [&](const httplib::Request& req, httplib::Response& res) {
    // If the UI sends a JSON body, apply it to config_ before saving.
    if (!req.body.empty()) {
        try {
            auto j = json::parse(req.body);

            if (j.contains("tiktok_unique_id"))      config_.tiktok_unique_id      = j.value("tiktok_unique_id", config_.tiktok_unique_id);
            if (j.contains("twitch_login"))          config_.twitch_login          = j.value("twitch_login", config_.twitch_login);
            if (j.contains("twitch_client_id"))      config_.twitch_client_id      = j.value("twitch_client_id", config_.twitch_client_id);
            if (j.contains("twitch_client_secret"))  config_.twitch_client_secret  = j.value("twitch_client_secret", config_.twitch_client_secret);
            if (j.contains("youtube_handle"))        config_.youtube_handle        = j.value("youtube_handle", config_.youtube_handle);

            // TikTok cookie/session fields (optional)
            if (j.contains("tiktok_sessionid"))      config_.tiktok_sessionid      = j.value("tiktok_sessionid", config_.tiktok_sessionid);
            if (j.contains("tiktok_sessionid_ss"))   config_.tiktok_sessionid_ss   = j.value("tiktok_sessionid_ss", config_.tiktok_sessionid_ss);
            if (j.contains("tiktok_tt_target_idc"))  config_.tiktok_tt_target_idc  = j.value("tiktok_tt_target_idc", config_.tiktok_tt_target_idc);

            // Overlay styling fields (optional)
            if (j.contains("overlay_font_family"))   config_.overlay_font_family   = j.value("overlay_font_family", config_.overlay_font_family);
            if (j.contains("overlay_font_size"))     config_.overlay_font_size     = j.value("overlay_font_size", config_.overlay_font_size);
            if (j.contains("overlay_text_shadow"))   config_.overlay_text_shadow   = j.value("overlay_text_shadow", config_.overlay_text_shadow);

            if (j.contains("metrics_json_path"))     config_.metrics_json_path     = j.value("metrics_json_path", config_.metrics_json_path);
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
            return;
        }
    }

    const std::wstring cfg_path_w = AppConfig::ConfigPath();
    const std::string  cfg_path   = WideToUtf8(cfg_path_w);

    if (!config_.Save()) {
        if (log_) {
            SafeLog(L"settingssave: FAILED writing " + cfg_path_w);
        }
        res.status = 500;
        json out;
        out["ok"] = false;
        out["error"] = "save_failed";
        out["path"] = cfg_path;
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        return;
    }

    if (log_) {
        SafeLog(L"settingssave: wrote " + cfg_path_w);
    }

    json out;
    out["ok"] = true;
    out["path"] = cfg_path;
    res.set_header("X-Config-Path", cfg_path.c_str());
    res.set_content(out.dump(2), "application/json; charset=utf-8");
};

svr.Post("/api/settingssave", handle_settings_save);
svr.Post("/api/settings/save", handle_settings_save);
    // Read current settings/config for the Web UI
    svr.Get("/api/settings", [&](const httplib::Request& /*req*/, httplib::Response& res) {
        json j = json::object();
        j["ok"] = true;

        // Primary identifiers shown/edited in the UI
        j["tiktok_unique_id"] = config_.tiktok_unique_id;
        j["twitch_login"]     = config_.twitch_login;
        j["youtube_handle"]   = config_.youtube_handle;

        // Optional fields used elsewhere
        j["metrics_json_path"]      = config_.metrics_json_path;
        j["overlay_font_family"]    = config_.overlay_font_family;
        j["overlay_font_size"]      = config_.overlay_font_size;
        j["overlay_text_shadow"]    = config_.overlay_text_shadow;

        // Helpful for debugging where config is resolved to
        try {
            j["config_path"] = WideToUtf8(config_.ConfigPath());
        } catch (...) {
            j["config_path"] = "";
        }

        res.set_content(j.dump(), "application/json; charset=utf-8");
    });

    // --- API: platform control (used by /app UI) ---
    // These endpoints are optional; they call callbacks provided via HttpServer::Options.
    auto handle_platform = [&](const httplib::Request& /*req*/, httplib::Response& res,
                               const char* platform, const char* action,
                               const std::function<bool()>& fn) {
        if (!fn) {
            res.status = 404;
            json out;
            out["ok"] = false;
            out["error"] = "not_implemented";
            out["platform"] = platform;
            out["action"] = action;
            out["state"] = "not_implemented";
            res.set_content(out.dump(), "application/json; charset=utf-8");
            if (log_) {
                std::wstring wp = std::wstring(platform, platform + std::strlen(platform));
                std::wstring wa = std::wstring(action, action + std::strlen(action));
                SafeLog(L"/api/platform/" + wp + L"/" + wa + L": not implemented");
            }
            return;
        }

        bool ok = false;
        try {
            ok = fn();
        } catch (const std::exception& e) {
            ok = false;
            if (log_) {
                std::wstring wp(platform, platform + std::strlen(platform));
                std::wstring wa(action, action + std::strlen(action));
                // best-effort narrow->wide
                std::string what = e.what() ? e.what() : "";
                std::wstring wwhat(what.begin(), what.end());
                SafeLog(L"/api/platform/" + wp + L"/" + wa + L": exception: " + wwhat);
            }
        } catch (...) {
            ok = false;
            if (log_) {
                std::wstring wp(platform, platform + std::strlen(platform));
                std::wstring wa(action, action + std::strlen(action));
                SafeLog(L"/api/platform/" + wp + L"/" + wa + L": unknown exception");
            }
        }

        json out;
        out["ok"] = ok;
        out["platform"] = platform;
        out["action"] = action;
        out["state"] = ok ? (std::string(action) == "start" ? "started" : "stopped") : "failed";

        if (!ok) {
            res.status = 500;
            out["error"] = "failed";
        }

        res.set_content(out.dump(), "application/json; charset=utf-8");

        if (log_) {
            std::wstring wp = std::wstring(platform, platform + std::strlen(platform));
            std::wstring wa = std::wstring(action, action + std::strlen(action));
            SafeLog(L"/api/platform/" + wp + L"/" + wa + (ok ? L": ok" : L": failed"));
        }
    };

svr.Post("/api/platform/tiktok/start", [&](const httplib::Request& req, httplib::Response& res) {
        handle_platform(req, res, "tiktok", "start", opt_.start_tiktok);
    });
    svr.Post("/api/platform/tiktok/stop", [&](const httplib::Request& req, httplib::Response& res) {
        handle_platform(req, res, "tiktok", "stop", opt_.stop_tiktok);
    });

    svr.Post("/api/platform/twitch/start", [&](const httplib::Request& req, httplib::Response& res) {
        handle_platform(req, res, "twitch", "start", opt_.start_twitch);
    });
    svr.Post("/api/platform/twitch/stop", [&](const httplib::Request& req, httplib::Response& res) {
        handle_platform(req, res, "twitch", "stop", opt_.stop_twitch);
    });

    svr.Post("/api/platform/youtube/start", [&](const httplib::Request& req, httplib::Response& res) {
        handle_platform(req, res, "youtube", "start", opt_.start_youtube);
    });
    svr.Post("/api/platform/youtube/stop", [&](const httplib::Request& req, httplib::Response& res) {
        handle_platform(req, res, "youtube", "stop", opt_.stop_youtube);
    });






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
                    } else {
                        msgs = std::move(s);
                    }
                }
            } catch (...) {
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
    } catch (...) {
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

    // --- Overlay: special chat.html injection ---
    svr.Get("/overlay/chat.html", [&](const httplib::Request&, httplib::Response& res) {
        std::filesystem::path htmlPath = opt_.overlay_root / "common" / "chat.html";
        auto html = ReadFileUtf8(htmlPath);
        if (html.empty()) {
            res.status = 404;
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
    // Your floating chat lives at /assets/app/chat.html.
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

    // Root -> overlay
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/overlay/");
    });
}