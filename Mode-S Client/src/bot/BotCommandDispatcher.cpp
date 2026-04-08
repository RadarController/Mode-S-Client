#include "bot/BotCommandDispatcher.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AppState.h"
#include "chat/ChatAggregator.h"
#include "core/StringUtil.h"
#include "log/UiLog.h"
#include "metar/MetarCommand.h"
#include "tiktok/TikTokSidecar.h"
#include "twitch/TwitchIrcWsClient.h"
#include "youtube/YouTubeLiveChatService.h"

namespace {

std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

namespace bot {

void SubscribeBotCommandHandler(
    ChatAggregator& chat,
    AppState& state,
    TwitchIrcWsClient& twitch,
    TikTokSidecar& tiktok,
    YouTubeLiveChatService& youtubeChat)
{
    static bool botSubscribed = false;
    if (botSubscribed) return;
    botSubscribed = true;

    chat.Subscribe([
        pChat = &chat,
        pState = &state,
        pTwitch = &twitch,
        pTikTok = &tiktok,
        pYouTubeChat = &youtubeChat
    ](const ChatMessage& m) {
        if (m.user == "StreamingATC.Bot") return;
        if (m.message.size() < 2 || m.message[0] != '!') return;

        size_t start = 1;
        while (start < m.message.size() && std::isspace(static_cast<unsigned char>(m.message[start]))) start++;
        size_t end = start;
        while (end < m.message.size() && !std::isspace(static_cast<unsigned char>(m.message[end]))) end++;
        if (end <= start) return;

        std::string cmd_lc = ToLowerAscii(m.message.substr(start, end - start));
        if (cmd_lc.empty()) return;

        const long long now_ms_ll = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

        const AppState::BotSettings bot_settings = pState->bot_settings_snapshot();
        if (bot_settings.silent_mode) {
            return;
        }

        static std::mutex rl_mu;
        static std::unordered_map<std::string, long long> last_by_user;
        static std::unordered_map<std::string, long long> last_by_platform;

        const long long kUserGapMs = static_cast<long long>(bot_settings.per_user_gap_ms);
        const long long kPlatformGapMs = static_cast<long long>(bot_settings.per_platform_gap_ms);

        std::string platform_lc = ToLowerAscii(m.platform);
        std::string user_key = platform_lc + "|" + m.user;

        {
            std::lock_guard<std::mutex> lk(rl_mu);
            if (kPlatformGapMs > 0) {
                auto itp = last_by_platform.find(platform_lc);
                if (itp != last_by_platform.end() && (now_ms_ll - itp->second) < kPlatformGapMs) {
                    return;
                }
            }
            if (kUserGapMs > 0) {
                auto itu = last_by_user.find(user_key);
                if (itu != last_by_user.end() && (now_ms_ll - itu->second) < kUserGapMs) {
                    return;
                }
            }
            last_by_platform[platform_lc] = now_ms_ll;
            last_by_user[user_key] = now_ms_ll;
        }

        auto send_reply = [&](std::string reply) {
            const size_t kMaxReplyLen = bot_settings.max_reply_len;
            if (kMaxReplyLen > 0 && reply.size() > kMaxReplyLen) {
                reply.resize(kMaxReplyLen);
            }
            if (kMaxReplyLen == 0) {
                return;
            }

            ChatMessage bot{};
            bot.platform = m.platform;
            bot.user = "StreamingATC.Bot";
            bot.message = reply;
            bot.ts_ms = static_cast<uint64_t>(now_ms_ll + 1);
            pChat->Add(std::move(bot));

            if (platform_lc == "twitch" && pTwitch) {
                if (!pTwitch->SendPrivMsg(reply)) {
                    LogLine(L"BOT: Twitch send failed");
                }
            }
            if (platform_lc == "tiktok" && pTikTok) {
                if (!pTikTok->send_chat(reply)) {
                    LogLine(L"BOT: TikTok send failed (sidecar)");
                }
            }
            if (platform_lc == "youtube" && pYouTubeChat) {
                std::string err;
                if (!pYouTubeChat->send_chat(reply, &err)) {
                    LogLine(ToW(std::string("BOT: YouTube send failed: ") + err));
                }
            }
        };

        std::string metarReply;
        std::string metarLogError;
        if (metar::TryGetMetarReply(m.message, metarReply, &metarLogError)) {
            if (!metarLogError.empty()) {
                LogLine(ToW(std::string("BOT: METAR lookup note: ") + metarLogError));
            }
            send_reply(std::move(metarReply));
            return;
        }

        std::string template_reply = pState->bot_try_get_response(
            cmd_lc,
            m.is_mod,
            m.is_broadcaster,
            now_ms_ll);
        if (template_reply.empty()) return;

        std::string reply = template_reply;
        ReplaceAll(reply, "{user}", m.user);
        ReplaceAll(reply, "{platform}", platform_lc);
        send_reply(std::move(reply));
    });
}

} // namespace bot
