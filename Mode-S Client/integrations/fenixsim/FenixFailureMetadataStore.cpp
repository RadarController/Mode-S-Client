#include "fenixsim/FenixFailureMetadataStore.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <windows.h>

#include "json.hpp"

namespace fenixsim {
namespace {

using nlohmann::json;

std::int64_t NowMs() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    const auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

bool JsonBoolLoose(const json& obj, const char* key, bool fallback) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj.at(key);
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int>() != 0;
    if (v.is_string()) {
        const auto s = v.get<std::string>();
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    }
    return fallback;
}

int JsonIntLoose(const json& obj, const char* key, int fallback) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj.at(key);
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number()) return static_cast<int>(v.get<double>());
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::int64_t JsonInt64Loose(const json& obj, const char* key, std::int64_t fallback) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj.at(key);
    if (v.is_number_integer()) return v.get<std::int64_t>();
    if (v.is_number()) return static_cast<std::int64_t>(v.get<double>());
    if (v.is_string()) {
        try {
            return std::stoll(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string JsonStringLoose(const json& obj, const char* key, const std::string& fallback) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj.at(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<std::int64_t>());
    if (v.is_number()) return std::to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return fallback;
}

json MakeCatalogJson(const Failure& failure) {
    json out = json::object();
    out["ata_id"] = failure.ata_id;
    out["ata_title"] = failure.ata_title;
    out["ata_short_title"] = failure.ata_short_title;
    out["group_name"] = failure.group_name;
    out["title"] = failure.title;
    return out;
}

json MakeDefaultEntry(const Failure& failure, std::int64_t now_ms) {
    json entry = json::object();
    entry["enabled"] = false;
    entry["stream_safe"] = false;
    entry["severity"] = "major";
    entry["recoverability"] = "persistent";
    entry["repeatable"] = false;
    entry["cooldown_ms"] = 15LL * 60LL * 1000LL;
    entry["max_per_session"] = 1;
    entry["weight"] = 1;
    entry["catalog"] = MakeCatalogJson(failure);

    json availability = json::object();
    availability["present_in_current_catalog"] = true;
    availability["first_seen_unix_ms"] = now_ms;
    availability["last_seen_unix_ms"] = now_ms;
    availability["missing_since_unix_ms"] = nullptr;
    entry["availability"] = std::move(availability);
    return entry;
}

bool NormalizeEntry(json& entry,
                    const Failure* failure,
                    std::int64_t now_ms,
                    bool present_in_catalog,
                    std::size_t& normalized_entries) {
    if (!entry.is_object()) {
        if (failure != nullptr) {
            entry = MakeDefaultEntry(*failure, now_ms);
        } else {
            entry = json::object();
        }
        ++normalized_entries;
    }

    const bool enabled = JsonBoolLoose(entry, "enabled", false);
    const bool stream_safe = JsonBoolLoose(entry, "stream_safe", false);
    const std::string severity = JsonStringLoose(entry, "severity", "major");
    const std::string recoverability = JsonStringLoose(entry, "recoverability", "persistent");
    const bool repeatable = JsonBoolLoose(entry, "repeatable", false);
    const std::int64_t cooldown_ms = JsonInt64Loose(entry, "cooldown_ms", 15LL * 60LL * 1000LL);
    const int max_per_session = JsonIntLoose(entry, "max_per_session", 1);
    const int weight = (std::max)(1, JsonIntLoose(entry, "weight", 1));

    entry["enabled"] = enabled;
    entry["stream_safe"] = stream_safe;
    entry["severity"] = severity;
    entry["recoverability"] = recoverability;
    entry["repeatable"] = repeatable;
    entry["cooldown_ms"] = cooldown_ms;
    entry["max_per_session"] = max_per_session;
    entry["weight"] = weight;

    if (failure != nullptr) {
        entry["catalog"] = MakeCatalogJson(*failure);
    } else if (!entry.contains("catalog") || !entry["catalog"].is_object()) {
        entry["catalog"] = json::object();
    }

    if (!entry.contains("availability") || !entry["availability"].is_object()) {
        entry["availability"] = json::object();
        ++normalized_entries;
    }

    auto& availability = entry["availability"];
    const std::int64_t first_seen = JsonInt64Loose(availability, "first_seen_unix_ms", now_ms);
    const std::int64_t last_seen = JsonInt64Loose(availability, "last_seen_unix_ms", present_in_catalog ? now_ms : 0);
    std::int64_t missing_since = 0;
    if (availability.contains("missing_since_unix_ms") && !availability["missing_since_unix_ms"].is_null()) {
        missing_since = JsonInt64Loose(availability, "missing_since_unix_ms", 0);
    }

    availability["present_in_current_catalog"] = present_in_catalog;
    availability["first_seen_unix_ms"] = first_seen > 0 ? first_seen : now_ms;
    if (present_in_catalog) {
        availability["last_seen_unix_ms"] = now_ms;
        availability["missing_since_unix_ms"] = nullptr;
    } else {
        if (last_seen > 0) {
            availability["last_seen_unix_ms"] = last_seen;
        } else {
            availability["last_seen_unix_ms"] = nullptr;
        }
        availability["missing_since_unix_ms"] = (missing_since > 0) ? missing_since : now_ms;
    }

    return true;
}

bool LoadJsonFile(const std::wstring& path, json& out, bool& exists, std::string* error) {
    exists = std::filesystem::exists(path);
    if (!exists) {
        out = json::object();
        return true;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) {
        if (error != nullptr) {
            *error = "Failed to open Fenix failure metadata file for reading.";
        }
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string data;
    data.resize(sz > 0 ? static_cast<std::size_t>(sz) : 0);
    if (sz > 0) {
        fread(data.data(), 1, static_cast<std::size_t>(sz), f);
    }
    fclose(f);

    try {
        out = data.empty() ? json::object() : json::parse(data);
        if (!out.is_object()) {
            if (error != nullptr) {
                *error = "Fenix failure metadata file root is not a JSON object.";
            }
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("Failed to parse Fenix failure metadata JSON: ") + ex.what();
        }
        return false;
    }
}

bool SaveJsonFile(const std::wstring& path, const json& root, std::string* error) {
    try {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    } catch (...) {}

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) {
        if (error != nullptr) {
            *error = "Failed to open Fenix failure metadata file for writing.";
        }
        return false;
    }

    const std::string out = root.dump(2);
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

FailureMetadataEntry ParseMetadataEntry(const Failure& failure, const json& entry) {
    FailureMetadataEntry out;
    out.enabled = JsonBoolLoose(entry, "enabled", false);
    out.stream_safe = JsonBoolLoose(entry, "stream_safe", false);
    out.severity = JsonStringLoose(entry, "severity", "major");
    out.recoverability = JsonStringLoose(entry, "recoverability", "persistent");
    out.repeatable = JsonBoolLoose(entry, "repeatable", false);
    out.cooldown_ms = JsonInt64Loose(entry, "cooldown_ms", 15LL * 60LL * 1000LL);
    out.max_per_session = JsonIntLoose(entry, "max_per_session", 1);
    out.weight = (std::max)(1, JsonIntLoose(entry, "weight", 1));

    const json catalog = entry.value("catalog", json::object());
    out.ata_id = JsonIntLoose(catalog, "ata_id", failure.ata_id);
    out.ata_title = JsonStringLoose(catalog, "ata_title", failure.ata_title);
    out.ata_short_title = JsonStringLoose(catalog, "ata_short_title", failure.ata_short_title);
    out.group_name = JsonStringLoose(catalog, "group_name", failure.group_name);
    out.title = JsonStringLoose(catalog, "title", failure.title);

    const json availability = entry.value("availability", json::object());
    out.present_in_current_catalog = JsonBoolLoose(availability, "present_in_current_catalog", true);
    out.first_seen_unix_ms = JsonInt64Loose(availability, "first_seen_unix_ms", 0);
    out.last_seen_unix_ms = JsonInt64Loose(availability, "last_seen_unix_ms", 0);
    if (availability.contains("missing_since_unix_ms") && !availability["missing_since_unix_ms"].is_null()) {
        out.missing_since_unix_ms = JsonInt64Loose(availability, "missing_since_unix_ms", 0);
    } else {
        out.missing_since_unix_ms = 0;
    }
    return out;
}

} // namespace

FenixFailureMetadataStore::FenixFailureMetadataStore(std::wstring file_name)
    : file_name_(std::move(file_name)) {
}

std::wstring FenixFailureMetadataStore::MetadataPath() const {
    return std::filesystem::path(GetExeDir()).append(file_name_).wstring();
}

bool FenixFailureMetadataStore::RefreshFromFailures(const std::vector<Failure>& failures,
                                                    FailureMetadataRefreshSummary& summary,
                                                    std::string* error) const {
    summary = FailureMetadataRefreshSummary{};
    summary.discovered_failures = failures.size();
    summary.metadata_path = MetadataPath();

    json root;
    bool exists = false;
    if (!LoadJsonFile(summary.metadata_path, root, exists, error)) {
        return false;
    }

    if (!root.contains("failures") || !root["failures"].is_object()) {
        root["failures"] = json::object();
    }

    auto& failures_obj = root["failures"];
    summary.metadata_entries_before = failures_obj.size();

    const std::int64_t now_ms = NowMs();
    std::unordered_set<std::string> seen_ids;
    seen_ids.reserve(failures.size());

    for (const auto& failure : failures) {
        if (failure.id.empty()) {
            continue;
        }

        seen_ids.insert(failure.id);

        if (!failures_obj.contains(failure.id)) {
            failures_obj[failure.id] = MakeDefaultEntry(failure, now_ms);
            ++summary.new_entries;
            continue;
        }

        auto& entry = failures_obj[failure.id];
        NormalizeEntry(entry, &failure, now_ms, true, summary.normalized_entries);
    }

    for (auto it = failures_obj.begin(); it != failures_obj.end(); ++it) {
        const std::string id = it.key();
        auto& entry = it.value();

        if (seen_ids.find(id) != seen_ids.end()) {
            // Already normalized above. New entries were fully seeded at creation time.
            if (summary.new_entries > 0 && entry.is_object()) {
                // No-op; kept to make intent explicit.
            }
            continue;
        }

        NormalizeEntry(entry, nullptr, now_ms, false, summary.normalized_entries);
        ++summary.stale_entries;
    }

    root["version"] = 1;
    root["source"] = "fenix_manual_failures";
    root["updated_at_unix_ms"] = now_ms;

    summary.metadata_entries_after = failures_obj.size();

    if (!SaveJsonFile(summary.metadata_path, root, error)) {
        return false;
    }

    return true;
}

bool FenixFailureMetadataStore::LoadMergedCatalog(const std::vector<Failure>& failures,
                                                  std::vector<MergedFailureCatalogEntry>& out_entries,
                                                  std::string* error) const {
    out_entries.clear();

    json root;
    bool exists = false;
    if (!LoadJsonFile(MetadataPath(), root, exists, error)) {
        return false;
    }

    const json failures_obj = root.value("failures", json::object());

    out_entries.reserve(failures.size());
    for (const auto& failure : failures) {
        if (failure.id.empty()) {
            continue;
        }

        MergedFailureCatalogEntry merged;
        merged.failure = failure;

        if (failures_obj.is_object() && failures_obj.contains(failure.id) && failures_obj.at(failure.id).is_object()) {
            merged.metadata = ParseMetadataEntry(failure, failures_obj.at(failure.id));
        } else {
            merged.metadata.ata_id = failure.ata_id;
            merged.metadata.ata_title = failure.ata_title;
            merged.metadata.ata_short_title = failure.ata_short_title;
            merged.metadata.group_name = failure.group_name;
            merged.metadata.title = failure.title;
            merged.metadata.present_in_current_catalog = true;
        }

        out_entries.push_back(std::move(merged));
    }

    return true;
}

} // namespace fenixsim
