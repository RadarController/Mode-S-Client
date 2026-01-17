#include "AppState.h"

#include <algorithm>
#include <fstream>
#include <filesystem>

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::int64_t AppState::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::vector<ChatMessage> AppState::recent_chat() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return std::vector<ChatMessage>(chat_.begin(), chat_.end());
}

void AppState::set_tiktok_viewers(int v) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.tiktok_viewers = v;
}
void AppState::set_tiktok_followers(int f) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.tiktok_followers = f;
}
void AppState::set_tiktok_live(bool live) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.tiktok_live = live;
}

void AppState::set_twitch_viewers(int v) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.twitch_viewers = v;
}
void AppState::set_twitch_followers(int f) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.twitch_followers = f;
}
void AppState::set_twitch_live(bool live) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.twitch_live = live;
}

void AppState::set_youtube_viewers(int v) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.youtube_viewers = v;
}
void AppState::set_youtube_followers(int f) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.youtube_followers = f;
}
void AppState::set_youtube_live(bool live) {
    std::lock_guard<std::mutex> lk(mtx_);
    metrics_.ts_ms = now_ms();
    metrics_.youtube_live = live;
}

Metrics AppState::get_metrics() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return metrics_;
}

nlohmann::json AppState::metrics_json() const {
    auto m = get_metrics();
    return {
        {"ts_ms", m.ts_ms},
        {"twitch_viewers", m.twitch_viewers},
        {"youtube_viewers", m.youtube_viewers},
        {"tiktok_viewers", m.tiktok_viewers},
        {"twitch_followers", m.twitch_followers},
        {"youtube_followers", m.youtube_followers},
        {"tiktok_followers", m.tiktok_followers},
        {"twitch_live", m.twitch_live},
        {"youtube_live", m.youtube_live},
        {"tiktok_live", m.tiktok_live},
        {"total_viewers", m.total_viewers()},
        {"total_followers", m.total_followers()}
    };
}

nlohmann::json AppState::chat_json() const {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& c : recent_chat()) {
        nlohmann::json j = c; // uses to_json(ChatMessage)
        arr.push_back(std::move(j));
    }
    return arr;
}

// --- Twitch EventSub diagnostics ---
void AppState::set_twitch_eventsub_status(const nlohmann::json& status) {
    std::lock_guard<std::mutex> lk(mtx_);
    twitch_eventsub_status_ = status;
}

nlohmann::json AppState::twitch_eventsub_status_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return twitch_eventsub_status_;
}

void AppState::add_twitch_eventsub_event(const nlohmann::json& ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    twitch_eventsub_events_.push_back(ev);
    // Keep small
    while (twitch_eventsub_events_.size() > 200) twitch_eventsub_events_.pop_front();
}

nlohmann::json AppState::twitch_eventsub_events_json(int limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    limit = std::max(1, std::min(limit, 1000));

    nlohmann::json out;
    out["count"] = (int)twitch_eventsub_events_.size();
    nlohmann::json arr = nlohmann::json::array();

    int start = (int)twitch_eventsub_events_.size() - limit;
    if (start < 0) start = 0;
    for (int i = start; i < (int)twitch_eventsub_events_.size(); ++i) {
        arr.push_back(twitch_eventsub_events_[i]);
    }
    out["events"] = std::move(arr);
    return out;
}

void AppState::clear_twitch_eventsub_events() {
    std::lock_guard<std::mutex> lk(mtx_);
    twitch_eventsub_events_.clear();
}

void AppState::push_tiktok_event(const EventItem& e) {
    std::lock_guard<std::mutex> lk(mtx_);
    tiktok_events_.push_back(e);
    while (tiktok_events_.size() > 200) tiktok_events_.pop_front();
}

nlohmann::json AppState::tiktok_events_json(size_t limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json out;
    out["count"] = (int)tiktok_events_.size();
    nlohmann::json arr = nlohmann::json::array();

    size_t n = tiktok_events_.size();
    size_t start = 0;
    if (limit > 0 && n > limit) start = n - limit;

    for (size_t i = start; i < n; ++i) {
        const auto& e = tiktok_events_[i];
        nlohmann::json j;
        j["platform"] = e.platform;
        j["type"] = e.type;
        j["user"] = e.user;
        j["message"] = e.message;
        j["ts_ms"] = e.ts_ms;
        arr.push_back(std::move(j));
    }

    out["events"] = std::move(arr);
    return out;
}

void AppState::push_youtube_event(const EventItem& e) {
    std::lock_guard<std::mutex> lk(mtx_);
    youtube_events_.push_back(e);
    while (youtube_events_.size() > 200) youtube_events_.pop_front();
}

nlohmann::json AppState::youtube_events_json(size_t limit) const {
    std::lock_guard<std::mutex> lk(mtx_);

    nlohmann::json out;
    out["count"] = (int)youtube_events_.size();
    nlohmann::json arr = nlohmann::json::array();

    size_t n = youtube_events_.size();
    size_t start = 0;
    if (limit > 0 && n > limit) start = n - limit;

    for (size_t i = start; i < n; ++i) {
        const auto& e = youtube_events_[i];
        nlohmann::json j;
        j["platform"] = e.platform;
        j["type"] = e.type;
        j["user"] = e.user;
        j["message"] = e.message;
        j["ts_ms"] = e.ts_ms;
        arr.push_back(std::move(j));
    }

    out["events"] = std::move(arr);
    return out;
}

// --- Bot commands ---
void AppState::set_bot_commands_storage_path(const std::string& path_utf8) {
    std::lock_guard<std::mutex> lk(mtx_);
    bot_commands_path_utf8_ = path_utf8;
}

static bool AtomicWriteUtf8File(const std::string& path_utf8, const std::string& content) {
    try {
        std::filesystem::path p = std::filesystem::u8path(path_utf8);
        std::filesystem::create_directories(p.parent_path());
        std::filesystem::path tmp = p;
        tmp += ".tmp";

        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f.write(content.data(), (std::streamsize)content.size());
            if (!f) return false;
        }

        std::error_code ec;
        std::filesystem::rename(tmp, p, ec);
        if (ec) {
            // On Windows, rename fails if target exists; fall back to remove+rename.
            std::filesystem::remove(p, ec);
            ec.clear();
            std::filesystem::rename(tmp, p, ec);
            if (ec) {
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool AppState::load_bot_commands_from_disk() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        path = bot_commands_path_utf8_;
    }
    if (path.empty()) return false;

    try {
        std::filesystem::path p = std::filesystem::u8path(path);
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (s.empty()) return false;

        nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
        if (j.is_discarded()) return false;

        // Don't auto-save while loading.
        std::lock_guard<std::mutex> lk(mtx_);
        bot_cmds_.clear();
        if (!j.is_array()) return false;

        for (const auto& c : j) {
            if (!c.is_object()) continue;
            std::string cmd = c.value("command", "");
            if (cmd.empty()) continue;
            if (!cmd.empty() && cmd[0] == '!') cmd.erase(cmd.begin());
            cmd = ToLower(cmd);
            if (cmd.empty()) continue;

            BotCmd bc;
            bc.response = c.value("response", "");
            bc.enabled = c.value("enabled", true);
            // Back-compat: prefer cooldown_ms, else accept cooldown_s.
            if (c.contains("cooldown_ms")) {
                bc.cooldown_ms = c.value("cooldown_ms", 3000);
            } else {
                int cs = c.value("cooldown_s", 3);
                bc.cooldown_ms = cs * 1000;
            }
            if (bc.cooldown_ms < 0) bc.cooldown_ms = 0;
            if (bc.cooldown_ms > 600000) bc.cooldown_ms = 600000;

            // Scope: all | mods | broadcaster
            bc.scope = ToLower(c.value("scope", "all"));
            if (bc.scope != "all" && bc.scope != "mods" && bc.scope != "broadcaster") {
                bc.scope = "all";
            }
            bot_cmds_[cmd] = std::move(bc);
        }
        return true;
    } catch (...) {
        return false;
    }
}

void AppState::set_bot_commands(const nlohmann::json& commands) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        bot_cmds_.clear();

        if (commands.is_array()) {
            for (const auto& c : commands) {
                if (!c.is_object()) continue;
                std::string cmd = c.value("command", "");
                if (cmd.empty()) continue;
                if (!cmd.empty() && cmd[0] == '!') cmd.erase(cmd.begin());
                cmd = ToLower(cmd);
                if (cmd.empty()) continue;

                BotCmd bc;
                bc.response = c.value("response", "");
                bc.enabled = c.value("enabled", true);
                if (c.contains("cooldown_ms")) {
                    bc.cooldown_ms = c.value("cooldown_ms", 3000);
                } else {
                    int cs = c.value("cooldown_s", 3);
                    bc.cooldown_ms = cs * 1000;
                }
                if (bc.cooldown_ms < 0) bc.cooldown_ms = 0;
                if (bc.cooldown_ms > 600000) bc.cooldown_ms = 600000;

                bc.scope = ToLower(c.value("scope", "all"));
                if (bc.scope != "all" && bc.scope != "mods" && bc.scope != "broadcaster") {
                    bc.scope = "all";
                }
                bot_cmds_[cmd] = std::move(bc);
            }
        }

        path = bot_commands_path_utf8_;
    }

    // Persist (best-effort). We write the incoming array (not the map) to preserve ordering.
    if (!path.empty()) {
        try {
            nlohmann::json out = commands;
            if (!out.is_array()) out = nlohmann::json::array();
            (void)AtomicWriteUtf8File(path, out.dump(2));
        } catch (...) {
            // ignore
        }
    }
}

nlohmann::json AppState::bot_commands_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& kv : bot_cmds_) {
        nlohmann::json j;
        j["command"] = kv.first;
        j["response"] = kv.second.response;
        j["enabled"] = kv.second.enabled;
        j["cooldown_ms"] = kv.second.cooldown_ms;
        j["scope"] = kv.second.scope;
        arr.push_back(std::move(j));
    }
    return arr;
}

std::string AppState::bot_lookup_response(const std::string& command_lc) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = bot_cmds_.find(command_lc);
    if (it == bot_cmds_.end()) return {};
    if (!it->second.enabled) return {};
    return it->second.response;
}

std::string AppState::bot_try_get_response(
    const std::string& command_lc,
    bool is_mod,
    bool is_broadcaster,
    std::int64_t now_ms)
{
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = bot_cmds_.find(command_lc);
    if (it == bot_cmds_.end()) return {};
    BotCmd& cmd = it->second;
    if (!cmd.enabled) return {};

    const bool has_mod = is_mod || is_broadcaster;
    if (cmd.scope == "mods" && !has_mod) return {};
    if (cmd.scope == "broadcaster" && !is_broadcaster) return {};

    if (cmd.cooldown_ms > 0) {
        const std::int64_t since = now_ms - cmd.last_fire_ms;
        if (cmd.last_fire_ms != 0 && since >= 0 && since < cmd.cooldown_ms) {
            return {};
        }
    }

    cmd.last_fire_ms = now_ms;
    return cmd.response;
}

void AppState::push_log_utf8(const std::string& msg) {
    if (msg.empty()) return;

    std::lock_guard<std::mutex> lk(mtx_);
    LogEntry e;
    e.id = ++log_next_id_;
    e.ts_ms = now_ms();
    e.msg = msg;

    log_.push_back(std::move(e));
    while (log_.size() > 2000) log_.pop_front(); // keep it bounded
}

nlohmann::json AppState::log_json(std::uint64_t since, int limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    limit = std::max(1, std::min(limit, 1000));

    nlohmann::json out;
    out["ok"] = true;
    nlohmann::json arr = nlohmann::json::array();

    // Return entries with id > since, oldest -> newest
    int count = 0;
    for (const auto& e : log_) {
        if (e.id <= since) continue;
        arr.push_back({
            {"id", e.id},
            {"ts_ms", e.ts_ms},
            {"msg", e.msg}
            });
        if (++count >= limit) break;
    }

    out["entries"] = std::move(arr);
    return out;
}
