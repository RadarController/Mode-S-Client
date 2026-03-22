#pragma once

#include <functional>
#include <optional>
#include <string>

#include "supporter/RecentSupporter.h"

class AppState;

namespace supporter {

class SupporterFeedService {
public:
    using AccessTokenFn = std::function<std::optional<std::string>()>;
    using StringFn = std::function<std::optional<std::string>()>;
    using LogFn = std::function<void(const std::wstring&)>;

    SupporterFeedService(AppState& state,
                         std::string twitch_login,
                         AccessTokenFn twitch_access_token,
                         StringFn twitch_client_id,
                         AccessTokenFn youtube_access_token,
                         StringFn youtube_channel_id,
                         LogFn log);

    FeedResult FetchRecent(int limit) const;

private:
    AppState& state_;
    std::string twitch_login_;
    AccessTokenFn twitch_access_token_;
    StringFn twitch_client_id_;
    AccessTokenFn youtube_access_token_;
    StringFn youtube_channel_id_;
    LogFn log_;
};

} // namespace supporter
