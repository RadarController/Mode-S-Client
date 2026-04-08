#pragma once

class AppState;
class ChatAggregator;
class TikTokSidecar;
class TwitchIrcWsClient;
class YouTubeLiveChatService;

namespace bot {

void SubscribeBotCommandHandler(
    ChatAggregator& chat,
    AppState& state,
    TwitchIrcWsClient& twitch,
    TikTokSidecar& tiktok,
    YouTubeLiveChatService& youtubeChat);

} // namespace bot
