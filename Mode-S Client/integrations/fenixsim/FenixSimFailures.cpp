#include "FenixSimFailures.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
#include <winhttp.h>

#include "json.hpp"

#pragma comment(lib, "winhttp.lib")

namespace fenixsim {
namespace {

using nlohmann::json;

constexpr wchar_t kManualFailuresPath[] = L"/fenix/failures/manual";
constexpr wchar_t kSaveManualPath[] = L"/fenix/failures/saveManual";

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring output(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, output.data(), required);
    return output;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string output(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, output.data(), required, nullptr, nullptr);
    return output;
}

std::string GetLastErrorMessage(const char* prefix) {
    const DWORD last_error = GetLastError();

    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags,
                                        nullptr,
                                        last_error,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer),
                                        0,
                                        nullptr);

    std::ostringstream oss;
    oss << prefix << " (Win32=" << last_error << ")";
    if (length > 0 && buffer != nullptr) {
        oss << ": " << WideToUtf8(std::wstring(buffer, length));
    }

    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    return oss.str();
}

std::optional<ArmedFailureCondition> ParseFailureCondition(const json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }

    ArmedFailureCondition condition;
    if (value.contains("ias") && !value["ias"].is_null()) {
        condition.ias = value["ias"].get<int>();
    }
    if (value.contains("alt") && !value["alt"].is_null()) {
        condition.alt_above_amsl = value["alt"].get<int>();
    }
    if (value.contains("altb") && !value["altb"].is_null()) {
        condition.alt_below_amsl = value["altb"].get<int>();
    }
    if (value.contains("time") && !value["time"].is_null()) {
        condition.time = value["time"].get<std::string>();
    }
    if (value.contains("afterEvent") && !value["afterEvent"].is_null()) {
        condition.after_event = value["afterEvent"].get<std::string>();
    }
    if (value.contains("afterEventSeconds") && !value["afterEventSeconds"].is_null()) {
        condition.after_event_seconds = value["afterEventSeconds"].get<int>();
    }

    if (condition.Empty()) {
        return std::nullopt;
    }

    return condition;
}

json MakeFailureConditionJson(const ArmedFailureCondition& condition) {
    json payload;
    payload["ias"] = condition.ias ? json(*condition.ias) : json(nullptr);
    payload["alt"] = condition.alt_above_amsl ? json(*condition.alt_above_amsl) : json(nullptr);
    payload["altb"] = condition.alt_below_amsl ? json(*condition.alt_below_amsl) : json(nullptr);
    payload["time"] = condition.time ? json(*condition.time) : json(nullptr);
    payload["afterEvent"] = condition.after_event ? json(*condition.after_event) : json(nullptr);
    payload["afterEventSeconds"] = condition.after_event_seconds ? json(*condition.after_event_seconds) : json(nullptr);
    return payload;
}

bool AppendBytes(HINTERNET request, std::string& output, std::string* error) {
    output.clear();

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            if (error != nullptr) {
                *error = GetLastErrorMessage("WinHttpQueryDataAvailable failed");
            }
            return false;
        }

        if (available == 0) {
            break;
        }

        std::string chunk(static_cast<size_t>(available), '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &bytes_read)) {
            if (error != nullptr) {
                *error = GetLastErrorMessage("WinHttpReadData failed");
            }
            return false;
        }

        chunk.resize(static_cast<size_t>(bytes_read));
        output.append(chunk);
    }

    return true;
}

bool EnsureSuccessStatus(HINTERNET request, std::string* error) {
    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpQueryHeaders failed");
        }
        return false;
    }

    if (status_code >= 200 && status_code < 300) {
        return true;
    }

    if (error != nullptr) {
        std::ostringstream oss;
        oss << "Unexpected HTTP status " << status_code;
        *error = oss.str();
    }
    return false;
}

} // namespace

bool ArmedFailureCondition::Empty() const {
    return !ias.has_value() &&
           !alt_above_amsl.has_value() &&
           !alt_below_amsl.has_value() &&
           !time.has_value() &&
           !after_event.has_value() &&
           !after_event_seconds.has_value();
}

FailureState Failure::State() const {
    if (failed) {
        return FailureState::Active;
    }
    if (failure_condition.has_value()) {
        return FailureState::Armed;
    }
    return FailureState::Inactive;
}

bool Failure::IsActive() const {
    return State() == FailureState::Active;
}

bool Failure::IsArmed() const {
    return State() == FailureState::Armed;
}

bool Failure::IsInactive() const {
    return State() == FailureState::Inactive;
}

FenixSimFailuresClient::FenixSimFailuresClient(std::string host, int port)
    : host_(std::move(host)), port_(port) {
}

bool FenixSimFailuresClient::FetchManualFailures(std::vector<Failure>& out_failures, std::string* error) const {
    out_failures.clear();

    std::string response_body;
    if (!HttpGetJson(kManualFailuresPath, response_body, error)) {
        return false;
    }

    try {
        const json root = json::parse(response_body);
        if (!root.is_array()) {
            if (error != nullptr) {
                *error = "Manual failures response was not a JSON array.";
            }
            return false;
        }

        for (const auto& ata_entry : root) {
            const int ata_id = ata_entry.value("id", 0);
            const std::string ata_title = ata_entry.value("title", "");
            const std::string ata_short_title = ata_entry.value("shortTitle", "");

            const auto groups_it = ata_entry.find("groups");
            if (groups_it == ata_entry.end() || !groups_it->is_array()) {
                continue;
            }

            for (const auto& group_entry : *groups_it) {
                const std::string group_name = group_entry.value("groupName", "");

                const auto failures_it = group_entry.find("failures");
                if (failures_it == group_entry.end() || !failures_it->is_array()) {
                    continue;
                }

                for (const auto& failure_entry : *failures_it) {
                    Failure failure;
                    failure.ata_id = ata_id;
                    failure.ata_title = ata_title;
                    failure.ata_short_title = ata_short_title;
                    failure.group_name = group_name;
                    failure.id = failure_entry.value("id", "");
                    failure.title = failure_entry.value("title", "");
                    failure.failed = failure_entry.value("failed", false);

                    const auto condition_it = failure_entry.find("failureCondition");
                    if (condition_it != failure_entry.end() && !condition_it->is_null()) {
                        failure.failure_condition = ParseFailureCondition(*condition_it);
                    }

                    if (!failure.id.empty()) {
                        out_failures.push_back(std::move(failure));
                    }
                }
            }
        }

        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("Failed to parse manual failures JSON: ") + ex.what();
        }
        return false;
    }
}

bool FenixSimFailuresClient::TriggerFailureNow(const std::string& failure_id, std::string* error) const {
    return PostImmediateFailure(failure_id, true, error);
}

bool FenixSimFailuresClient::ClearFailure(const std::string& failure_id, std::string* error) const {
    return PostImmediateFailure(failure_id, false, error);
}

bool FenixSimFailuresClient::ClearArmedFailure(const std::string& failure_id, std::string* error) const {
    json payload;
    payload["id"] = failure_id;
    payload["failed"] = false;
    payload["failureCondition"] = nullptr;

    std::string response_body;
    return HttpPostJson(kSaveManualPath, payload.dump(), response_body, error);
}

bool FenixSimFailuresClient::ArmFailure(const std::string& failure_id,
                                        const ArmedFailureCondition& condition,
                                        std::string* error) const {
    if (condition.Empty()) {
        if (error != nullptr) {
            *error = "ArmFailure requires at least one failure condition field.";
        }
        return false;
    }

    json payload;
    payload["id"] = failure_id;
    payload["failed"] = false;
    payload["failureCondition"] = MakeFailureConditionJson(condition);

    std::string response_body;
    return HttpPostJson(kSaveManualPath, payload.dump(), response_body, error);
}

bool FenixSimFailuresClient::TriggerFailureNowIfInactive(const std::string& failure_id,
                                                         SafeWriteResult& result,
                                                         std::string* error) const {
    result = SafeWriteResult::TransportError;

    std::vector<Failure> failures;
    if (!FetchManualFailures(failures, error)) {
        return false;
    }

    Failure current;
    if (!FindFailureById(failures, failure_id, current)) {
        result = SafeWriteResult::NotFound;
        return true;
    }

    if (current.IsActive()) {
        result = SafeWriteResult::SkippedAlreadyActive;
        return true;
    }

    if (current.IsArmed()) {
        result = SafeWriteResult::SkippedAlreadyArmed;
        return true;
    }

    if (!TriggerFailureNow(failure_id, error)) {
        result = SafeWriteResult::TransportError;
        return false;
    }

    std::vector<Failure> verify_failures;
    if (!FetchManualFailures(verify_failures, error)) {
        result = SafeWriteResult::VerificationFailed;
        return false;
    }

    Failure verified;
    if (!FindFailureById(verify_failures, failure_id, verified)) {
        result = SafeWriteResult::VerificationFailed;
        return false;
    }

    if (!verified.IsActive()) {
        result = SafeWriteResult::VerificationFailed;
        if (error != nullptr) {
            *error = "Failure write completed but verification did not show the target failure as active.";
        }
        return false;
    }

    result = SafeWriteResult::Success;
    return true;
}

bool FenixSimFailuresClient::ArmFailureIfInactive(const std::string& failure_id,
                                                  const ArmedFailureCondition& condition,
                                                  SafeWriteResult& result,
                                                  std::string* error) const {
    result = SafeWriteResult::TransportError;

    std::vector<Failure> failures;
    if (!FetchManualFailures(failures, error)) {
        return false;
    }

    Failure current;
    if (!FindFailureById(failures, failure_id, current)) {
        result = SafeWriteResult::NotFound;
        return true;
    }

    if (current.IsActive()) {
        result = SafeWriteResult::SkippedAlreadyActive;
        return true;
    }

    if (current.IsArmed()) {
        result = SafeWriteResult::SkippedAlreadyArmed;
        return true;
    }

    if (!ArmFailure(failure_id, condition, error)) {
        result = SafeWriteResult::TransportError;
        return false;
    }

    std::vector<Failure> verify_failures;
    if (!FetchManualFailures(verify_failures, error)) {
        result = SafeWriteResult::VerificationFailed;
        return false;
    }

    Failure verified;
    if (!FindFailureById(verify_failures, failure_id, verified)) {
        result = SafeWriteResult::VerificationFailed;
        return false;
    }

    if (!verified.IsArmed()) {
        result = SafeWriteResult::VerificationFailed;
        if (error != nullptr) {
            *error = "Failure arm write completed but verification did not show the target failure as armed.";
        }
        return false;
    }

    result = SafeWriteResult::Success;
    return true;
}

bool FenixSimFailuresClient::HttpGetJson(const std::wstring& path,
                                         std::string& response_body,
                                         std::string* error) const {
    response_body.clear();

    const std::wstring user_agent = L"Mode-S Client FenixSim Integration/1.0";
    const std::wstring wide_host = Utf8ToWide(host_);

    HINTERNET session = WinHttpOpen(user_agent.c_str(),
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (session == nullptr) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpOpen failed");
        }
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, wide_host.c_str(), static_cast<INTERNET_PORT>(port_), 0);
    if (connection == nullptr) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpConnect failed");
        }
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection,
                                           L"GET",
                                           path.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           0);
    if (request == nullptr) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpOpenRequest failed");
        }
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const wchar_t* headers = L"Accept: application/json\r\n";
    const BOOL sent = WinHttpSendRequest(request,
                                         headers,
                                         static_cast<DWORD>(-1L),
                                         WINHTTP_NO_REQUEST_DATA,
                                         0,
                                         0,
                                         0);
    if (!sent) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpSendRequest failed");
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpReceiveResponse failed");
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const bool ok = EnsureSuccessStatus(request, error) && AppendBytes(request, response_body, error);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool FenixSimFailuresClient::HttpPostJson(const std::wstring& path,
                                          const std::string& request_body,
                                          std::string& response_body,
                                          std::string* error) const {
    response_body.clear();

    const std::wstring user_agent = L"Mode-S Client FenixSim Integration/1.0";
    const std::wstring wide_host = Utf8ToWide(host_);

    HINTERNET session = WinHttpOpen(user_agent.c_str(),
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (session == nullptr) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpOpen failed");
        }
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, wide_host.c_str(), static_cast<INTERNET_PORT>(port_), 0);
    if (connection == nullptr) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpConnect failed");
        }
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection,
                                           L"POST",
                                           path.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           0);
    if (request == nullptr) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpOpenRequest failed");
        }
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    const BOOL sent = WinHttpSendRequest(request,
                                         headers.c_str(),
                                         static_cast<DWORD>(-1L),
                                         reinterpret_cast<LPVOID>(const_cast<char*>(request_body.data())),
                                         static_cast<DWORD>(request_body.size()),
                                         static_cast<DWORD>(request_body.size()),
                                         0);
    if (!sent) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpSendRequest failed");
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        if (error != nullptr) {
            *error = GetLastErrorMessage("WinHttpReceiveResponse failed");
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const bool ok = EnsureSuccessStatus(request, error) && AppendBytes(request, response_body, error);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool FenixSimFailuresClient::PostImmediateFailure(const std::string& failure_id,
                                                  bool failed,
                                                  std::string* error) const {
    json payload;
    payload["id"] = failure_id;
    payload["failed"] = failed;

    std::string response_body;
    return HttpPostJson(kSaveManualPath, payload.dump(), response_body, error);
}

bool FenixSimFailuresClient::FindFailureById(const std::vector<Failure>& failures,
                                             const std::string& failure_id,
                                             Failure& out_failure) const {
    const auto it = std::find_if(failures.begin(),
                                 failures.end(),
                                 [&failure_id](const Failure& failure) {
                                     return failure.id == failure_id;
                                 });

    if (it == failures.end()) {
        return false;
    }

    out_failure = *it;
    return true;
}

} // namespace fenixsim
