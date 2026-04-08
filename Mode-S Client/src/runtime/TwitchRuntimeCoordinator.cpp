#include "runtime/TwitchRuntimeCoordinator.h"

#include "AppConfig.h"
#include "AppState.h"
#include "chat/ChatAggregator.h"
#include "core/StringUtil.h"
#include "log/UiLog.h"
#include "platform/PlatformControl.h"
#include "twitch/TwitchAuth.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchIrcWsClient.h"

namespace runtime {

void StartTwitchRuntimeServices(
    const std::function<void(const std::string&)>& restartTwitchHelixPoller,
    TwitchAuth& twitchAuth,
    AppConfig& config,
    TwitchEventSubWsClient& twitchEventSub,
    TwitchIrcWsClient& twitch,
    AppState& state,
    ChatAggregator& chat)
{
    LogLine(L"TWITCH: starting Helix poller thread");
    if (restartTwitchHelixPoller) {
        restartTwitchHelixPoller("init");
    }
    else {
        LogLine(L"TWITCH: Helix poller restart callback is not wired.");
    }

    LogLine(L"TWITCH: refreshing OAuth token...");
    twitchAuth.on_tokens_updated = [&config, &twitchEventSub, &twitch, &state, &chat](
        const std::string& access,
        const std::string& /*refresh*/,
        const std::string& login) {
            const std::string effective_login = !login.empty() ? login : config.twitch_login;
            LogLine(L"TWITCH: tokens updated - refreshing EventSub token and restarting IRC");

            config.twitch_login = effective_login;
            twitchEventSub.UpdateAccessToken(access);

            PlatformControl::StartOrRestartTwitchIrc(
                twitch,
                state,
                chat,
                effective_login,
                access,
                [](const std::wstring& s) { LogLine(s); });
        };

    if (!twitchAuth.Start()) {
        LogLine(L"TWITCH: OAuth token refresh/start failed (check embedded Twitch credentials and twitch.user_refresh_token)");
    }
    else {
        LogLine(L"TWITCH: OAuth token refresh worker started");
    }
}

} // namespace runtime
