#include "EuroScopeIngestService.h"

bool EuroScopeIngestService::Ingest(std::string_view body, std::string& err)
{
    try {
        auto j = nlohmann::json::parse(body.begin(), body.end());

        if (!j.contains("ts_ms") || !j["ts_ms"].is_number_unsigned()) {
            err = "missing ts_ms";
            return false;
        }

        const uint64_t ts_ms = j["ts_ms"].get<uint64_t>();

        {
            std::lock_guard<std::mutex> lk(mtx_);
            last_payload_ = std::move(j);
            last_ts_ms_ = ts_ms;
        }

        return true;
    }
    catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    catch (...) {
        err = "unknown parse error";
        return false;
    }
}

nlohmann::json EuroScopeIngestService::Metrics(uint64_t now_ms) const
{
    nlohmann::json payload;
    uint64_t ts = 0;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        payload = last_payload_;
        ts = last_ts_ms_;
    }

    const bool connected =
        (ts != 0) &&
        (now_ms >= ts) &&
        ((now_ms - ts) <= FRESH_MS);

    return nlohmann::json{
        { "euroscope", std::move(payload) },
        { "euroscope_ts_ms", ts },
        { "euroscope_connected", connected }
    };
}
