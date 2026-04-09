#include "youtube/YouTubeLiveStatusService.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <thread>

#include "AppState.h"

namespace {

struct HttpResult {
    int status = 0;
    DWORD winerr = 0;
    std::string body;
};

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET v) : h(v) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h; }
    bool valid() const { return h != nullptr; }
};

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

static std::string Trim(std::string s) {
    auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t a = 0, b = s.size();
    while (a < b && is_space((unsigned char)s[a])) ++a;
    while (b > a && is_space((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string EnsureAtHandle(std::string s) {
    s = Trim(std::move(s));
    if (s.empty()) return s;
    if (s[0] == '@') return s;
    return "@" + s;
}

static HttpResult WinHttpGetUtf8(const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& extraHeaders,
    bool secure)
{
    HttpResult r;

    const std::wstring kBaseHeaders =
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        L"AppleWebKit/537.36 (KHTML, like Gecko) "
        L"Chrome/120.0.0.0 Safari/537.36\r\n"
        L"Accept: text/html,application/json;q=0.9,*/*;q=0.8\r\n"
        L"Accept-Language: en-GB,en;q=0.9,en-US;q=0.8\r\n"
        L"Accept-Encoding: identity\r\n"
        L"Connection: close\r\n";

    WinHttpHandle hSession(WinHttpOpen(L"Mozilla/5.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession.valid()) { r.winerr = GetLastError(); return r; }

    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    WinHttpHandle hConnect(WinHttpConnect(hSession, host.c_str(), port, 0));
    if (!hConnect.valid()) { r.winerr = GetLastError(); return r; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle hRequest(WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!hRequest.valid()) { r.winerr = GetLastError(); return r; }

    WinHttpAddRequestHeaders(hRequest, kBaseHeaders.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    if (!extraHeaders.empty()) {
        WinHttpAddRequestHeaders(hRequest, extraHeaders.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) { r.winerr = GetLastError(); return r; }

    ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) { r.winerr = GetLastError(); return r; }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
    {
        r.status = (int)status;
    }

    std::string out;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) { r.winerr = GetLastError(); break; }
        if (avail == 0) break;

        size_t cur = out.size();
        out.resize(cur + (size_t)avail);

        DWORD read = 0;
        if (!WinHttpReadData(hRequest, out.data() + cur, avail, &read)) { r.winerr = GetLastError(); break; }
        out.resize(cur + (size_t)read);
    }

    r.body = std::move(out);
    return r;
}

static bool LooksLikeConsentWall(const std::string& html) {
    return html.find("consent.youtube.com") != std::string::npos ||
        (html.find("CONSENT") != std::string::npos && html.find("consent") != std::string::npos);
}

static bool ExtractVideoId(const std::string& html, std::string& outVideoId) {
    outVideoId.clear();

    const std::string key = "\"videoId\"";
    size_t p = html.find(key);
    if (p != std::string::npos) {
        p += key.size();
        while (p < html.size() && std::isspace((unsigned char)html[p])) ++p;
        if (p < html.size() && html[p] == ':') ++p;
        while (p < html.size() && std::isspace((unsigned char)html[p])) ++p;
        if (p < html.size() && html[p] == '"') {
            ++p;
            std::string v;
            for (; p < html.size(); ++p) {
                const char c = html[p];
                if (c == '"') break;
                v.push_back(c);
            }
            if (v.size() == 11) {
                outVideoId = std::move(v);
                return true;
            }
        }
    }

    size_t w = html.find("watch?v=");
    if (w != std::string::npos && w + 8 + 11 <= html.size()) {
        std::string cand = html.substr(w + 8, 11);
        auto ok = [](char c) {
            return (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '_' || c == '-';
        };
        bool good = true;
        for (char c : cand) {
            if (!ok(c)) { good = false; break; }
        }
        if (good) {
            outVideoId = std::move(cand);
            return true;
        }
    }

    return false;
}

static bool ParseLive(const std::string& html) {
    if (html.empty()) return false;
    if (html.find("\"isUpcoming\":true") != std::string::npos ||
        html.find("\"upcomingEventData\"") != std::string::npos) {
        return false;
    }

    std::string videoId;
    if (!ExtractVideoId(html, videoId) || videoId.empty()) {
        return false;
    }

    if ((html.find("\"isLiveNow\":false") != std::string::npos &&
         html.find("\"isLiveNow\":true") == std::string::npos) &&
        html.find("\"isLiveContent\":true") == std::string::npos) {
        return false;
    }

    return html.find("\"isLiveNow\":true") != std::string::npos ||
        html.find("\"isLiveContent\":true") != std::string::npos;
}

static int ToIntCompact(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), ','), s.end());
    s = Trim(std::move(s));
    if (s.empty()) return 0;

    char suffix = 0;
    if (!s.empty()) {
        char c = (char)std::toupper((unsigned char)s.back());
        if (c == 'K' || c == 'M') {
            suffix = c;
            s.pop_back();
            s = Trim(std::move(s));
        }
    }

    double n = 0.0;
    try {
        n = std::stod(s);
    }
    catch (...) {
        return 0;
    }

    if (suffix == 'K') n *= 1000.0;
    else if (suffix == 'M') n *= 1000000.0;

    return (int)n;
}

static int ParseViewers(const std::string& html) {
    const std::string key = "\"concurrentViewers\"";
    size_t p = html.find(key);
    if (p != std::string::npos) {
        p += key.size();
        while (p < html.size() && std::isspace((unsigned char)html[p])) ++p;
        if (p < html.size() && html[p] == ':') ++p;
        while (p < html.size() && std::isspace((unsigned char)html[p])) ++p;
        bool quoted = false;
        if (p < html.size() && html[p] == '"') { quoted = true; ++p; }

        std::string digits;
        for (; p < html.size(); ++p) {
            char c = html[p];
            if (quoted && c == '"') break;
            if (!quoted && !std::isdigit((unsigned char)c)) break;
            if (std::isdigit((unsigned char)c)) digits.push_back(c);
        }

        if (!digits.empty()) {
            try { return std::stoi(digits); } catch (...) {}
        }
    }

    auto findWatchingNow = [&](const std::string& needle) -> int {
        size_t pos = html.find(needle);
        if (pos == std::string::npos) return 0;

        size_t start = pos;
        while (start > 0 && (std::isdigit((unsigned char)html[start - 1]) || html[start - 1] == ',' || html[start - 1] == '.')) {
            --start;
        }
        if (start < pos) {
            return ToIntCompact(html.substr(start, pos - start));
        }
        return 0;
    };

    int n = findWatchingNow(" watching now");
    if (n > 0) return n;
    n = findWatchingNow(" Watching now");
    if (n > 0) return n;

    return 0;
}

} // namespace

namespace youtube {

YouTubeLiveStatusService::YouTubeLiveStatusService() {}
YouTubeLiveStatusService::~YouTubeLiveStatusService() { Stop(); }

bool YouTubeLiveStatusService::Start(HandleFn getHandle, AppState& state, LogFn log)
{
    if (running_.exchange(true)) return false;
    thread_ = std::thread(&YouTubeLiveStatusService::worker, this, std::move(getHandle), &state, std::move(log));
    return true;
}

void YouTubeLiveStatusService::Stop()
{
    running_.store(false);
    if (thread_.joinable()) {
        try { thread_.join(); }
        catch (...) {}
    }
}

void YouTubeLiveStatusService::worker(HandleFn getHandle, AppState* state, LogFn log)
{
    auto Log = [&](const std::wstring& s) {
        if (log) log(s);
    };

    std::string lastHandle;
    bool lastLive = false;
    bool lastHadSuccess = false;
    std::string lastError;

    while (running_.load()) {
        std::string handle = EnsureAtHandle(getHandle ? getHandle() : "");
        if (handle.empty()) {
            if (state) {
                state->set_youtube_live(false);
                state->set_youtube_viewers(0);
            }
            lastHandle.clear();
            lastHadSuccess = false;
            lastError.clear();

            for (int i = 0; i < 15 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }

        const std::string livePath = "/" + handle + "/live";
        HttpResult r = WinHttpGetUtf8(
            L"www.youtube.com",
            INTERNET_DEFAULT_HTTPS_PORT,
            ToW(livePath),
            L"",
            true);

        if (r.status == 200 && !r.body.empty() && LooksLikeConsentWall(r.body)) {
            r = WinHttpGetUtf8(
                L"www.youtube.com",
                INTERNET_DEFAULT_HTTPS_PORT,
                ToW(livePath),
                L"Cookie: SOCS=CAI; CONSENT=YES+1\r\n",
                true);
        }

        if (r.status != 200 || r.body.empty()) {
            std::string err = "status=" + std::to_string(r.status) + " winerr=" + std::to_string(r.winerr);
            if (err != lastError) {
                Log(L"YOUTUBE: live status poll failed (" + ToW(err) + L")");
                lastError = err;
            }

            for (int i = 0; i < 15 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }

        const bool live = ParseLive(r.body);
        const int viewers = live ? ParseViewers(r.body) : 0;

        if (state) {
            state->set_youtube_live(live);
            state->set_youtube_viewers(viewers);
        }

        if (!lastHadSuccess || lastHandle != handle || lastLive != live) {
            Log(L"YOUTUBE: live status " + std::wstring(live ? L"live" : L"offline") +
                L" for " + ToW(handle) +
                L" (viewers=" + std::to_wstring(viewers) + L")");
        }

        lastHandle = handle;
        lastLive = live;
        lastHadSuccess = true;
        lastError.clear();

        for (int i = 0; i < 15 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

} // namespace youtube