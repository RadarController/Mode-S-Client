#pragma once

#include <functional>
#include <string>

struct AppConfig;
class AppState;
class ChatAggregator;
class TwitchAuth;
class TwitchEventSubWsClient;
class TwitchIrcWsClient;

namespace runtime {

void StartTwitchRuntimeServices(
    const std::function<void(const std::string&)>& restartTwitchHelixPoller,
    TwitchAuth& twitchAuth,
    AppConfig& config,
    TwitchEventSubWsClient& twitchEventSub,
    TwitchIrcWsClient& twitch,
    AppState& state,
    ChatAggregator& chat);

} // namespace runtime
