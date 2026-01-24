#pragma once
#include <string>
#include <functional>
#include <windows.h>

#include "json.hpp"

class AppState;
class ChatAggregator;
class TikTokSidecar;
class TwitchIrcWsClient;
struct AppConfig;

namespace PlatformControl {

using LogFn = std::function<void(const std::wstring&)>;

// Starts/restarts the TikTok python sidecar using the provided username (no '@').
// Updates AppState on events and pushes chat into ChatAggregator.
bool StartOrRestartTikTokSidecar(
    TikTokSidecar& tiktok,
    AppState& state,
    ChatAggregator& chat,
    const std::wstring& exeDir,
    const std::string& tiktokUniqueId,
    HWND hwndMain,
    LogFn log);

// Starts/restarts the YouTube python sidecar using the provided handle (no '@').
// Updates AppState on events and pushes chat into ChatAggregator.
bool StartOrRestartYouTubeSidecar(
    TikTokSidecar& youtube,
    AppState& state,
    ChatAggregator& chat,
    const std::wstring& exeDir,
    const std::string& youtubeHandle,
    HWND hwndMain,
    LogFn log);

// Starts/restarts the Twitch IRC client using config.twitch_login (sanitized).
bool StartOrRestartTwitchIrc(
    TwitchIrcWsClient& twitch,
    AppState& state,
    ChatAggregator& chat,
    const std::string& twitchLogin,
    const std::string& accessTokenRaw,
    LogFn log);

// Stops helpers (also clears AppState live flags + viewers to avoid stale UI).
void StopTikTok(TikTokSidecar& tiktok, AppState& state, HWND hwndMain, UINT uiMsg, LogFn log);
void StopYouTube(TikTokSidecar& youtube, AppState& state, HWND hwndMain, UINT uiMsg, LogFn log);
void StopTwitch(TwitchIrcWsClient& twitch, AppState& state, HWND hwndMain, UINT uiMsg, LogFn log);

} // namespace PlatformControl
