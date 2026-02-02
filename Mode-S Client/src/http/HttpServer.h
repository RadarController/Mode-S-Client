#pragma once
#include <memory>
#include <string>
#include <thread>
#include <filesystem>
#include <functional>

#include "httplib.h"
#include "LogBuffer.h"

class AppState;
class ChatAggregator;
class EuroScopeIngestService;
struct AppConfig;

// Simple embedded HTTP server that hosts API routes and overlay static files.
class HttpServer {
public:
    struct Options {
        std::function<std::string()> youtube_auth_info_json;
        std::string bind_host = "127.0.0.1";
        int port = 17845;
        std::filesystem::path overlay_root; // typically <exe_dir>/assets/overlay

        // Optional platform control callbacks (wired from main app)
        std::function<bool()> start_tiktok;
        std::function<bool()> stop_tiktok;
        std::function<bool()> start_twitch;
        std::function<bool()> stop_twitch;
        std::function<bool()> start_youtube;
        std::function<bool()> stop_youtube;

        // Twitch OAuth (interactive) endpoints (optional)
        // /auth/twitch/start will call twitch_auth_build_authorize_url(redirect_uri)
        // /auth/twitch/callback will call twitch_auth_handle_callback(code, state, redirect_uri)
        std::function<std::string(const std::string& redirect_uri, std::string* out_error)> twitch_auth_build_authorize_url;
        std::function<bool(const std::string& code,
                           const std::string& state,
                           const std::string& redirect_uri,
                           std::string* out_error)> twitch_auth_handle_callback;

        // YouTube OAuth (interactive) endpoints (optional)
        // /auth/youtube/start will call youtube_auth_build_authorize_url(redirect_uri)
        // /auth/youtube/callback will call youtube_auth_handle_callback(code, state, redirect_uri)
        std::function<std::string(const std::string& redirect_uri, std::string* out_error)> youtube_auth_build_authorize_url;
        std::function<bool(const std::string& code,
                           const std::string& state,
                           const std::string& redirect_uri,
                           std::string* out_error)> youtube_auth_handle_callback;
    };

    using LogFn = std::function<void(const std::wstring&)>;

    HttpServer(AppState& state,
               ChatAggregator& chat,
               EuroScopeIngestService& euroscope,
               AppConfig& config,
               Options options,
               LogFn log);

    ~HttpServer();

    void Start();
    void Stop();

private:
    void RegisterRoutes();
    void ApplyOverlayTokens(std::string& html);

    AppState& state_;
    ChatAggregator& chat_;
    EuroScopeIngestService& euroscope_;
    AppConfig& config_;
    Options opt_;
    LogFn log_;

    // Thread-safe log buffer for the Web UI (/api/log)
    LogBuffer logbuf_;

    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
};