#pragma once
#include <string>
#include <windows.h>
#include <filesystem>
#include <cstdio>
#include "json.hpp"

struct AppConfig
{
    std::string overlay_font_family = "Inter";  // or "" for system default
    int         overlay_font_size = 22;
    bool        overlay_text_shadow = true;

    std::string tiktok_unique_id;  // no '@'
    std::string twitch_login;      // login (lowercase is safest)
    std::string twitch_client_id;
    std::string twitch_client_secret;
    std::string metrics_json_path; // optional override
    std::string youtube_handle;    // with '@'
    std::string tiktok_sessionid;
    std::string tiktok_sessionid_ss;
    std::string tiktok_tt_target_idc;

    static std::wstring GetExeDir()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p = path;
        auto pos = p.find_last_of(L"\\/");
        return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
    }

    // Resolve config.json location:
//  1) current working directory (./config.json)
//  2) folder containing the running .exe (<exe-dir>/config.json)
//
// For Save(): prefer writing back to whichever location we loaded from; otherwise use CWD.
inline static std::wstring s_last_loaded_path;

static std::wstring ResolveConfigPathForRead()
{
    try {
        auto cwd = std::filesystem::current_path() / "config.json";
        if (std::filesystem::exists(cwd)) return cwd.wstring();
    } catch (...) {}

    auto exe = std::filesystem::path(GetExeDir()) / "config.json";
    return exe.wstring();
}

static std::wstring ResolveConfigPathForWrite()
{
    if (!s_last_loaded_path.empty()) return s_last_loaded_path;

    // If CWD has a config.json, update that; else create in CWD.
    try {
        auto cwd = std::filesystem::current_path() / "config.json";
        return cwd.wstring();
    } catch (...) {}

    // Last resort: exe directory.
    return (std::filesystem::path(GetExeDir()) / "config.json").wstring();
}

// Backward-compatible helper used across the codebase.
// Prefer the path we will write to (which is the loaded path when available,
// otherwise the CWD path). This is primarily for diagnostics (logging/UI).
static std::wstring ConfigPath()
{
    return ResolveConfigPathForWrite();
}

    static void DebugLogConfigLookup(const wchar_t* action, const std::wstring& path)
    {
        // Log to both debugger output (Visual Studio Output window) and stderr.
        // This helps diagnose "works in Release, not in Debug" issues where the working directory differs.
        wchar_t buf[2048] = {0};
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"[AppConfig] %s config.json at: %s\n", action ? action : L"Looking for", path.c_str());
        OutputDebugStringW(buf);
        fwprintf(stderr, L"%s", buf);
    }

    bool Load()
    {
        const auto path = ResolveConfigPathForRead();
        DebugLogConfigLookup(L"Looking for", path);
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"rb");
        if (f) { s_last_loaded_path = path; }
        if (!f) {
            // Helpful: log last error so we know whether it's simply missing vs permissions, etc.
            const DWORD err = GetLastError();
            wchar_t buf[256] = {0};
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"[AppConfig] Failed to open config.json (err=%lu)\n", (unsigned long)err);
            OutputDebugStringW(buf);
            fwprintf(stderr, L"%s", buf);
            return false;
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);

        std::string data;
        data.resize(sz > 0 ? (size_t)sz : 0);
        if (sz > 0) fread(data.data(), 1, (size_t)sz, f);
        fclose(f);

        try {
            auto j = nlohmann::json::parse(data);
            tiktok_unique_id = j.value("tiktok_unique_id", tiktok_unique_id);
            twitch_login = j.value("twitch_login", twitch_login);
            twitch_client_id = j.value("twitch_client_id", twitch_client_id);
            twitch_client_secret = j.value("twitch_client_secret", twitch_client_secret);
            metrics_json_path = j.value("metrics_json_path", metrics_json_path);
            youtube_handle = j.value("youtube_handle", youtube_handle);
            tiktok_sessionid = j.value("tiktok_sessionid", tiktok_sessionid);
            tiktok_sessionid_ss = j.value("tiktok_sessionid_ss", tiktok_sessionid_ss);
            tiktok_tt_target_idc = j.value("tiktok_tt_target_idc", tiktok_tt_target_idc);
            overlay_font_family = j.value("overlay_font_family", overlay_font_family);
            overlay_font_size = j.value("overlay_font_size", overlay_font_size);
            overlay_text_shadow = j.value("overlay_text_shadow", overlay_text_shadow);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool Save() const
    {
        const auto path = ResolveConfigPathForWrite();
        DebugLogConfigLookup(L"Saving to", path);

        // 1) Start with existing JSON (so we preserve keys we don't explicitly manage)
        nlohmann::json j = nlohmann::json::object();

        {
            FILE* rf = nullptr;
            _wfopen_s(&rf, path.c_str(), L"rb");
            if (rf) {
                fseek(rf, 0, SEEK_END);
                long sz = ftell(rf);
                fseek(rf, 0, SEEK_SET);

                std::string data;
                data.resize(sz > 0 ? (size_t)sz : 0);
                if (sz > 0) fread(data.data(), 1, (size_t)sz, rf);
                fclose(rf);

                try {
                    if (!data.empty()) {
                        auto parsed = nlohmann::json::parse(data);
                        if (parsed.is_object()) j = std::move(parsed);
                    }
                }
                catch (...) {
                    // If config.json is malformed, fall back to empty object
                    j = nlohmann::json::object();
                }
            }
        }

        // 2) Update only the fields this app is changing
        j["tiktok_unique_id"] = tiktok_unique_id;
        j["twitch_login"] = twitch_login;
        j["twitch_client_id"] = twitch_client_id;
        j["twitch_client_secret"] = twitch_client_secret;
        if (!metrics_json_path.empty()) j["metrics_json_path"] = metrics_json_path;
        j["youtube_handle"] = youtube_handle;
        j["tiktok_sessionid"] = tiktok_sessionid;
        j["tiktok_sessionid_ss"] = tiktok_sessionid_ss;
        j["tiktok_tt_target_idc"] = tiktok_tt_target_idc;
        j["overlay_font_family"] = overlay_font_family;
        j["overlay_font_size"] = overlay_font_size;
        j["overlay_text_shadow"] = overlay_text_shadow;

        // 3) Write back
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"wb");
        if (!f) return false;

        auto out = j.dump(2);
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        return true;
    }
};
