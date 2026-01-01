#include "youtube/YouTubeLiveChatService.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <chrono>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#include "json.hpp"
#include "chat/ChatAggregator.h"
#include "AppState.h"

using json = nlohmann::json;

static uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

struct HttpResult {
    int status = 0;
    DWORD winerr = 0;
    std::string body;
};

static HttpResult WinHttpRequestUtf8(const std::wstring& method,
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& headers,
    const std::string& body,
    bool secure)
{
    HttpResult r;
    DWORD status = 0; DWORD statusSize = sizeof(status);
    std::string out;

    HINTERNET hSession = WinHttpOpen(L"Mode-S Client/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { r.winerr = GetLastError(); return r; }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { r.winerr = GetLastError(); WinHttpCloseHandle(hSession); return r; }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { r.winerr = GetLastError(); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return r; }

    // Reasonable timeouts for polling
    WinHttpSetTimeouts(hRequest, 5000, 5000, 8000, 8000);

    if (!headers.empty()) {
        WinHttpAddRequestHeaders(hRequest, headers.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0);

    if (!ok) { r.winerr = GetLastError(); goto done; }

    ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) { r.winerr = GetLastError(); goto done; }

    if (WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
    {
        r.status = (int)status;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) { r.winerr = GetLastError(); break; }
        if (avail == 0) break;
        size_t cur = out.size();
        out.resize(cur + avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, out.data() + cur, avail, &read)) { r.winerr = GetLastError(); break; }
        out.resize(cur + read);
    }

    r.body = std::move(out);

done:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return r;
}

static std::string Trim(const std::string& s) {
    auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t a = 0, b = s.size();
    while (a < b && is_space((unsigned char)s[a])) a++;
    while (b > a && is_space((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string EnsureAtHandle(std::string h) {
    h = Trim(h);
    if (h.empty()) return h;
    // If user pasted full URL, try to extract @handle part.
    auto atPos = h.find("@");
    if (atPos != std::string::npos) {
        // take from '@' to end-ish, stop at first slash or whitespace
        size_t end = h.find_first_of(" \t\r\n?", atPos);
        std::string out = h.substr(atPos, (end == std::string::npos) ? std::string::npos : (end - atPos));
        // strip trailing slash
        while (!out.empty() && out.back() == '/') out.pop_back();
        return out;
    }
    // If they entered UC channel id or plain name, leave as-is; we’ll try both paths below.
    return h;
}

static bool FindFirstBetween(const std::string& hay, const std::string& a, const std::string& b, std::string& out) {
    size_t p = hay.find(a);
    if (p == std::string::npos) return false;
    p += a.size();
    size_t q = hay.find(b, p);
    if (q == std::string::npos) return false;
    out = hay.substr(p, q - p);
    return true;
}

static bool ExtractFirstJsonValueString(const std::string& hay, const std::string& key, std::string& out) {
    // crude: finds `"key":"VALUE"` and returns VALUE (not fully JSON-escaped-safe, but works for these tokens)
    std::string needle = "\"" + key + "\":\"";
    size_t p = hay.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    size_t q = hay.find("\"", p);
    if (q == std::string::npos) return false;
    out = hay.substr(p, q - p);
    return true;
}

// Recursively walk JSON to collect live chat text message renderers.
static void ExtractChatMessages(const json& j, std::vector<ChatMessage>& outMsgs) {
    if (j.is_object()) {
        // Look for liveChatTextMessageRenderer
        auto it = j.find("liveChatTextMessageRenderer");
        if (it != j.end() && it->is_object()) {
            const json& r = *it;
            ChatMessage m{};
            m.platform = "youtube";
            m.ts_ms = NowMs();

            // authorName.simpleText
            try {
                if (r.contains("authorName") && r["authorName"].contains("simpleText"))
                    m.user = r["authorName"]["simpleText"].get<std::string>();
            }
            catch (...) {}

            // message.runs -> concat text and emoji shortcuts
            try {
                if (r.contains("message") && r["message"].contains("runs") && r["message"]["runs"].is_array()) {
                    std::string msg;
                    for (const auto& run : r["message"]["runs"]) {
                        if (run.contains("text")) {
                            msg += run["text"].get<std::string>();
                        }
                        else if (run.contains("emoji")) {
                            const auto& e = run["emoji"];
                            if (e.contains("shortcuts") && e["shortcuts"].is_array() && !e["shortcuts"].empty()) {
                                msg += e["shortcuts"][0].get<std::string>();
                            }
                            else if (e.contains("emojiId")) {
                                msg += ":" + e["emojiId"].get<std::string>() + ":";
                            }
                            else {
                                msg += "�";
                            }
                        }
                    }
                    m.message = msg;
                }
            }
            catch (...) {}

            if (!m.user.empty() && !m.message.empty()) {
                outMsgs.push_back(std::move(m));
            }
        }

        // Recurse
        for (auto it2 = j.begin(); it2 != j.end(); ++it2) {
            ExtractChatMessages(it2.value(), outMsgs);
        }
    }
    else if (j.is_array()) {
        for (const auto& v : j) ExtractChatMessages(v, outMsgs);
    }
}

// Find next continuation token and timeoutMs from response.
static bool ExtractContinuationAndTimeout(const json& j, std::string& continuation, int& timeoutMs) {
    // Typical shape:
    // continuationContents.liveChatContinuation.continuations[0].timedContinuationData.{continuation,timeoutMs}
    try {
        if (j.contains("continuationContents") &&
            j["continuationContents"].contains("liveChatContinuation"))
        {
            const auto& lc = j["continuationContents"]["liveChatContinuation"];
            if (lc.contains("continuations") && lc["continuations"].is_array() && !lc["continuations"].empty()) {
                const auto& c0 = lc["continuations"][0];
                if (c0.contains("timedContinuationData")) {
                    const auto& t = c0["timedContinuationData"];
                    if (t.contains("continuation")) continuation = t["continuation"].get<std::string>();
                    if (t.contains("timeoutMs")) timeoutMs = t["timeoutMs"].get<int>();
                    return !continuation.empty();
                }
                if (c0.contains("invalidationContinuationData")) {
                    const auto& t = c0["invalidationContinuationData"];
                    if (t.contains("continuation")) continuation = t["continuation"].get<std::string>();
                    if (t.contains("timeoutMs")) timeoutMs = t.value("timeoutMs", timeoutMs);
                    return !continuation.empty();
                }
            }
        }
    }
    catch (...) {}
    return false;
}

YouTubeLiveChatService::YouTubeLiveChatService() {}
YouTubeLiveChatService::~YouTubeLiveChatService() { stop(); }

bool YouTubeLiveChatService::start(const std::string& youtube_handle_or_channel,
    ChatAggregator& chat,
    LogFn log)
{
    if (running_.load()) return false;
    if (Trim(youtube_handle_or_channel).empty()) return false;

    running_.store(true);
    thread_ = std::thread(&YouTubeLiveChatService::worker, this, youtube_handle_or_channel, &chat, std::move(log));
    return true;
}

void YouTubeLiveChatService::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void YouTubeLiveChatService::worker(std::string handleIn, ChatAggregator* chat, LogFn log)
{
    auto Log = [&](const std::wstring& s) {
        if (log) log(s);
        };

    std::string handle = EnsureAtHandle(handleIn);

    Log(L"YOUTUBE: starting live chat poller for '" + ToW(handle) + L"'");

    // 1) Resolve live videoId by hitting /@handle/live (preferred) or fallback /c/<x>/live or /channel/<UC...>/live
    std::string livePath;
    if (!handle.empty() && handle[0] == '@') livePath = "/" + handle + "/live";
    else livePath = "/@" + handle + "/live"; // try to treat as handle

    HttpResult liveHtml = WinHttpRequestUtf8(L"GET", L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT,
        ToW(livePath), L"", "", true);

    if (liveHtml.status != 200 || liveHtml.body.empty()) {
        // fallback: /c/<handle>/live and /channel/<handle>/live
        std::string p1 = "/c/" + handle + "/live";
        liveHtml = WinHttpRequestUtf8(L"GET", L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT, ToW(p1), L"", "", true);
        if (liveHtml.status != 200 || liveHtml.body.empty()) {
            std::string p2 = "/channel/" + handle + "/live";
            liveHtml = WinHttpRequestUtf8(L"GET", L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT, ToW(p2), L"", "", true);
        }
    }

    if (!running_.load()) return;

    if (liveHtml.status != 200 || liveHtml.body.empty()) {
        Log(L"YOUTUBE: failed to load /live page (status=" + std::to_wstring(liveHtml.status) + L")");
        running_.store(false);
        return;
    }

    std::string videoId;
    // Most reliable cheap scrape: first "videoId":"..."
    if (!ExtractFirstJsonValueString(liveHtml.body, "videoId", videoId) || videoId.empty()) {
        Log(L"YOUTUBE: could not find videoId on /live page (are they live?)");
        running_.store(false);
        return;
    }

    Log(L"YOUTUBE: live videoId=" + ToW(videoId));

    // 2) Load live_chat page to obtain INNERTUBE_API_KEY, clientVersion, and initial continuation
    std::string chatPath = "/live_chat?is_popout=1&v=" + videoId;
    HttpResult chatHtml = WinHttpRequestUtf8(L"GET", L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT,
        ToW(chatPath), L"", "", true);

    if (!running_.load()) return;

    if (chatHtml.status != 200 || chatHtml.body.empty()) {
        Log(L"YOUTUBE: failed to load live_chat page (status=" + std::to_wstring(chatHtml.status) + L")");
        running_.store(false);
        return;
    }

    std::string apiKey;
    std::string clientVersion;
    if (!ExtractFirstJsonValueString(chatHtml.body, "INNERTUBE_API_KEY", apiKey) || apiKey.empty()) {
        Log(L"YOUTUBE: could not find INNERTUBE_API_KEY");
        running_.store(false);
        return;
    }
    if (!ExtractFirstJsonValueString(chatHtml.body, "INNERTUBE_CLIENT_VERSION", clientVersion) || clientVersion.empty()) {
        clientVersion = "2.20250101.00.00"; // fallback; usually present, but keep going if missing
    }

    // continuation often appears multiple times; the first one near liveChatRenderer is fine
    std::string continuation;
    // crude: find "liveChatRenderer" then search continuation after it
    size_t p = chatHtml.body.find("liveChatRenderer");
    if (p != std::string::npos) {
        std::string tail = chatHtml.body.substr(p);
        ExtractFirstJsonValueString(tail, "continuation", continuation);
    }
    if (continuation.empty()) {
        ExtractFirstJsonValueString(chatHtml.body, "continuation", continuation);
    }

    if (continuation.empty()) {
        Log(L"YOUTUBE: could not find initial continuation token");
        running_.store(false);
        return;
    }

    Log(L"YOUTUBE: got apiKey/clientVersion/continuation; entering poll loop.");

    // 3) Poll youtubei live chat
    int sleepMs = 1500;
    while (running_.load()) {
        json body;
        body["context"]["client"]["clientName"] = "WEB";
        body["context"]["client"]["clientVersion"] = clientVersion;
        body["continuation"] = continuation;

        std::string bodyStr = body.dump();

        std::wstring headers =
            L"Content-Type: application/json\r\n"
            L"Origin: https://www.youtube.com\r\n"
            L"Referer: https://www.youtube.com/\r\n";

        std::wstring path = ToW("/youtubei/v1/live_chat/get_live_chat?key=" + apiKey);

        HttpResult r = WinHttpRequestUtf8(L"POST",
            L"www.youtube.com",
            INTERNET_DEFAULT_HTTPS_PORT,
            path,
            headers,
            bodyStr,
            true);

        if (!running_.load()) break;

        if (r.status != 200 || r.body.empty()) {
            // Back off a bit; transient errors happen.
            Log(L"YOUTUBE: poll failed status=" + std::to_wstring(r.status) + L" winerr=" + std::to_wstring(r.winerr));
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            continue;
        }

        json j;
        try {
            j = json::parse(r.body);
        }
        catch (...) {
            Log(L"YOUTUBE: poll returned non-JSON; backing off");
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            continue;
        }

        // Extract messages
        std::vector<ChatMessage> msgs;
        ExtractChatMessages(j, msgs);
        for (auto& m : msgs) {
            if (chat) chat->Add(std::move(m));
        }

        // Update continuation + timeout
        int timeoutMs = sleepMs;
        std::string nextCont;
        if (ExtractContinuationAndTimeout(j, nextCont, timeoutMs) && !nextCont.empty()) {
            continuation = std::move(nextCont);
        }

        // clamp sleep
        if (timeoutMs < 250) timeoutMs = 250;
        if (timeoutMs > 10000) timeoutMs = 10000;

        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    }

    Log(L"YOUTUBE: stopped.");
    running_.store(false);
}