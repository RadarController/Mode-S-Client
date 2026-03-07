#include "HttpServer.h"
#include "log/UiLog.h"
#include "core/StringUtil.h"
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
#include "../../integrations/simconnect/SimConnectWorker.h"
#include "../AppConfig.h"

namespace {

    inline void SafeOutputLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        if (!log) return;
        try {
            log(msg);
        }
        catch (...) {
            // Never allow logging to break server threads
        }
    }

    inline void HttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer] " + msg);
    }

    inline void TwitchHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][Twitch] " + msg);
    }

    inline void YouTubeHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][YouTube] " + msg);
    }

    inline void SettingsHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][Settings] " + msg);
    }

    inline void EuroScopeHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][EuroScope] " + msg);
    }

    inline void SecurityHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][Security] " + msg);
    }

    inline void BotHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][Bot] " + msg);
    }

    inline void OverlayHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][Overlay] " + msg);
    }

    inline void ChatHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][Chat] " + msg);
    }

    inline void PlatformHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][Platform] " + msg);
    }

} // namespace

    static std::int64_t NowUnixSeconds() {
        using namespace std::chrono;
        return (std::int64_t)duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    static std::string JsonGetStringPath(const nlohmann::json& j, std::initializer_list<const char*> path) {
        const nlohmann::json* cur = &j;
        for (const char* key : path) {
            if (!cur->is_object()) return "";
            auto it = cur->find(key);
            if (it == cur->end()) return "";
            cur = &(*it);
        }
        if (cur->is_string()) return cur->get<std::string>();
        if (cur->is_number_integer()) return std::to_string(cur->get<long long>());
        if (cur->is_number_float()) return std::to_string(cur->get<double>());
        return "";
    }

    static bool SimBriefFetchLatestJson(int pilot_id, long* out_status, std::string* out_body, std::string* out_error) {
        if (out_status) *out_status = 0;
        if (out_body) out_body->clear();
        if (out_error) out_error->clear();

        const std::string path = "/api/xml.fetcher.php?userid=" + std::to_string(pilot_id) + "&json=1";

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient cli("www.simbrief.com", 443);
#else
        // Fallback (no TLS). This may fail if SimBrief enforces HTTPS.
        httplib::Client cli("www.simbrief.com", 80);
#endif

        cli.set_follow_location(true);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);
        cli.set_write_timeout(10);

        auto res = cli.Get(path.c_str());
        if (!res) {
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
            if (out_error) *out_error = "openssl_not_enabled";
#else
            if (out_error) *out_error = "request_failed";
#endif
            return false;
        }

        if (out_status) *out_status = res->status;
        if (out_body) *out_body = res->body;
        if (res->status != 200) {
            if (out_error) *out_error = "http_" + std::to_string(res->status);
            return false;
        }

        return true;
    }
}


static double JsonGetDoublePath(const nlohmann::json& j, std::initializer_list<const char*> path, double fallback = 0.0, bool* out_ok = nullptr) {
    const nlohmann::json* cur = &j;
    for (auto* k : path) {
        if (!cur->is_object()) { if (out_ok) *out_ok = false; return fallback; }
        auto it = cur->find(k);
        if (it == cur->end()) { if (out_ok) *out_ok = false; return fallback; }
        cur = &(*it);
    }
    try {
        if (cur->is_number_float() || cur->is_number_integer()) {
            if (out_ok) *out_ok = true;
            return cur->get<double>();
        }
        if (cur->is_string()) {
            const std::string s = cur->get<std::string>();
            if (s.empty()) { if (out_ok) *out_ok = false; return fallback; }
            if (out_ok) *out_ok = true;
            return std::stod(s);
        }
    } catch (...) {}
    if (out_ok) *out_ok = false;
    return fallback;
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

    // Start SimBrief cache worker (safe even if it fails; endpoint will still respond).
    StartSimBriefWorker();

    // Start SimConnect worker (safe even if MSFS isn't running; it will keep retrying).
    StartSimConnectWorker();

    thread_ = std::thread([this]() {
        try {
            HttpLog(log_, L"Listening on http://" + ToW(opt_.bind_host) + L":" + std::to_wstring(opt_.port));
            const bool ok = svr_->listen(opt_.bind_host.c_str(), opt_.port);
            if (!ok) {
                HttpLog(log_, L"Listen returned false");
            }
        }
        catch (...) {
            HttpLog(log_, L"Server thread crashed");
        }
        });

void HttpServer::Stop() {
    if (!svr_) return;

    StopSimBriefWorker();
    StopSimConnectWorker();
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

void HttpServer::StartSimBriefWorker() {
    // Avoid double-start.
    if (simbrief_thread_.joinable()) return;

    simbrief_stop_.store(false);

    // Hard-coded initial pilot ID (Option B). Can be made configurable later.
    constexpr int kPilotId = 11686;
    constexpr int kRefreshSeconds = 10 * 60;

    // Seed cache with an empty object.
    {
        std::lock_guard<std::mutex> lk(simbrief_mu_);
        simbrief_cache_ = nlohmann::json::object();
        simbrief_error_.clear();
        simbrief_last_refresh_unix_ = 0;
    }

    simbrief_thread_ = std::thread([this]() {
        constexpr int kPilotId = 11686;
        constexpr int kRefreshSeconds = 10 * 60;

        auto do_refresh = [&]() {
            long status = 0;
            std::string body;
            std::string err;

            if (!SimBriefFetchLatestJson(kPilotId, &status, &body, &err)) {
                std::lock_guard<std::mutex> lk(simbrief_mu_);
                simbrief_error_ = err.empty() ? "fetch_failed" : err;
                simbrief_last_refresh_unix_ = NowUnixSeconds();
                return;
            }

            nlohmann::json raw;
            try {
                raw = nlohmann::json::parse(body);
            }
            catch (...) {
                std::lock_guard<std::mutex> lk(simbrief_mu_);
                simbrief_error_ = "invalid_json";
                simbrief_last_refresh_unix_ = NowUnixSeconds();
                return;
            }

            // Extract the fields we care about, with tolerant fallbacks.
            std::string callsign = JsonGetStringPath(raw, { "atc", "callsign" });
            if (callsign.empty()) callsign = JsonGetStringPath(raw, { "general", "callsign" });

            std::string dep = JsonGetStringPath(raw, { "origin", "icao_code" });
            if (dep.empty()) dep = JsonGetStringPath(raw, { "atc", "orig" });

            std::string dest = JsonGetStringPath(raw, { "destination", "icao_code" });

            // Optional: coordinates + distance (used for progress calculations when we also have live position).
            bool ok1=false, ok2=false, ok3=false, ok4=false, okd=false;
            double origin_lat2 = JsonGetDoublePath(raw, { "origin", "pos_lat" }, 0.0, &ok1);
            double origin_lon2 = JsonGetDoublePath(raw, { "origin", "pos_long" }, 0.0, &ok2);
            double dest_lat2   = JsonGetDoublePath(raw, { "destination", "pos_lat" }, 0.0, &ok3);
            double dest_lon2   = JsonGetDoublePath(raw, { "destination", "pos_long" }, 0.0, &ok4);

            // Some exports use lat/lon keys (be tolerant).
            if (!(ok1 && ok2)) {
                origin_lat2 = JsonGetDoublePath(raw, { "origin", "lat" }, origin_lat2, &ok1);
                origin_lon2 = JsonGetDoublePath(raw, { "origin", "lon" }, origin_lon2, &ok2);
            }
            if (!(ok3 && ok4)) {
                dest_lat2 = JsonGetDoublePath(raw, { "destination", "lat" }, dest_lat2, &ok3);
                dest_lon2 = JsonGetDoublePath(raw, { "destination", "lon" }, dest_lon2, &ok4);
            }

            // Planned route distance (nm) if present.
            const double route_nm = JsonGetDoublePath(raw, { "general", "route_distance" }, 0.0, &okd);

            if (dest.empty()) dest = JsonGetStringPath(raw, { "atc", "dest" });

            std::string ofp_id = JsonGetStringPath(raw, { "general", "ofp_id" });
            if (ofp_id.empty()) ofp_id = JsonGetStringPath(raw, { "general", "flight_id" });

            std::int64_t generated_unix = 0;
            {
                const std::string gen = JsonGetStringPath(raw, { "general", "time_generated" });
                if (!gen.empty()) {
                    try { generated_unix = std::stoll(gen); } catch (...) {}
                }
            }

            const std::int64_t now = NowUnixSeconds();

            nlohmann::json slim = {
                {"ok", true},
                {"source", "simbrief"},
                {"pilot_id", kPilotId},
                {"callsign", callsign},
                {"departure", dep},
                {"destination", dest},
                {"ofp_id", ofp_id},
                {"generated_at_unix", generated_unix},
                {"origin_lat_deg", (ok1 && ok2) ? origin_lat2 : 0.0},
                {"origin_lon_deg", (ok1 && ok2) ? origin_lon2 : 0.0},
                {"dest_lat_deg",   (ok3 && ok4) ? dest_lat2   : 0.0},
                {"dest_lon_deg",   (ok3 && ok4) ? dest_lon2   : 0.0},
                {"route_distance_nm", okd ? route_nm : 0.0},
                {"last_refresh_unix", now},
                {"age_seconds", 0},
                {"error", ""}
            };

            {
                std::lock_guard<std::mutex> lk(simbrief_mu_);
                simbrief_cache_ = std::move(slim);
                simbrief_error_.clear();
                simbrief_last_refresh_unix_ = now;
            }
        };

        // First refresh immediately.
        do_refresh();

        // Then refresh every 10 minutes, but allow quick shutdown.
        while (!simbrief_stop_.load()) {
            for (int i = 0; i < kRefreshSeconds && !simbrief_stop_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (simbrief_stop_.load()) break;
            do_refresh();
        }
    });
}

void HttpServer::StopSimBriefWorker() {
    simbrief_stop_.store(true);
    if (simbrief_thread_.joinable()) {
        if (std::this_thread::get_id() == simbrief_thread_.get_id()) {
            simbrief_thread_.detach();
        }
        else {
            simbrief_thread_.join();
        }
    }
}


void HttpServer::StartSimConnectWorker() {
    if (simconnect_) return;
    simconnect_ = std::make_unique<simconnect::SimConnectWorker>();
    simconnect_->Start();
}

void HttpServer::StopSimConnectWorker() {
    if (!simconnect_) return;
    simconnect_->Stop();
    simconnect_.reset();
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

    // --- API: SimBrief flight plan summary (for MSFS overlays) ---
    // GET /api/simbrief/flight
    // Cached; refreshes automatically every 10 minutes in a background worker.
    svr.Get("/api/simbrief/flight", [&](const httplib::Request&, httplib::Response& res) {
        nlohmann::json out;
        std::string err;
        std::int64_t last = 0;

        {
            std::lock_guard<std::mutex> lk(simbrief_mu_);
            out = simbrief_cache_;
            err = simbrief_error_;
            last = simbrief_last_refresh_unix_;
        }

        if (!out.is_object()) out = nlohmann::json::object();

        const std::int64_t now = NowUnixSeconds();
        const std::int64_t age = (last > 0 && now >= last) ? (now - last) : 0;

        // If we have an error, report it, but still return any cached values.
        if (!err.empty()) {
            out["ok"] = false;
            out["error"] = err;
        }

        out["last_refresh_unix"] = last;
        out["age_seconds"] = age;

        // Ensure these keys always exist so overlays can be dumb.
        if (!out.contains("callsign")) out["callsign"] = "";
        if (!out.contains("departure")) out["departure"] = "";
        if (!out.contains("destination")) out["destination"] = "";
        if (!out.contains("pilot_id")) out["pilot_id"] = 11686;
        if (!out.contains("source")) out["source"] = "simbrief";
        if (!out.contains("altitude_ft")) out["altitude_ft"] = 0.0;
        if (!out.contains("speed_kts")) out["speed_kts"] = 0.0;
        if (!out.contains("progress")) out["progress"] = 0.0;
        if (!out.contains("progress_pct")) out["progress_pct"] = 0.0;

        // --- Live sim fields (SimConnect) ---
        if (simconnect_) {
            const auto s = simconnect_->GetSnapshot();
            out["sim_connected"] = s.connected;
            if (s.has_altitude) out["altitude_ft"] = s.altitude_ft;
            if (s.has_gs) out["speed_kts"] = s.ground_speed_kts; // overlay uses SPD
            if (s.has_position) {
                out["lat_deg"] = s.lat_deg;
                out["lon_deg"] = s.lon_deg;
            }

            // Distance-based progress when we have origin/dest coords + live position.
            const double oLat = out.value("origin_lat_deg", 0.0);
            const double oLon = out.value("origin_lon_deg", 0.0);
            const double dLat = out.value("dest_lat_deg", 0.0);
            const double dLon = out.value("dest_lon_deg", 0.0);

            auto haversine_nm = [](double lat1, double lon1, double lat2, double lon2) -> double {
                constexpr double R_km = 6371.0;
                auto deg2rad = [](double x) { return x * 3.14159265358979323846 / 180.0; };
                const double p1 = deg2rad(lat1);
                const double p2 = deg2rad(lat2);
                const double dp = deg2rad(lat2 - lat1);
                const double dl = deg2rad(lon2 - lon1);
                const double a = std::sin(dp / 2) * std::sin(dp / 2) +
                                 std::cos(p1) * std::cos(p2) *
                                 std::sin(dl / 2) * std::sin(dl / 2);
                const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
                const double km = R_km * c;
                return km * 0.539956803; // km -> nm
            };

            double progress = 0.0;
            if (s.has_position && (oLat != 0.0 || oLon != 0.0) && (dLat != 0.0 || dLon != 0.0)) {
                const double total_nm = haversine_nm(oLat, oLon, dLat, dLon);
                const double rem_nm   = haversine_nm(s.lat_deg, s.lon_deg, dLat, dLon);
                if (total_nm > 1.0) {
                    progress = 1.0 - (rem_nm / total_nm);
                    if (progress < 0.0) progress = 0.0;
                    if (progress > 1.0) progress = 1.0;
                }
            }

            out["progress"] = progress;           // 0..1 (overlay supports this)
            out["progress_pct"] = progress * 100.0;
        } else {
            out["sim_connected"] = false;
        }


        res.status = 200;
        res.set_content(out.dump(2), "application/json; charset=utf-8");
    });

    svr_->Get("/api/simconnect/state", [&](const httplib::Request&, httplib::Response& res) {
        nlohmann::json out = nlohmann::json::object();

        if (simconnect_) {
            const auto s = simconnect_->GetSnapshot();
            out["ok"] = true;
            out["connected"] = s.connected;
            out["ts_unix_ms"] = s.ts_unix_ms;

            out["lat_deg"] = s.has_position ? s.lat_deg : 0.0;
            out["lon_deg"] = s.has_position ? s.lon_deg : 0.0;

            out["altitude_ft"] = s.has_altitude ? s.altitude_ft : 0.0;
            out["ground_speed_kts"] = s.has_gs ? s.ground_speed_kts : 0.0;
            out["indicated_airspeed_kts"] = s.has_ias ? s.indicated_airspeed_kts : 0.0;

            out["has_position"] = s.has_position;
            out["has_altitude"] = s.has_altitude;
            out["has_gs"] = s.has_gs;
            out["has_ias"] = s.has_ias;
        } else {
            out["ok"] = true;
            out["connected"] = false;
            out["ts_unix_ms"] = NowUnixSeconds() * 1000;
            out["has_position"] = false;
            out["has_altitude"] = false;
            out["has_gs"] = false;
            out["has_ias"] = false;
            out["lat_deg"] = 0.0;
            out["lon_deg"] = 0.0;
            out["altitude_ft"] = 0.0;
            out["ground_speed_kts"] = 0.0;
            out["indicated_airspeed_kts"] = 0.0;
        }

        res.status = 200;
        res.set_content(out.dump(2), "application/json; charset=utf-8");
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
        try {
            const auto d = state_.twitch_stream_draft_snapshot();

            TwitchHttpLog(log_,
                L"Applying stream info: title=" + ToW(d.title) +
                L" category_name=" + ToW(d.category_name) +
                L" category_id=" + ToW(d.category_id)
            );

            std::string err;
            if (!TwitchHelixUpdateChannelInfo(
                config_,
                d.title,
                d.category_id,
                &err))
            {
                TwitchHttpLog(log_, L"Apply stream info failed: " + ToW(err));

                nlohmann::json out = { {"ok", false}, {"error", err} };
                res.status = 500;
                res.set_content(out.dump(), "application/json; charset=utf-8");
                return;
            }

            TwitchHttpLog(log_, L"Apply stream info succeeded");

            nlohmann::json out = { {"ok", true} };
            res.set_content(out.dump(), "application/json; charset=utf-8");
            res.status = 200;
        }
        catch (const std::exception& e) {
            TwitchHttpLog(log_, L"Apply stream info exception: " + ToW(e.what()));

            nlohmann::json out = { {"ok", false}, {"error", "exception"}, {"what", e.what()} };
            res.status = 500;
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }
        catch (...) {
            TwitchHttpLog(log_, L"Apply stream info unknown exception");

            nlohmann::json out = { {"ok", false}, {"error", "unknown_exception"} };
            res.status = 500;
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }
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


    // --- API: Debug / test injection for alert overlay ---
    // POST /api/debug/alerts
    // Development-only: injects a synthetic Twitch EventSub-style event into the same queue
    // served by /api/twitch/eventsub/events, so overlays can be tested without live events.
#ifndef NDEBUG
    svr.Post("/api/debug/alerts", [&](const httplib::Request& req, httplib::Response& res) {
        // Safety: only allow local requests
        const std::string ra = req.remote_addr;
        if (!(ra == "127.0.0.1" || ra == "::1" || ra == "localhost")) {
            res.status = 403;
            res.set_content(R"({"ok":false,"error":"forbidden"})", "application/json; charset=utf-8");
            return;
        }

        try {
            auto in = nlohmann::json::parse(req.body);
            if (!in.is_object()) {
                res.status = 400;
                res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
                return;
            }

            // No longer require Twitch-shaped payloads for this endpoint.
            const std::string platform = in.value("platform", "");
            const std::string type = in.value("type", "");
            if (platform.empty() || type.empty()) {
                res.status = 400;
                res.set_content(R"({"ok":false,"error":"expected_platform_and_type"})", "application/json; charset=utf-8");
                return;
            }

            // Fill common fields if missing
            if (!in.contains("ts_ms") || !in["ts_ms"].is_number_integer()) {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                in["ts_ms"] = now;
            }
            if (!in.contains("id") || !in["id"].is_string() || in["id"].get<std::string>().empty()) {
                in["id"] = std::string("debug-") + std::to_string(in["ts_ms"].get<long long>());
            }

            state_.add_twitch_eventsub_event(in);

            nlohmann::json out;
            out["ok"] = true;
            out["id"] = in.value("id", "");
            res.status = 200;
            res.set_content(out.dump(2), "application/json; charset=utf-8");
        }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
        }
    });
#else
    // In release builds, keep this endpoint disabled.
    svr.Post("/api/debug/alerts", [&](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(R"({"ok":false,"error":"not_found"})", "application/json; charset=utf-8");
    });
#endif

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
            } catch (...) {}
        }

        // 2) Fallback: most recent livestream VOD by checking liveStreamingDetails on recent videos.
        if (video_id.empty()) {
            // Pull a small set of recent uploads for this channel.
            if (YouTubeApiGet("/youtube/v3/search?part=snippet&forMine=true&type=video&order=date&maxResults=10",
                              access_token, &st, &body) && st == 200) {
                try {
                    const auto js = nlohmann::json::parse(body);
                    std::vector<std::string> ids;
                    if (js.contains("items") && js["items"].is_array()) {
                        for (const auto& it : js["items"]) {
                            if (!it.contains("id") || !it["id"].is_object()) continue;
                            const auto vid = it["id"].value("videoId", "");
                            if (!vid.empty()) ids.push_back(vid);
                        }
                    }

                    if (!ids.empty()) {
                        // Batch query liveStreamingDetails for these videos (comma separated list).
                        std::string id_param;
                        for (size_t i = 0; i < ids.size(); ++i) {
                            if (i) id_param += ",";
                            id_param += ids[i];
                        }

                        std::string body2;
                        long st2 = 0;
                        if (YouTubeApiGet(("/youtube/v3/videos?part=liveStreamingDetails&id=" + id_param),
                                          access_token, &st2, &body2) && st2 == 200) {
                            const auto jv = nlohmann::json::parse(body2);
                            if (jv.contains("items") && jv["items"].is_array()) {
                                // Pick the most recent ended livestream (actualEndTime), else most recent started.
                                std::string best_id;
                                std::string best_time;
                                bool best_is_end = false;

                                auto pick_time = [](const nlohmann::json& lsd) -> std::pair<std::string, bool> {
                                    if (lsd.contains("actualEndTime") && lsd["actualEndTime"].is_string())
                                        return { lsd["actualEndTime"].get<std::string>(), true };
                                    if (lsd.contains("actualStartTime") && lsd["actualStartTime"].is_string())
                                        return { lsd["actualStartTime"].get<std::string>(), false };
                                    return { "", false };
                                };

                                for (const auto& item : jv["items"]) {
                                    if (!item.contains("id") || !item["id"].is_string()) continue;
                                    if (!item.contains("liveStreamingDetails") || !item["liveStreamingDetails"].is_object()) continue;

                                    const auto [t, is_end] = pick_time(item["liveStreamingDetails"]);
                                    if (t.empty()) continue;

                                    // ISO8601 timestamps compare lexicographically.
                                    if (best_id.empty() ||
                                        (is_end && !best_is_end) ||
                                        (is_end == best_is_end && t > best_time)) {
                                        best_id = item["id"].get<std::string>();
                                        best_time = t;
                                        best_is_end = is_end;
                                    }
                                }

                                if (!best_id.empty()) {
                                    video_id = best_id;
                                    mode = "recent_livestream_vod";
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        }

        if (video_id.empty()) {
            res.status = 404;
            res.set_content(R"({"ok":false,"error":"no_livestream_vod_found"})", "application/json; charset=utf-8");
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
                TwitchHttpLog(log_, L"OAuth start requested but routes are not enabled");
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
                TwitchHttpLog(log_, L"OAuth start failed to build authorize URL: " + ToW(err));
                res.status = 500;
                res.set_content(std::string("BuildAuthorizeUrl failed: ") + err, "text/plain; charset=utf-8");
                return;
            }

            TwitchHttpLog(log_, L"OAuth start redirecting browser to Twitch");
            res.status = 302;
            res.set_header("Location", url);
            });

        if (!opt_.twitch_auth_handle_callback) {
            TwitchHttpLog(log_, L"OAuth callback requested but routes are not enabled");
            res.status = 404;
            res.set_content("not wired", "text/plain; charset=utf-8");
            return;
        }

        const std::string code = req.get_param_value("code");
        const std::string state = req.get_param_value("state");

        std::string host = req.get_header_value("Host");
        if (host.empty()) host = "localhost:" + std::to_string(opt_.port);

        const std::string redirect_uri =
            std::string("http://") + host + "/auth/twitch/callback";

        std::string err;
        const bool ok = opt_.twitch_auth_handle_callback(code, state, redirect_uri, &err);

        if (!ok) {
            TwitchHttpLog(log_, L"OAuth callback token exchange failed: " + ToW(err));

            res.status = 500;
            res.set_content(std::string("OAuth callback failed: ") + err,
                "text/plain; charset=utf-8");
            return;
        }

        TwitchHttpLog(log_, L"OAuth callback completed successfully");

        res.set_content(
            "OK - Twitch auth completed. You can close this tab.",
            "text/plain; charset=utf-8"
        );

        // --- YouTube OAuth (interactive) ---
        // Start:
        //   http://localhost:17845/auth/youtube/start
        // Callback:
        //   http://localhost:17845/auth/youtube/callback
        svr.Get("/auth/youtube/callback", [&](const httplib::Request& req, httplib::Response& res) {
            if (!opt_.youtube_auth_handle_callback) {
                YouTubeHttpLog(log_, L"OAuth callback requested but routes are not enabled");
                res.status = 404;
                res.set_content("not wired", "text/plain; charset=utf-8");
                return;
            }

            const std::string code = req.get_param_value("code");
            const std::string state = req.get_param_value("state");

            std::string host = req.get_header_value("Host");
            if (host.empty()) host = "localhost:" + std::to_string(opt_.port);
            const std::string redirect_uri = std::string("http://") + host + "/auth/youtube/callback";

            YouTubeHttpLog(log_, L"OAuth callback received");

            std::string err;
            const bool ok = opt_.youtube_auth_handle_callback(code, state, redirect_uri, &err);
            if (!ok) {
                YouTubeHttpLog(log_, L"OAuth callback token exchange failed: " + ToW(err));
                res.status = 500;
                res.set_content(std::string("OAuth callback failed: ") + err, "text/plain; charset=utf-8");
                return;
            }

            YouTubeHttpLog(log_, L"OAuth callback completed successfully");

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
                catch (const std::exception& e) {
                    SettingsHttpLog(log_, L"Save request rejected: invalid JSON: " + ToW(e.what()));
                    res.status = 400;
                    res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
                    return;
                }
                catch (...) {
                    SettingsHttpLog(log_, L"Save request rejected: invalid JSON");
                    res.status = 400;
                    res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
                    return;
                }
            }

            const std::wstring cfg_path_w = AppConfig::ConfigPath();
            const std::string  cfg_path = WideToUtf8(cfg_path_w);

            if (!config_.Save()) {
                SettingsHttpLog(log_, L"Save failed for " + cfg_path_w);

                res.status = 500;
                json out;
                out["ok"] = false;
                out["error"] = "save_failed";
                out["path"] = cfg_path;
                res.set_content(out.dump(2), "application/json; charset=utf-8");
                return;
            }

            SettingsHttpLog(log_, L"Saved settings to " + cfg_path_w);

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

        // Twitch secrets
        out["twitch_client_id"] = config_.twitch_client_id;
        out["twitch_client_secret"] = config_.twitch_client_secret;

        // TikTok cookie/session fields
        out["tiktok_sessionid"] = config_.tiktok_sessionid;
        out["tiktok_sessionid_ss"] = config_.tiktok_sessionid_ss;
        out["tiktok_tt_target_idc"] = config_.tiktok_tt_target_idc;

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

    svr.Post("/api/euroscope", [&](const httplib::Request& req, httplib::Response& res) {
        std::string err;
        if (!euroscope_.Ingest(req.body, err)) {
            EuroScopeHttpLog(log_, L"Ingest failed: " + ToW(err));
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

        SecurityHttpLog(
            log_,
            L"Blocked non-local request to " + ToW(req.path) + L" from " + ToW(req.remote_addr)
        );

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
        }
        catch (const std::exception& e) {
            BotHttpLog(log_, L"Bot settings update rejected: bad JSON: " + ToW(e.what()));
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }
        catch (...) {
            BotHttpLog(log_, L"Bot settings update rejected: bad JSON");
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        json settings = body;
        if (body.is_object() && body.contains("settings")) settings = body["settings"];

        std::string err;
        if (!state_.set_bot_settings(settings, &err)) {
            BotHttpLog(log_, L"Bot settings update failed: " + ToW(err.empty() ? "invalid_settings" : err));
            res.status = 400;
            json out;
            out["ok"] = false;
            out["error"] = err.empty() ? "invalid_settings" : err;
            out["settings"] = state_.bot_settings_json();
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }

        BotHttpLog(log_, L"Bot settings updated successfully");

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
        }
        catch (const std::exception& e) {
            BotHttpLog(log_, L"Bot commands update rejected: bad JSON: " + ToW(e.what()));
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }
        catch (...) {
            BotHttpLog(log_, L"Bot commands update rejected: bad JSON");
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        json commands = body;
        if (body.is_object() && body.contains("commands")) commands = body["commands"];

        state_.set_bot_commands(commands);

        BotHttpLog(log_, L"Bot commands replaced successfully");

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
        catch (const std::exception& e) {
            OverlayHttpLog(log_, L"Overlay header update rejected: bad JSON: " + ToW(e.what()));
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }
        catch (...) {
            OverlayHttpLog(log_, L"Overlay header update rejected: bad JSON");
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        nlohmann::json header = body;
        if (body.is_object() && body.contains("header")) header = body["header"];

        std::string err;
        if (!state_.set_overlay_header(header, &err)) {
            OverlayHttpLog(log_, L"Overlay header update failed: " + ToW(err.empty() ? "invalid_header" : err));
            res.status = 400;
            nlohmann::json out;
            out["ok"] = false;
            out["error"] = err.empty() ? "invalid_header" : err;
            out["header"] = state_.overlay_header_json();
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }

        OverlayHttpLog(log_, L"Overlay header updated successfully");

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
        catch (const std::exception& e) {
            BotHttpLog(log_, L"Bot command upsert rejected: bad JSON: " + ToW(e.what()));
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }
        catch (...) {
            BotHttpLog(log_, L"Bot command upsert rejected: bad JSON");
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }

        std::string err;
        if (!state_.bot_upsert_command(body, &err)) {
            BotHttpLog(log_, L"Bot command upsert failed: " + ToW(err.empty() ? "invalid_command" : err));
            res.status = 400;
            json out;
            out["ok"] = false;
            out["error"] = err.empty() ? "invalid_command" : err;
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }

        BotHttpLog(log_, L"Bot command upserted successfully");

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
        const bool removed = state_.bot_delete_command(cmd);

        if (removed) {
            BotHttpLog(log_, L"Bot command deleted: " + ToW(cmd));
        }
        else {
            BotHttpLog(log_, L"Bot command delete requested but not found: " + ToW(cmd));
        }

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
            BotHttpLog(log_, L"Bot command delete rejected: missing command");
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"missing_command"})", "application/json; charset=utf-8");
        }
        });

    svr.Delete("/api/bot/commands", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_local(req, res)) return;

        if (!req.has_param("command")) {
            BotHttpLog(log_, L"Bot command delete rejected: missing command");
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
        catch (const std::exception& e) {
            BotHttpLog(log_, L"Bot test inject rejected: bad JSON: " + ToW(e.what()));
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"bad_json"})", "application/json; charset=utf-8");
            return;
        }
        catch (...) {
            BotHttpLog(log_, L"Bot test inject rejected: bad JSON");
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

        json out;
        out["ok"] = true;
        out["ts_ms"] = now_ms_ll;

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
                    out["note"] = bs.silent_mode ? "silent_mode_reply_preview" : "reply_preview_only";
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

            BotHttpLog(
                log_,
                L"Bot test inject accepted: platform=" + ToW(platform) +
                L" user=" + ToW(user) +
                (out.value("matched", false) ? L" matched=yes" : L" matched=no")
            );
        }
        else {
            BotHttpLog(log_, L"Bot test inject requested with empty message");
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

        ChatHttpLog(
            log_,
            L"Test chat injected: platform=" + ToW(m.platform) +
            L" user=" + ToW(m.user)
        );

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


    // --- API: Unified alerts history (missed alerts / replay tooling) ---
    // GET /api/alerts/history?limit=200&platform=twitch|tiktok|youtube
    svr.Get("/api/alerts/history", [&](const httplib::Request& req, httplib::Response& res) {
        int limit = 200;
        if (req.has_param("limit")) {
            try { limit = std::max(1, std::min(5000, std::stoi(req.get_param_value("limit")))); }
            catch (...) {}
        }
        std::string platform;
        if (req.has_param("platform")) platform = req.get_param_value("platform");

        auto j = state_.alerts_history_json(limit, platform);
        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(j.dump(2), "application/json; charset=utf-8");
        });

#ifndef NDEBUG
    // POST /api/alerts/resend  (localhost-only)
    // Body: { "id": "hist-123" } or { "ids": ["hist-123","hist-124"] }
    svr.Post("/api/alerts/resend", [&](const httplib::Request& req, httplib::Response& res) {
        try {

        const std::string ra = req.remote_addr;
        if (!(ra == "127.0.0.1" || ra == "::1" || ra == "localhost")) {
            res.status = 403;
            res.set_content(R"({"ok":false,"error":"forbidden"})", "application/json; charset=utf-8");
            return;
        }

        json in;
        try { in = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"invalid_json"})", "application/json; charset=utf-8");
            return;
        }

        std::vector<std::string> ids;
        if (in.contains("id") && in["id"].is_string()) {
            ids.push_back(in["id"].get<std::string>());
        } else if (in.contains("ids") && in["ids"].is_array()) {
            for (auto& v : in["ids"]) if (v.is_string()) ids.push_back(v.get<std::string>());
        }

        if (ids.empty()) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"missing_id"})", "application/json; charset=utf-8");
            return;
        }

        json out;
        out["ok"] = true;
        out["replayed"] = json::array();
        out["failed"] = json::array();

        for (const auto& id : ids) {
            json replayed;
            std::string err;
            if (state_.resend_alert_history(id, &replayed, &err)) {
                out["replayed"].push_back(replayed);
            } else {
                out["failed"].push_back(json{{"id", id}, {"error", err}});
            }
        }

        res.set_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res.set_header("Pragma", "no-cache");
        res.set_content(out.dump(2), "application/json; charset=utf-8");
        }
        catch (const std::exception& ex) {
            res.status = 500;
            nlohmann::json out;
            out["ok"] = false;
            out["error"] = "exception";
            out["detail"] = ex.what();
            res.set_content(out.dump(2), "application/json; charset=utf-8");
            return;
        }
        catch (...) {
            res.status = 500;
            res.set_content(R"({\"ok\":false,\"error\":\"exception\"})", "application/json; charset=utf-8");
            return;
        }
});
#else
    svr.Post("/api/alerts/resend", [&](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(R"({"ok":false,"error":"not_found"})", "application/json; charset=utf-8");
        });
#endif


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


    inline void PlatformHttpLog(std::function<void(const std::wstring&)>& log,
        const std::wstring& msg)
    {
        SafeOutputLog(log, L"[HttpServer][Platform] " + msg);
    }

    // Root -> overlay
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/overlay/");
        });
}