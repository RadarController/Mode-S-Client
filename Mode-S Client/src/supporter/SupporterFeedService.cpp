#include "supporter/SupporterFeedService.h"

#include <algorithm>

#include "AppState.h"
#include "supporter/RecentSupporter.h"
#include "twitch/TwitchSupporterProvider.h"
#include "youtube/YouTubeSupporterProvider.h"

namespace supporter {

SupporterFeedService::SupporterFeedService(AppState& state,
                                           std::string twitch_login,
                                           AccessTokenFn twitch_access_token,
                                           StringFn twitch_client_id,
                                           AccessTokenFn youtube_access_token,
                                           StringFn youtube_channel_id,
                                           LogFn log)
    : state_(state)
    , twitch_login_(std::move(twitch_login))
    , twitch_access_token_(std::move(twitch_access_token))
    , twitch_client_id_(std::move(twitch_client_id))
    , youtube_access_token_(std::move(youtube_access_token))
    , youtube_channel_id_(std::move(youtube_channel_id))
    , log_(std::move(log)) {
}

FeedResult SupporterFeedService::FetchRecent(int limit) const {
    if (limit <= 0) limit = 16;
    if (limit > 50) limit = 50;

    twitch::TwitchSupporterProvider twitch_provider(state_, twitch_login_, twitch_access_token_, twitch_client_id_, log_);
    youtube::YouTubeSupporterProvider youtube_provider(youtube_access_token_, youtube_channel_id_, log_);

    FetchResult twitch = twitch_provider.FetchRecent(limit);
    FetchResult youtube = youtube_provider.FetchRecent(limit);

    FeedResult out;
    out.twitch = twitch.status;
    out.youtube = youtube.status;

    out.items.reserve(twitch.items.size() + youtube.items.size());
    out.items.insert(out.items.end(), twitch.items.begin(), twitch.items.end());
    out.items.insert(out.items.end(), youtube.items.begin(), youtube.items.end());

    std::sort(out.items.begin(), out.items.end(), [](const RecentSupporter& a, const RecentSupporter& b) {
        if (a.supported_at_ms != b.supported_at_ms) return a.supported_at_ms > b.supported_at_ms;
        if (a.platform != b.platform) return a.platform < b.platform;
        return a.display_name < b.display_name;
    });

    if (static_cast<int>(out.items.size()) > limit) {
        out.items.resize(static_cast<size_t>(limit));
    }

    return out;
}

} // namespace supporter
