#pragma once
#include <mutex>
#include <string>
#include <string_view>
#include <cstdint>
#include "json.hpp"

// Owns EuroScope ingest state and exposes a small JSON merge payload for /api/metrics.
class EuroScopeIngestService {
public:
    // Ingest raw JSON POST body from EuroScope plugin.
    // Requires "ts_ms" (epoch ms) so Mode-S Client can calculate freshness.
    bool Ingest(std::string_view body, std::string& err);

    // Returns an object suitable for j.update(...):
    //  { "euroscope": {...}, "euroscope_ts_ms": <uint64>, "euroscope_connected": <bool> }
    nlohmann::json Metrics(uint64_t now_ms) const;

private:
    mutable std::mutex mtx_;
    nlohmann::json last_payload_ = nlohmann::json::object();
    uint64_t last_ts_ms_ = 0;

    static constexpr uint64_t FRESH_MS = 5000;
};
