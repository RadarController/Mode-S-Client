#include "http/HttpServerOptionsBuilder.h"

#include <filesystem>
#include <optional>
#include <string>

#include "AppConfig.h"
#include "AppState.h"
#include "chat/ChatAggregator.h"
#include "core/StringUtil.h"
#include "json.hpp"
#include "log/UiLog.h"
#include "oauth/EmbeddedOAuthConfig.h"
#include "platform/PlatformControl.h"
#include "tiktok/TikTokSidecar.h"
#include "twitch/TwitchAuth.h"
#include "twitch/TwitchEventSubWsClient.h"
#include "twitch/TwitchIrcWsClient.h"
#include "youtube/YouTubeAuth.h"
#include "youtube/YouTubeLiveChatService.h"
#include "fenixsim/FenixFailureCoordinator.h"

namespace httpoptions {

HttpServer::Options BuildHttpServerOptions(
    AppBootstrap::Dependencies& deps,
    const std::wstring& exeDir,
    const std::function<void(const std::string&)>& restartTwitchHelixPoller)
{
    auto* pConfig = &deps.config;
    auto* pState = &deps.state;
    auto* pChat = &deps.chat;
    auto* pTikTok = &deps.tiktok;
    auto* pYouTube = &deps.youtube;
    auto* pTwitch = &deps.twitch;
    auto* pTwitchEventSub = &deps.twitchEventSub;
    auto* pTwitchAuth = &deps.twitchAuth;
    auto* pYouTubeAuth = &deps.youtubeAuth;
    auto* pYouTubeChat = &deps.youtubeChat;
    auto* pFenixFailureCoordinator = &deps.fenixFailureCoordinator;
    auto* pTwitchHelixBoundLogin = &deps.twitchHelixBoundLogin;

    HttpServer::Options opt;
    opt.bind_host = "127.0.0.1";
    opt.port = 17845;
    opt.overlay_root = std::filesystem::path(exeDir) / "assets" / "overlay";

    opt.start_tiktok = [pConfig, pTikTok, pState, pChat, exeDir]() -> bool {
        pConfig->tiktok_unique_id = SanitizeTikTok(pConfig->tiktok_unique_id);
        if (pConfig->tiktok_unique_id.empty()) {
            LogLine(L"TIKTOK: username is empty - refusing to start");
            return false;
        }

        return PlatformControl::StartOrRestartTikTokSidecar(
            *pTikTok,
            *pState,
            *pChat,
            exeDir,
            pConfig->tiktok_unique_id,
            [](const std::wstring& s) { LogLine(s); });
    };

    opt.stop_tiktok = [pTikTok, pState]() -> bool {
        PlatformControl::StopTikTok(*pTikTok, *pState, [](const std::wstring& s) { LogLine(s); });
        return true;
    };

    opt.start_twitch = [
        pConfig,
        pTwitchAuth,
        pTwitch,
        pState,
        pChat,
        pTwitchEventSub,
        restartTwitchHelixPoller,
        pTwitchHelixBoundLogin]() -> bool {
        pConfig->twitch_login = SanitizeTwitchLogin(pConfig->twitch_login);
        if (pConfig->twitch_login.empty()) {
            LogLine(L"TWITCH: channel login is empty - refusing to start");
            return false;
        }

        const std::string token = pTwitchAuth->GetAccessToken().value_or("");
        if (token.empty()) {
            LogLine(L"TWITCH: token not available yet (auth not ready) - refusing to start");
            return false;
        }

        if (*pTwitchHelixBoundLogin != pConfig->twitch_login) {
            if (restartTwitchHelixPoller) {
                restartTwitchHelixPoller("web dashboard start");
            }
            else {
                LogLine(L"TWITCH: Helix poller restart callback is not wired.");
            }
        }

        const bool ok = PlatformControl::StartOrRestartTwitchIrc(
            *pTwitch,
            *pState,
            *pChat,
            pConfig->twitch_login,
            token,
            [](const std::wstring& s) { LogLine(s); });

        if (!ok) return false;

        std::string eventSubClientId = pConfig->twitch_client_id;
        if (eventSubClientId.empty()) {
            eventSubClientId = EmbeddedOAuthConfig::TwitchClientId();
        }

        if (eventSubClientId.empty()) {
            LogLine(L"TWITCH: EventSub not started - missing embedded Twitch client id");
            return true;
        }

        pTwitchEventSub->Start(
            eventSubClientId,
            token,
            pConfig->twitch_login,
            [pChat](const ChatMessage& msg) {
                ChatMessage c = msg;
                c.is_event = true;
                pChat->Add(std::move(c));
            },
            [pState](const nlohmann::json& ev) { pState->add_twitch_eventsub_event(ev); },
            [pState](const nlohmann::json& st) {
                pState->set_twitch_eventsub_status(st);

                static std::uint64_t last_seq = 0;
                const std::uint64_t seq = st.value("last_error_seq", static_cast<std::uint64_t>(0));
                if (seq != 0 && seq != last_seq) {
                    last_seq = seq;
                    const std::string msg = st.value("last_error", std::string{});
                    if (!msg.empty()) pState->push_twitch_eventsub_error(msg);
                }
            });

        return true;
    };

    opt.stop_twitch = [pTwitch, pTwitchEventSub, pState]() -> bool {
        PlatformControl::StopTwitch(
            *pTwitch,
            *pTwitchEventSub,
            *pState,
            [](const std::wstring& s) { LogLine(s); });
        return true;
    };

    opt.start_youtube = [pConfig, pYouTube, pState, pChat, pYouTubeChat, exeDir]() -> bool {
        pConfig->youtube_handle = SanitizeYouTubeHandle(pConfig->youtube_handle);
        if (pConfig->youtube_handle.empty()) {
            LogLine(L"YOUTUBE: handle/channel is empty - refusing to start");
            return false;
        }

        const bool ok = PlatformControl::StartOrRestartYouTubeSidecar(
            *pYouTube,
            *pState,
            *pChat,
            exeDir,
            pConfig->youtube_handle,
            [](const std::wstring& s) { LogLine(s); });

        if (!ok) return false;

        pYouTubeChat->start(
            pConfig->youtube_handle,
            *pChat,
            [](const std::wstring& s) { LogLine(s); },
            pState);

        return true;
    };

    opt.stop_youtube = [pYouTube, pState, pYouTubeChat]() -> bool {
        PlatformControl::StopYouTube(*pYouTube, *pState, [](const std::wstring& s) { LogLine(s); });
        pYouTubeChat->stop();
        return true;
    };

    opt.twitch_auth_build_authorize_url = [pTwitchAuth](const std::string& redirect_uri, std::string* out_error) -> std::string {
        std::string err;
        const std::string url = pTwitchAuth->BuildAuthorizeUrl(redirect_uri, &err);
        if (url.empty()) {
            if (out_error) *out_error = err;
            LogLine(ToW(std::string("TWITCHAUTH: BuildAuthorizeUrl failed: ") + err));
        }
        return url;
    };

    opt.twitch_auth_handle_callback = [pTwitchAuth](
        const std::string& code,
        const std::string& state,
        const std::string& redirect_uri,
        std::string* out_error) -> bool {
        std::string err;
        const bool ok = pTwitchAuth->HandleOAuthCallback(code, state, redirect_uri, &err);
        if (!ok) {
            if (out_error) *out_error = err;
            LogLine(ToW(std::string("TWITCHAUTH: OAuth callback failed: ") + err));
        }
        return ok;
    };

    opt.youtube_auth_build_authorize_url = [pYouTubeAuth](const std::string& redirect_uri, std::string* out_error) -> std::string {
        std::string err;
        const std::string url = pYouTubeAuth->BuildAuthorizeUrl(redirect_uri, &err);
        if (url.empty()) {
            if (out_error) *out_error = err;
            LogLine(ToW(std::string("YTAUTH: BuildAuthorizeUrl failed: ") + err));
        }
        return url;
    };

    opt.youtube_auth_handle_callback = [pYouTubeAuth](
        const std::string& code,
        const std::string& state,
        const std::string& redirect_uri,
        std::string* out_error) -> bool {
        std::string err;
        const bool ok = pYouTubeAuth->HandleOAuthCallback(code, state, redirect_uri, &err);
        if (!ok) {
            if (out_error) *out_error = err;
            LogLine(ToW(std::string("YTAUTH: OAuth callback failed: ") + err));
        }
        return ok;
    };

    opt.twitch_get_access_token = [pTwitchAuth]() -> std::optional<std::string> {
        return pTwitchAuth->GetAccessToken();
    };

    opt.twitch_get_client_id = []() -> std::optional<std::string> {
        const std::string client_id = EmbeddedOAuthConfig::TwitchClientId();
        if (client_id.empty()) return std::nullopt;
        return client_id;
    };

    opt.youtube_get_access_token = [pYouTubeAuth]() -> std::optional<std::string> {
        return pYouTubeAuth->GetAccessToken();
    };

    opt.youtube_get_channel_id = [pYouTubeAuth]() -> std::optional<std::string> {
        return pYouTubeAuth->GetChannelId();
    };

    opt.youtube_auth_info_json = [pYouTubeAuth]() {
        nlohmann::json j;
        j["ok"] = true;
        j["start_url"] = "/auth/youtube/start";
        j["oauth_routes_wired"] = true;

        const auto snap = pYouTubeAuth->GetTokenSnapshot();
        j["has_refresh_token"] = snap.has_value() && !snap->refresh_token.empty();
        j["has_access_token"] = snap.has_value() && !snap->access_token.empty();
        j["expires_at_unix"] = snap.has_value() ? snap->expires_at_unix : 0;
        j["scope"] = snap.has_value() ? snap->scope_joined : "";
        j["channel_id"] = pYouTubeAuth->GetChannelId().value_or("");
        j["scopes_readable"] = std::string(YouTubeAuth::RequiredScopeReadable());
        j["scopes_encoded"] = std::string(YouTubeAuth::RequiredScopeEncoded());
        return j.dump(2);
    };

    opt.simulator_automation_status_json = [pFenixFailureCoordinator]() {
        return pFenixFailureCoordinator->StatusJson();
    };

    opt.simulator_automation_enable = [pFenixFailureCoordinator]() -> bool {
        pFenixFailureCoordinator->SetEnabled(true);
        LogLine(L"SIMAUTO: simulator automation enabled");
        return true;
    };

    opt.simulator_automation_disable = [pFenixFailureCoordinator]() -> bool {
        pFenixFailureCoordinator->SetEnabled(false);
        LogLine(L"SIMAUTO: simulator automation disabled");
        return true;
    };

    opt.simulator_automation_panic = [pFenixFailureCoordinator]() -> bool {
        pFenixFailureCoordinator->PanicStop();
        LogLine(L"SIMAUTO: simulator automation panic stop engaged");
        return true;
    };

    return opt;
}

} // namespace httpoptions
