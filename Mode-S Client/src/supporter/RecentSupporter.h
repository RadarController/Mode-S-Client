#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

namespace supporter {

struct RecentSupporter {
    std::string platform;       // twitch | youtube
    std::string id;
    std::string display_name;
    std::int64_t supported_at_ms = 0;
    std::string support_type;   // subscriber | member
    std::string tier_name;
    bool is_gift = false;
    bool is_renewal = false;
};

struct SourceStatus {
    bool ok = false;
    bool connected = false;
    std::string error;
    int item_count = 0;
};

struct FetchResult {
    std::vector<RecentSupporter> items;
    SourceStatus status;
};

struct FeedResult {
    std::vector<RecentSupporter> items;
    SourceStatus twitch;
    SourceStatus youtube;
};

nlohmann::json ToJson(const RecentSupporter& item);
nlohmann::json ToJson(const SourceStatus& status);
nlohmann::json ToJson(const FeedResult& feed);

} // namespace supporter
