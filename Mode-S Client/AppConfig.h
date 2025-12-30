#pragma once
#include <string>
#include <windows.h>
#include <filesystem>
#include "json.hpp"

struct AppConfig
{
    std::string overlay_font_family = "Inter";  // or "" for system default
    int         overlay_font_size = 22;
    bool        overlay_text_shadow = true;

    std::string tiktok_unique_id;  // no '@'
    std::string twitch_login;      // login (lowercase is safest)
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

    static std::wstring ConfigPath()
    {
        return GetExeDir() + L"\\config.json";
    }

    bool Load()
    {
        const auto path = ConfigPath();
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"rb");
        if (!f) return false;

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
        const auto path = ConfigPath();

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
