#include "AppState.h"

#include <algorithm>
#include <fstream>
#include <filesystem>

static bool AtomicWriteUtf8File(const std::string& path_utf8, const std::string& content);

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

// --- Bot safety settings ---
void AppState::set_bot_settings_storage_path(const std::string& path_utf8) {
    std::lock_guard<std::mutex> lk(mtx_);
    bot_settings_path_utf8_ = path_utf8;
}

static AppState::BotSettings ClampBotSettings(AppState::BotSettings s) {
    // Prevent pathological values.
    if (s.per_user_gap_ms < 0) s.per_user_gap_ms = 0;
    if (s.per_user_gap_ms > 600000) s.per_user_gap_ms = 600000;

    if (s.per_platform_gap_ms < 0) s.per_platform_gap_ms = 0;
    if (s.per_platform_gap_ms > 600000) s.per_platform_gap_ms = 600000;

    // reply len: allow 0 (effectively "no reply"), but keep a sane upper bound.
    if (s.max_reply_len > 2000) s.max_reply_len = 2000;
    return s;
}

bool AppState::load_bot_settings_from_disk() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        path = bot_settings_path_utf8_;
    }
    if (path.empty()) return false;

    try {
        std::filesystem::path p = std::filesystem::u8path(path);
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (s.empty()) return false;

        nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;

        BotSettings loaded;
        loaded.per_user_gap_ms = j.value("per_user_gap_ms", loaded.per_user_gap_ms);
        loaded.per_platform_gap_ms = j.value("per_platform_gap_ms", loaded.per_platform_gap_ms);

        // max_reply_len may be stored as number; accept int/size.
        if (j.contains("max_reply_len")) {
            try {
                long long v = j["max_reply_len"].get<long long>();
                if (v < 0) v = 0;
                loaded.max_reply_len = (size_t)v;
            } catch (...) {
                // ignore and keep default
            }
        }
        loaded.silent_mode = j.value("silent_mode", loaded.silent_mode);

        loaded = ClampBotSettings(std::move(loaded));

        {
            std::lock_guard<std::mutex> lk(mtx_);
            bot_settings_ = std::move(loaded);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool AppState::set_bot_settings(const nlohmann::json& settings_obj, std::string* err) {
    if (!settings_obj.is_object()) {
        if (err) *err = "not_object";
        return false;
    }

    BotSettings s;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        s = bot_settings_;
    }

    if (settings_obj.contains("per_user_gap_ms")) s.per_user_gap_ms = settings_obj.value("per_user_gap_ms", (long long)s.per_user_gap_ms);
    if (settings_obj.contains("per_platform_gap_ms")) s.per_platform_gap_ms = settings_obj.value("per_platform_gap_ms", (long long)s.per_platform_gap_ms);

    if (settings_obj.contains("max_reply_len")) {
        try {
            long long v = settings_obj["max_reply_len"].get<long long>();
            if (v < 0) v = 0;
            s.max_reply_len = (size_t)v;
        } catch (...) {
            if (err) *err = "bad_max_reply_len";
            return false;
        }
    }

    if (settings_obj.contains("silent_mode")) s.silent_mode = settings_obj.value("silent_mode", s.silent_mode);

    s = ClampBotSettings(std::move(s));

    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        bot_settings_ = s;
        path = bot_settings_path_utf8_;
    }

    if (!path.empty()) {
        try {
            (void)AtomicWriteUtf8File(path, bot_settings_json().dump(2));
        } catch (...) {}
    }
    return true;
}

nlohmann::json AppState::bot_settings_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json j;
    j["per_user_gap_ms"] = bot_settings_.per_user_gap_ms;
    j["per_platform_gap_ms"] = bot_settings_.per_platform_gap_ms;
    j["max_reply_len"] = bot_settings_.max_reply_len;
    j["silent_mode"] = bot_settings_.silent_mode;
    return j;
}

AppState::BotSettings AppState::bot_settings_snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return bot_settings_;
}


// --- Overlay header ---
void AppState::set_overlay_header_storage_path(const std::string& path_utf8) {
    std::lock_guard<std::mutex> lk(mtx_);
    overlay_header_path_utf8_ = path_utf8;
}

static AppState::OverlayHeader ClampOverlayHeader(AppState::OverlayHeader h) {
    // Basic sanity limits to avoid huge strings in overlays.
    const std::size_t kMaxTitle = 200;
    const std::size_t kMaxSubtitle = 200;
    if (h.title.size() > kMaxTitle) h.title.resize(kMaxTitle);
    if (h.subtitle.size() > kMaxSubtitle) h.subtitle.resize(kMaxSubtitle);
    return h;
}

bool AppState::load_overlay_header_from_disk() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        path = overlay_header_path_utf8_;
    }
    if (path.empty()) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    try {
        auto j = nlohmann::json::parse(content);
        OverlayHeader h;
        h.title = j.value("title", "");
        h.subtitle = j.value("subtitle", "");
        h = ClampOverlayHeader(h);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            overlay_header_ = h;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool AppState::set_overlay_header(const nlohmann::json& header_obj, std::string* err) {
    OverlayHeader h;
    try {
        if (!header_obj.is_object()) {
            if (err) *err = "expected JSON object";
            return false;
        }
        h.title = header_obj.value("title", "");
        h.subtitle = header_obj.value("subtitle", "");
    } catch (...) {
        if (err) *err = "invalid JSON";
        return false;
    }

    h = ClampOverlayHeader(h);

    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        overlay_header_ = h;
        path = overlay_header_path_utf8_;
    }

    // Persist best-effort if configured.
    if (!path.empty()) {
        nlohmann::json out = {
            {"title", h.title},
            {"subtitle", h.subtitle}
        };
        if (!AtomicWriteUtf8File(path, out.dump(2))) {
            // Keep in-memory update even if disk write fails.
            if (err) *err = "failed to write file";
            // still return true to avoid blocking UX
        }
    }
    return true;
}

nlohmann::json AppState::overlay_header_json() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return nlohmann::json{
        {"title", overlay_header_.title},
        {"subtitle", overlay_header_.subtitle}
    };
}

AppState::OverlayHeader AppState::overlay_header_snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return overlay_header_;
}

static std::string NormalizeCommandKey(std::string cmd) {
    if (!cmd.empty() && cmd[0] == '!') cmd.erase(cmd.begin());
    cmd = ToLower(std::move(cmd));
    // Trim surrounding whitespace
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!cmd.empty() && is_ws((unsigned char)cmd.front())) cmd.erase(cmd.begin());
    while (!cmd.empty() && is_ws((unsigned char)cmd.back())) cmd.pop_back();
    // Stop at first whitespace (command token)
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (is_ws((unsigned char)cmd[i])) {
            cmd.resize(i);
            break;
        }
    }
    return cmd;
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

        if (!j.is_array()) return false;

        // Build a fresh map first; only swap in if parsing succeeds.
        std::unordered_map<std::string, BotCmd> loaded;

        for (const auto& c : j) {
            if (!c.is_object()) continue;
            std::string cmd = NormalizeCommandKey(c.value("command", ""));
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
            bc.last_fire_ms = 0; // never persist cooldown state
            loaded[cmd] = std::move(bc);
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            bot_cmds_ = std::move(loaded);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool AppState::bot_upsert_command(const nlohmann::json& command_obj, std::string* err) {
    if (!command_obj.is_object()) {
        if (err) *err = "not_object";
        return false;
    }

    std::string cmd = NormalizeCommandKey(command_obj.value("command", ""));
    if (cmd.empty()) {
        if (err) *err = "missing_command";
        return false;
    }

    BotCmd bc;
    bc.response = command_obj.value("response", "");
    bc.enabled = command_obj.value("enabled", true);
    if (command_obj.contains("cooldown_ms")) {
        bc.cooldown_ms = command_obj.value("cooldown_ms", 3000);
    } else {
        int cs = command_obj.value("cooldown_s", 3);
        bc.cooldown_ms = cs * 1000;
    }
    if (bc.cooldown_ms < 0) bc.cooldown_ms = 0;
    if (bc.cooldown_ms > 600000) bc.cooldown_ms = 600000;
    bc.scope = ToLower(command_obj.value("scope", "all"));
    if (bc.scope != "all" && bc.scope != "mods" && bc.scope != "broadcaster") bc.scope = "all";
    bc.last_fire_ms = 0;

    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        bot_cmds_[cmd] = bc;
        path = bot_commands_path_utf8_;
    }

    if (!path.empty()) {
        try {
            (void)AtomicWriteUtf8File(path, bot_commands_json().dump(2));
        } catch (...) {}
    }
    return true;
}

bool AppState::bot_delete_command(const std::string& command) {
    std::string cmd = NormalizeCommandKey(command);
    if (cmd.empty()) return false;

    bool removed = false;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        removed = (bot_cmds_.erase(cmd) > 0);
        path = bot_commands_path_utf8_;
    }
    if (removed && !path.empty()) {
        try {
            (void)AtomicWriteUtf8File(path, bot_commands_json().dump(2));
        } catch (...) {}
    }
    return removed;
}

void AppState::set_bot_commands(const nlohmann::json& commands) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        bot_cmds_.clear();

        if (commands.is_array()) {
            for (const auto& c : commands) {
                if (!c.is_object()) continue;
                std::string cmd = NormalizeCommandKey(c.value("command", ""));
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
                bc.last_fire_ms = 0;
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
    std::vector<std::string> keys;
    keys.reserve(bot_cmds_.size());
    for (const auto& kv : bot_cmds_) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& k : keys) {
        const auto& kv = *bot_cmds_.find(k);
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

std::string AppState::bot_peek_response(
    const std::string& command_lc,
    bool is_mod,
    bool is_broadcaster,
    std::int64_t now_ms) const
{
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = bot_cmds_.find(command_lc);
    if (it == bot_cmds_.end()) return {};
    const BotCmd& cmd = it->second;
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


// --- Twitch Stream Info draft ---

void AppState::load_twitch_stream_draft_from_config_unlocked()
{
    if (twitch_stream_draft_loaded_) return;
    twitch_stream_draft_loaded_ = true;

    try {
        const auto path = std::filesystem::absolute("config.json");
        std::ifstream in(path, std::ios::binary);
        if (!in) return;

        std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (s.empty()) return;

        auto j = nlohmann::json::parse(s);

        // Preferred key
        if (j.contains("twitch_streaminfo") && j["twitch_streaminfo"].is_object()) {
            const auto& t = j["twitch_streaminfo"];
            twitch_stream_draft_.title = t.value("title", "");
            twitch_stream_draft_.category_name = t.value("category_name", t.value("category", ""));
            twitch_stream_draft_.category_id = t.value("category_id", t.value("game_id", ""));
            twitch_stream_draft_.description = t.value("description", "");
            return;
        }

        // Back-compat: allow nesting under "twitch"
        if (j.contains("twitch") && j["twitch"].is_object()) {
            const auto& tw = j["twitch"];
            if (tw.contains("streaminfo") && tw["streaminfo"].is_object()) {
                const auto& t = tw["streaminfo"];
                twitch_stream_draft_.title = t.value("title", "");
                twitch_stream_draft_.category_name = t.value("category_name", t.value("category", ""));
                twitch_stream_draft_.category_id = t.value("category_id", t.value("game_id", ""));
                twitch_stream_draft_.description = t.value("description", "");
                return;
            }
        }
    }
    catch (...) {
        // best-effort; leave defaults
    }
}

void AppState::save_twitch_stream_draft_to_config_unlocked()
{
    try {
        const auto path = std::filesystem::absolute("config.json");

        nlohmann::json j = nlohmann::json::object();

        // Load existing config if present so we don't clobber secrets/other settings.
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (!s.empty()) {
                    j = nlohmann::json::parse(s);
                    if (!j.is_object()) j = nlohmann::json::object();
                }
            }
        }

        nlohmann::json t;
        t["title"] = twitch_stream_draft_.title;
        t["category_name"] = twitch_stream_draft_.category_name;
        t["category_id"] = twitch_stream_draft_.category_id;
        t["description"] = twitch_stream_draft_.description;

        j["twitch_streaminfo"] = t;

        // Atomic write via existing helper (expects UTF-8 path).
        AtomicWriteUtf8File(path.u8string(), j.dump(2));
    }
    catch (...) {
        // best-effort persistence; ignore failures
    }
}

void AppState::set_twitch_stream_draft(const TwitchStreamDraft& d)
{
    std::lock_guard<std::mutex> lk(mtx_);
    load_twitch_stream_draft_from_config_unlocked(); // ensure loaded once
    twitch_stream_draft_ = d;
    save_twitch_stream_draft_to_config_unlocked();
}

AppState::TwitchStreamDraft AppState::twitch_stream_draft_snapshot()
{
    std::lock_guard<std::mutex> lk(mtx_);
    load_twitch_stream_draft_from_config_unlocked();
    return twitch_stream_draft_;
}

nlohmann::json AppState::twitch_stream_draft_json()
{
    std::lock_guard<std::mutex> lk(mtx_);
    load_twitch_stream_draft_from_config_unlocked();
    nlohmann::json j;
    j["ok"] = true;
    j["title"] = twitch_stream_draft_.title;
    j["category_name"] = twitch_stream_draft_.category_name;
    j["category_id"] = twitch_stream_draft_.category_id;   // <-- REQUIRED
    j["description"] = twitch_stream_draft_.description;
    return j;
}
