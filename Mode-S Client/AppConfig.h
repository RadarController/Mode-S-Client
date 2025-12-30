#pragma once
#include <string>
#include <windows.h>
#include <filesystem>
#include "json.hpp"

struct AppConfig
{
    std::string tiktok_unique_id;  // no '@'
    std::string twitch_login;      // login (lowercase is safest)
    std::string youtube_handle;    // with '@'

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
            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool Save() const
    {
        auto j = nlohmann::json{
            {"tiktok_unique_id", tiktok_unique_id},
            {"twitch_login", twitch_login},
            {"youtube_handle", youtube_handle}
        };
        const auto path = ConfigPath();
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"wb");
        if (!f) return false;

        auto out = j.dump(2);
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        return true;
    }
};
