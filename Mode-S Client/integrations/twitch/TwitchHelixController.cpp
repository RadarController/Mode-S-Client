#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "twitch/TwitchHelixController.h"

#include "AppConfig.h"
#include "AppState.h"
#include "core/StringUtil.h"
#include "twitch/TwitchHelixService.h"
#include "log/UiLog.h"

namespace TwitchHelixController {

void RestartPoller(Dependencies& deps, const std::string& reason)
{
    const std::string login = deps.config.twitch_login;
    if (login.empty()) return;

    if (deps.boundLogin == login && deps.thread.joinable()) {
        return;
    }

    LogLine(L"TWITCH: restarting Helix poller (" + ToW(reason) + L")");

    if (deps.thread.joinable()) {
        deps.running = false;
        deps.thread.join();
    }

    deps.state.set_twitch_viewers(0);
    deps.state.set_twitch_followers(0);
    deps.state.set_twitch_live(false);

    deps.running = true;
    deps.boundLogin = login;

    deps.thread = StartTwitchHelixPoller(
        deps.hwnd,
        deps.config,
        deps.state,
        deps.running,
        0,
        TwitchHelixUiCallbacks{
            [](const std::wstring& s) { LogLine(s); },
            [&](const std::wstring& /*s*/) {},
            [&](bool /*live*/) {},
            [&](int /*v*/) {},
            [&](int /*f*/) {}
        }
    );
}

} // namespace TwitchHelixController
