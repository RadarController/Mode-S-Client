#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace youtube {

struct RecentSubscriber {
    std::string subscription_id;
    std::string subscriber_channel_id;
    std::string subscriber_title;
    std::string subscriber_avatar_url;
    std::int64_t subscribed_at_ms = 0;
};

class YouTubeSubscriberProvider {
public:
    using AccessTokenFn = std::function<std::optional<std::string>()>;
    using LogFn = std::function<void(const std::wstring&)>;

    YouTubeSubscriberProvider(AccessTokenFn access_token, LogFn log);

    std::vector<RecentSubscriber> FetchRecent(int limit,
                                              std::string* out_error = nullptr) const;

private:
    AccessTokenFn access_token_;
    LogFn log_;
};

} // namespace youtube
