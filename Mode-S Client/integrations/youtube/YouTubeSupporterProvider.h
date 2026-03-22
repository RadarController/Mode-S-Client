#pragma once

#include <functional>
#include <optional>
#include <string>

#include "supporter/RecentSupporter.h"

namespace youtube {

class YouTubeSupporterProvider {
public:
    using AccessTokenFn = std::function<std::optional<std::string>()>;
    using StringFn = std::function<std::optional<std::string>()>;
    using LogFn = std::function<void(const std::wstring&)>;

    YouTubeSupporterProvider(AccessTokenFn access_token,
                             StringFn channel_id,
                             LogFn log);

    supporter::FetchResult FetchRecent(int limit) const;

private:
    AccessTokenFn access_token_;
    StringFn channel_id_;
    LogFn log_;
};

} // namespace youtube
