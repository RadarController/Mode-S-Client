#include "supporter/RecentSupporter.h"

namespace supporter {

nlohmann::json ToJson(const RecentSupporter& item) {
    nlohmann::json j;
    j["platform"] = item.platform;
    j["id"] = item.id;
    j["display_name"] = item.display_name;
    j["supported_at"] = item.supported_at_ms;
    j["support_type"] = item.support_type;
    j["tier_name"] = item.tier_name;
    j["is_gift"] = item.is_gift;
    j["is_renewal"] = item.is_renewal;
    return j;
}

nlohmann::json ToJson(const SourceStatus& status) {
    nlohmann::json j;
    j["ok"] = status.ok;
    j["connected"] = status.connected;
    j["error"] = status.error;
    j["item_count"] = status.item_count;
    return j;
}

nlohmann::json ToJson(const FeedResult& feed) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : feed.items) {
        items.push_back(ToJson(item));
    }

    nlohmann::json j;
    j["items"] = std::move(items);
    j["total"] = static_cast<int>(feed.items.size());
    j["sources"] = {
        {"twitch", ToJson(feed.twitch)},
        {"youtube", ToJson(feed.youtube)}
    };
    return j;
}

} // namespace supporter
