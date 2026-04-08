#include "bot/BotStorageBootstrap.h"

#include <filesystem>

#include "AppState.h"
#include "core/StringUtil.h"
#include "log/UiLog.h"

namespace bot {

void InitializeBotStorage(AppState& state, const std::wstring& exeDir)
{
    try {
        const std::filesystem::path botPath = std::filesystem::path(exeDir) / "bot_commands.json";
        state.set_bot_commands_storage_path(ToUtf8(botPath.wstring()));
        if (state.load_bot_commands_from_disk()) {
            LogLine(L"BOT: loaded commands from bot_commands.json");
        }
        else {
            LogLine(L"BOT: no bot_commands.json found (or empty/invalid) - starting with in-memory defaults");
        }
    }
    catch (...) {
        LogLine(L"BOT: failed to set/load bot commands storage path");
    }

    try {
        const std::filesystem::path setPath = std::filesystem::path(exeDir) / "bot_settings.json";
        state.set_bot_settings_storage_path(ToUtf8(setPath.wstring()));
        if (state.load_bot_settings_from_disk()) {
            LogLine(L"BOT: loaded settings from bot_settings.json");
        }
        else {
            LogLine(L"BOT: no bot_settings.json found (or empty/invalid) - using defaults");
        }
    }
    catch (...) {
        LogLine(L"BOT: failed to set/load bot settings storage path");
    }
}

} // namespace bot
