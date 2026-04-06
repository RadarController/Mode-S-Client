#pragma once

#include <optional>
#include <string>
#include <vector>

namespace fenixsim {

enum class FailureState {
    Inactive,
    Armed,
    Active
};

enum class SafeWriteResult {
    Success,
    SkippedAlreadyActive,
    SkippedAlreadyArmed,
    NotFound,
    VerificationFailed,
    TransportError,
    InvalidResponse
};

struct ArmedFailureCondition {
    std::optional<int> ias;
    std::optional<int> alt_above_amsl;
    std::optional<int> alt_below_amsl;
    std::optional<std::string> time;
    std::optional<std::string> after_event;
    std::optional<int> after_event_seconds;

    bool Empty() const;
};

struct Failure {
    int ata_id = 0;
    std::string ata_title;
    std::string ata_short_title;
    std::string group_name;

    std::string id;
    std::string title;

    bool failed = false;
    std::optional<ArmedFailureCondition> failure_condition;

    [[nodiscard]] FailureState State() const;
    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool IsArmed() const;
    // Option A semantics: any failure with a non-null failureCondition is treated
    // as already "in flight" for random-selection purposes, because it is already
    // armed/scheduled and should not be picked again.
    [[nodiscard]] bool IsInFlight() const;
    [[nodiscard]] bool IsInactive() const;
};

class FenixSimFailuresClient {
public:
    explicit FenixSimFailuresClient(std::string host = "localhost", int port = 8083);

    bool FetchManualFailures(std::vector<Failure>& out_failures, std::string* error = nullptr) const;

    bool TriggerFailureNow(const std::string& failure_id, std::string* error = nullptr) const;
    bool ClearFailure(const std::string& failure_id, std::string* error = nullptr) const;
    bool ClearArmedFailure(const std::string& failure_id, std::string* error = nullptr) const;
    bool ArmFailure(const std::string& failure_id,
                    const ArmedFailureCondition& condition,
                    std::string* error = nullptr) const;

    bool TriggerFailureNowIfInactive(const std::string& failure_id,
                                     SafeWriteResult& result,
                                     std::string* error = nullptr) const;

    bool ArmFailureIfInactive(const std::string& failure_id,
                              const ArmedFailureCondition& condition,
                              SafeWriteResult& result,
                              std::string* error = nullptr) const;

private:
    std::string host_;
    int port_;

    bool HttpGetJson(const std::wstring& path, std::string& response_body, std::string* error) const;
    bool HttpPostJson(const std::wstring& path,
                      const std::string& request_body,
                      std::string& response_body,
                      std::string* error) const;

    bool PostImmediateFailure(const std::string& failure_id,
                              bool failed,
                              std::string* error) const;

    bool FindFailureById(const std::vector<Failure>& failures,
                         const std::string& failure_id,
                         Failure& out_failure) const;
};

} // namespace fenixsim
