#include "metar/MetarCommand.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <windows.h>
#include <winhttp.h>

#include "core/StringUtil.h"
#include "http/WinHttpClient.h"

namespace metar {
namespace {

std::string TrimAscii(std::string s)
{
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t a = 0;
    size_t b = s.size();
    while (a < b && is_space(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && is_space(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string ToUpperAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool IsStrictIcao(const std::string& s)
{
    if (s.size() != 4) return false;
    for (unsigned char c : s) {
        if (!std::isalpha(c)) return false;
    }
    return true;
}

struct MetarCacheEntry {
    long long cached_at_ms = 0;
    std::string raw;
};

bool TryFetchMetarFromAviationWeather(const std::string& icaoUpper,
    std::string& outRawMetar,
    std::string* outError)
{
    outRawMetar.clear();
    if (!IsStrictIcao(icaoUpper)) {
        if (outError) *outError = "Invalid ICAO";
        return false;
    }

    const std::wstring headers =
        L"Accept: text/plain\r\n"
        L"User-Agent: Mode-S Client/1.0\r\n";

    const std::wstring path =
        L"/api/data/metar?ids=" + ToW(icaoUpper) + L"&format=raw";

    const http::HttpResult r = http::WinHttpRequestUtf8(
        L"GET",
        L"aviationweather.gov",
        INTERNET_DEFAULT_HTTPS_PORT,
        path,
        headers,
        "",
        true);

    if (r.status == 204) {
        if (outError) *outError = "No METAR available for " + icaoUpper;
        return false;
    }

    if (r.status != 200) {
        if (outError) {
            *outError = "AviationWeather METAR lookup failed: HTTP " + std::to_string(r.status);
            if (!r.body.empty()) *outError += " body=" + r.body;
        }
        return false;
    }

    std::string raw = TrimAscii(r.body);
    if (raw.empty()) {
        if (outError) *outError = "AviationWeather METAR lookup returned an empty response";
        return false;
    }

    const size_t nl = raw.find_first_of("\r\n");
    if (nl != std::string::npos) {
        raw = TrimAscii(raw.substr(0, nl));
    }

    if (raw.empty()) {
        if (outError) *outError = "AviationWeather METAR lookup returned an unusable response";
        return false;
    }

    outRawMetar = raw;
    return true;
}

} // namespace

bool TryGetMetarReply(const std::string& messageText,
    std::string& outReply,
    std::string* outLogError)
{
    outReply.clear();

    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    const std::string trimmed = TrimAscii(messageText);
    if (trimmed.size() < 6 || trimmed[0] != '!') {
        return false;
    }

    size_t firstSpace = trimmed.find_first_of(" \t\r\n");
    std::string cmd = trimmed.substr(1, firstSpace == std::string::npos ? std::string::npos : firstSpace - 1);
    if (to_lower(cmd) != "metar") {
        return false;
    }

    std::string arg = (firstSpace == std::string::npos)
        ? std::string{}
        : TrimAscii(trimmed.substr(firstSpace + 1));

    if (!IsStrictIcao(arg)) {
        outReply = "Usage: !metar EGCC";
        return true;
    }

    const std::string icaoUpper = ToUpperAscii(arg);

    static std::mutex cacheMu;
    static std::unordered_map<std::string, MetarCacheEntry> cache;

    const long long nowMs = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    {
        std::lock_guard<std::mutex> lk(cacheMu);
        auto it = cache.find(icaoUpper);
        if (it != cache.end() && !it->second.raw.empty() && (nowMs - it->second.cached_at_ms) < 60000) {
            outReply = icaoUpper + " METAR: " + it->second.raw;
            return true;
        }
    }

    std::string rawMetar;
    std::string fetchError;
    if (!TryFetchMetarFromAviationWeather(icaoUpper, rawMetar, &fetchError)) {
        if (outLogError) *outLogError = fetchError;
        if (fetchError.find("No METAR available") != std::string::npos) {
            outReply = "No METAR found for " + icaoUpper;
        }
        else {
            outReply = "METAR lookup unavailable right now for " + icaoUpper;
        }
        return true;
    }

    {
        std::lock_guard<std::mutex> lk(cacheMu);
        cache[icaoUpper] = MetarCacheEntry{ nowMs, rawMetar };
    }

    outReply = rawMetar;
    return true;
}

} // namespace metar
