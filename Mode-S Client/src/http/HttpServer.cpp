#include "HttpServer.h"

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
#include "../AppConfig.h"

using json = nlohmann::json;

static std::string Trim(const std::string& s) {
    auto is_space = [](unsigned char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; };
    size_t a = 0, b = s.size();
    while (a < b && is_space((unsigned char)s[a])) a++;
    while (b > a && is_space((unsigned char)s[b-1])) b--;
    return s.substr(a, b-a);
}

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
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

void HttpServer::RegisterRoutes() {
    auto& svr = *svr_;

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

        json out;
        out["ts_ms"] = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        out["messages"] = std::move(msgs);

        res.set_content(out.dump(2), "application/json; charset=utf-8");
    };

    svr.Get("/api/chat/recent", handle_chat_recent);
    svr.Get("/api/chat", handle_chat_recent);

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

        ReplaceAll(html, "%%GOOGLE_FONTS_LINK%%", googleLink);
        ReplaceAll(html, "%%FONT_FAMILY%%", fontStack);
        ReplaceAll(html, "%%FONT_SIZE%%", std::to_string(config_.overlay_font_size));
        ReplaceAll(html, "%%TEXT_SHADOW%%", shadow);

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
        res.set_content(std::move(bytes), ContentTypeFor(rel));
    });

    // Root -> overlay
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/overlay/");
    });
}
