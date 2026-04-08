#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fenixsim/FenixSimFailures.h"

namespace fenixsim {

struct FailureMetadataEntry {
    bool enabled = false;
    bool stream_safe = false;
    std::string severity = "major";
    std::string recoverability = "persistent";
    bool repeatable = false;
    std::int64_t cooldown_ms = 15LL * 60LL * 1000LL;
    int max_per_session = 1;
    int weight = 1;

    int ata_id = 0;
    std::string ata_title;
    std::string ata_short_title;
    std::string group_name;
    std::string title;

    bool present_in_current_catalog = false;
    std::int64_t first_seen_unix_ms = 0;
    std::int64_t last_seen_unix_ms = 0;
    std::int64_t missing_since_unix_ms = 0;
};

struct MergedFailureCatalogEntry {
    Failure failure;
    FailureMetadataEntry metadata;
};

struct FailureMetadataRefreshSummary {
    std::size_t discovered_failures = 0;
    std::size_t metadata_entries_before = 0;
    std::size_t metadata_entries_after = 0;
    std::size_t new_entries = 0;
    std::size_t stale_entries = 0;
    std::size_t normalized_entries = 0;
    std::wstring metadata_path;
};

class FenixFailureMetadataStore {
public:
    explicit FenixFailureMetadataStore(std::wstring file_name = L"fenix_failure_metadata.json");

    std::wstring MetadataPath() const;

    bool RefreshFromFailures(const std::vector<Failure>& failures,
                             FailureMetadataRefreshSummary& summary,
                             std::string* error = nullptr) const;

    bool LoadMergedCatalog(const std::vector<Failure>& failures,
                           std::vector<MergedFailureCatalogEntry>& out_entries,
                           std::string* error = nullptr) const;

private:
    std::wstring file_name_;
};

} // namespace fenixsim
