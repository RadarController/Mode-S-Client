#pragma once

#include <functional>
#include <optional>
#include <string>

#include "supporter/RecentSupporter.h"

class AppState;

namespace twitch {

class TwitchSupporterProvider {
public:
    using AccessTokenFn = std::function<std::optional<std::string>()>;
    using LogFn = std::function<void(const std::wstring&)>;

    TwitchSupporterProvider(AppState& state,
                            std::string login,
                            AccessTokenFn access_token,
                            std::function<std::optional<std::string>()> client_id,
                            LogFn log);

    supporter::FetchResult FetchRecent(int limit) const;

private:
    AppState& state_;
    std::string login_;
    AccessTokenFn access_token_;
    std::function<std::optional<std::string>()> client_id_;
    LogFn log_;
};

} // namespace twitch
