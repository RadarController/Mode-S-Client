#pragma once
#include <memory>
#include <string>
#include <thread>
#include <filesystem>
#include <functional>

#include "httplib.h"

class AppState;
class ChatAggregator;
class EuroScopeIngestService;
struct AppConfig;

// Simple embedded HTTP server that hosts API routes and overlay static files.
class HttpServer {
public:
    struct Options {
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

    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
};