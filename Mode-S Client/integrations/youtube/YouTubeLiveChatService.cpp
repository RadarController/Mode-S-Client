#include "youtube/YouTubeLiveChatService.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_set>

#include "json.hpp"
#include "chat/ChatAggregator.h"
#include "AppState.h"

using json = nlohmann::json;

static std::mutex yt_seen_mu;
static std::unordered_set<std::string> yt_seen;

static uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

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
    WinHttpHandle(WinHttpHandle&& o) noexcept { h = o.h; o.h = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept {
        if (this != &o) {
            if (h) WinHttpCloseHandle(h);
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    operator HINTERNET() const { return h; }
    bool valid() const { return h != nullptr; }
};

static HttpResult WinHttpRequestUtf8(const std::wstring& method,
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& extraHeaders,
    const std::string& body,
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
    WinHttpHandle hRequest(WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!hRequest.valid()) { r.winerr = GetLastError(); return r; }

    WinHttpAddRequestHeaders(hRequest, kBaseHeaders.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    if (!extraHeaders.empty()) {
        WinHttpAddRequestHeaders(hRequest, extraHeaders.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0);

    if (!ok) { r.winerr = GetLastError(); return r; }

    ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) { r.winerr = GetLastError(); return r; }

    DWORD status = 0; DWORD statusSize = sizeof(status);
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

static std::string Trim(const std::string& s) {
    auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t a = 0, b = s.size();
    while (a < b && is_space((unsigned char)s[a])) a++;
    while (b > a && is_space((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string EnsureAtHandle(const std::string& s) {
    std::string t = Trim(s);
    if (t.empty()) return t;
    if (t[0] == '@') return t;

    auto p = t.find("/@");
    if (p != std::string::npos) {
        auto q = t.find_first_of("?#/", p + 2);
        if (q == std::string::npos) return t.substr(p + 1);
        return t.substr(p + 1, q - (p + 1));
    }
    return "@" + t;
}

// tolerant: finds `"key" : "VALUE"` with optional whitespace and escaped chars
static bool ExtractFirstJsonValueString(const std::string& hay, const std::string& key, std::string& out) {
    out.clear();

    const std::string k = "\"" + key + "\"";
    size_t p = hay.find(k);
    if (p == std::string::npos) return false;
    p += k.size();

    auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

    while (p < hay.size() && is_ws((unsigned char)hay[p])) ++p;
    if (p >= hay.size() || hay[p] != ':') return false;
    ++p;

    while (p < hay.size() && is_ws((unsigned char)hay[p])) ++p;
    if (p >= hay.size() || hay[p] != '"') return false;
    ++p;

    std::string v;
    v.reserve(64);
    bool esc = false;
    for (; p < hay.size(); ++p) {
        char c = hay[p];
        if (esc) { v.push_back(c); esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') break;
        v.push_back(c);
    }

    if (v.empty()) return false;
    out = std::move(v);
    return true;
}

// Extract a JSON object literal that appears after a marker (e.g. "ytcfg.set(" or "var ytInitialData = ").
static bool ExtractJsonObjectAfterMarker(const std::string& hay,
    const std::string& marker,
    std::string& outJson)
{
    size_t m = hay.find(marker);
    if (m == std::string::npos) return false;

    size_t p = hay.find('{', m);
    if (p == std::string::npos) return false;

    int depth = 0;
    bool inStr = false;
    char quote = 0;
    bool esc = false;

    for (size_t i = p; i < hay.size(); ++i) {
        char c = hay[i];

        if (inStr) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == quote) { inStr = false; quote = 0; continue; }
            continue;
        } else {
            if (c == '"' || c == '\'') { inStr = true; quote = c; continue; }
            if (c == '{') { depth++; }
            else if (c == '}') {
                depth--;
                if (depth == 0) {
                    outJson = hay.substr(p, i - p + 1);
                    return true;
                }
            }
        }
    }
    return false;
}

static bool FindFirstStringByKeyRecursive(const json& j, const std::string& key, std::string& out) {
    try {
        if (j.is_object()) {
            auto it = j.find(key);
            if (it != j.end() && it->is_string()) {
                out = it->get<std::string>();
                return !out.empty();
            }
            for (auto it2 = j.begin(); it2 != j.end(); ++it2) {
                if (FindFirstStringByKeyRecursive(it2.value(), key, out)) return true;
            }
        } else if (j.is_array()) {
            for (const auto& v : j) {
                if (FindFirstStringByKeyRecursive(v, key, out)) return true;
            }
        }
    } catch (...) {}
    return false;
}

static bool ExtractYtCfg(const std::string& html, std::string& apiKey, std::string& clientVersion, std::string& visitorData) {
    apiKey.clear(); clientVersion.clear(); visitorData.clear();

    std::string cfgStr;
    if (ExtractJsonObjectAfterMarker(html, "ytcfg.set(", cfgStr)) {
        try {
            json cfg = json::parse(cfgStr);
            apiKey = cfg.value("INNERTUBE_API_KEY", "");
            clientVersion = cfg.value("INNERTUBE_CLIENT_VERSION", "");
            visitorData = cfg.value("VISITOR_DATA", "");
            return !apiKey.empty();
        } catch (...) {
            // fall through
        }
    }

    ExtractFirstJsonValueString(html, "INNERTUBE_API_KEY", apiKey);
    ExtractFirstJsonValueString(html, "INNERTUBE_CLIENT_VERSION", clientVersion);
    ExtractFirstJsonValueString(html, "VISITOR_DATA", visitorData);
    return !apiKey.empty();
}

static bool ExtractInitialContinuation(const std::string& html, std::string& continuation) {
    continuation.clear();

    std::string initialStr;

    // Try several common markers/assignments
    if (!ExtractJsonObjectAfterMarker(html, "var ytInitialData", initialStr)) {
        ExtractJsonObjectAfterMarker(html, "window[\"ytInitialData\"]", initialStr);
    }
    if (initialStr.empty()) {
        ExtractJsonObjectAfterMarker(html, "ytInitialData", initialStr);
    }

    if (!initialStr.empty()) {
        try {
            json init = json::parse(initialStr);
            if (FindFirstStringByKeyRecursive(init, "continuation", continuation) && !continuation.empty()) {
                return true;
            }
        }
        catch (...) {}
    }

    size_t p = html.find("liveChatRenderer");
    if (p != std::string::npos) {
        std::string tail = html.substr(p);
        ExtractFirstJsonValueString(tail, "continuation", continuation);
    }
    if (continuation.empty()) {
        ExtractFirstJsonValueString(html, "continuation", continuation);
    }
    return !continuation.empty();
}

static bool LooksLikeConsentWall(const std::string& html) {
    return (html.find("consent.youtube.com") != std::string::npos) ||
           (html.find("CONSENT") != std::string::npos && html.find("consent") != std::string::npos);
}

static void ExtractChatMessages(const json& j, std::vector<ChatMessage>& outMsgs) {
    if (j.is_object()) {
        auto it = j.find("liveChatTextMessageRenderer");
        if (it != j.end() && it->is_object()) {
            const json& r = *it;
            ChatMessage m{};
            m.platform = "youtube";
            m.ts_ms = NowMs();

            try {
                if (r.contains("authorName") && r["authorName"].contains("simpleText"))
                    m.user = r["authorName"]["simpleText"].get<std::string>();
            } catch (...) {}

            try {
                if (r.contains("message") && r["message"].contains("runs") && r["message"]["runs"].is_array()) {
                    std::string msg;
                    for (const auto& run : r["message"]["runs"]) {
                        if (run.contains("text")) {
                            msg += run["text"].get<std::string>();
                        } else if (run.contains("emoji")) {
                            const auto& e = run["emoji"];
                            if (e.contains("shortcuts") && e["shortcuts"].is_array() && !e["shortcuts"].empty()) {
                                msg += e["shortcuts"][0].get<std::string>();
                            } else if (e.contains("emojiId")) {
                                msg += ":" + e["emojiId"].get<std::string>() + ":";
                            } else {
                                msg += "�";
                            }
                        }
                    }
                    m.message = msg;
                }
            } catch (...) {}

            if (!m.user.empty() && !m.message.empty()) outMsgs.push_back(std::move(m));
        }

        for (auto it2 = j.begin(); it2 != j.end(); ++it2) {
            ExtractChatMessages(it2.value(), outMsgs);
        }
    } else if (j.is_array()) {
        for (const auto& v : j) ExtractChatMessages(v, outMsgs);
    }
}


static void ExtractYouTubeEvents(const json& j, std::vector<EventItem>& outEvents) {
    if (j.is_object()) {
        // Super Chat
        auto it = j.find("liveChatPaidMessageRenderer");
        if (it != j.end() && it->is_object()) {
            const json& r = *it;
            EventItem e{};
            e.platform = "youtube";
            e.type = "superchat";
            e.ts_ms = (std::int64_t)NowMs();

            try {
                if (r.contains("authorName") && r["authorName"].contains("simpleText"))
                    e.user = r["authorName"]["simpleText"].get<std::string>();
            } catch (...) {}

            std::string amount;
            try {
                if (r.contains("purchaseAmountText") && r["purchaseAmountText"].contains("simpleText"))
                    amount = r["purchaseAmountText"]["simpleText"].get<std::string>();
            } catch (...) {}

            std::string msg;
            try {
                if (r.contains("message") && r["message"].contains("runs") && r["message"]["runs"].is_array()) {
                    for (const auto& run : r["message"]["runs"]) {
                        if (run.contains("text")) msg += run["text"].get<std::string>();
                    }
                }
            } catch (...) {}

            if (!amount.empty()) {
                e.message = "sent Super Chat " + amount;
                if (!msg.empty()) e.message += ": " + msg;
            } else {
                e.message = "sent Super Chat";
                if (!msg.empty()) e.message += ": " + msg;
            }

            if (!e.user.empty()) outEvents.push_back(std::move(e));
        }

        // Super Sticker
        it = j.find("liveChatPaidStickerRenderer");
        if (it != j.end() && it->is_object()) {
            const json& r = *it;
            EventItem e{};
            e.platform = "youtube";
            e.type = "supersticker";
            e.ts_ms = (std::int64_t)NowMs();

            try {
                if (r.contains("authorName") && r["authorName"].contains("simpleText"))
                    e.user = r["authorName"]["simpleText"].get<std::string>();
            } catch (...) {}

            std::string amount;
            try {
                if (r.contains("purchaseAmountText") && r["purchaseAmountText"].contains("simpleText"))
                    amount = r["purchaseAmountText"]["simpleText"].get<std::string>();
            } catch (...) {}

            e.message = amount.empty() ? "sent Super Sticker" : ("sent Super Sticker " + amount);
            if (!e.user.empty()) outEvents.push_back(std::move(e));
        }

        // Membership (new member / milestone)
        it = j.find("liveChatMembershipItemRenderer");
        if (it != j.end() && it->is_object()) {
            const json& r = *it;
            EventItem e{};
            e.platform = "youtube";
            e.type = "membership";
            e.ts_ms = (std::int64_t)NowMs();

            try {
                if (r.contains("authorName") && r["authorName"].contains("simpleText"))
                    e.user = r["authorName"]["simpleText"].get<std::string>();
            } catch (...) {}

            std::string txt;
            try {
                if (r.contains("headerSubtext") && r["headerSubtext"].contains("runs") && r["headerSubtext"]["runs"].is_array()) {
                    for (const auto& run : r["headerSubtext"]["runs"]) {
                        if (run.contains("text")) txt += run["text"].get<std::string>();
                    }
                }
            } catch (...) {}

            e.message = txt.empty() ? "became a member" : txt;
            if (!e.user.empty()) outEvents.push_back(std::move(e));
        }

        for (auto it2 = j.begin(); it2 != j.end(); ++it2) {
            ExtractYouTubeEvents(it2.value(), outEvents);
        }
    } else if (j.is_array()) {
        for (const auto& v : j) ExtractYouTubeEvents(v, outEvents);
    }
}

static bool ExtractContinuationAndTimeout(const json& j, std::string& continuation, int& timeoutMs) {
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
    } catch (...) {}
    return false;
}

YouTubeLiveChatService::YouTubeLiveChatService() {}
YouTubeLiveChatService::~YouTubeLiveChatService() { stop(); }

bool YouTubeLiveChatService::start(const std::string& youtube_handle_or_channel,
        ChatAggregator& chat,
        LogFn log,
        AppState* state)
{
    if (running_.load()) return false;
    if (Trim(youtube_handle_or_channel).empty()) return false;

    running_.store(true);
    thread_ = std::thread(&YouTubeLiveChatService::worker, this, youtube_handle_or_channel, &chat, state, std::move(log));
    return true;
}

void YouTubeLiveChatService::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void YouTubeLiveChatService::worker(std::string handleIn, ChatAggregator* chat, AppState* state, LogFn log) {
    auto Log = [&](const std::wstring& s) {
        if (log) log(s);
        };

    std::string handle = EnsureAtHandle(handleIn);
    Log(L"YOUTUBE: starting live chat poller for '" + ToW(handle) + L"'");

    // 1) Resolve live videoId by hitting /@handle/live
    std::string livePath = "/" + handle + "/live";

    HttpResult liveHtml = WinHttpRequestUtf8(L"GET", L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT,
        ToW(livePath), L"", "", true);

    if (liveHtml.status == 200 && !liveHtml.body.empty() && LooksLikeConsentWall(liveHtml.body)) {
        std::wstring cookieHdr = L"Cookie: SOCS=CAI; CONSENT=YES+1\r\n";
        liveHtml = WinHttpRequestUtf8(L"GET", L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT,
            ToW(livePath), cookieHdr, "", true);
    }

    if (!running_.load()) return;

    if (liveHtml.status != 200 || liveHtml.body.empty()) {
        Log(L"YOUTUBE: failed to load /live page (status=" + std::to_wstring(liveHtml.status) + L")");
        running_.store(false);
        return;
    }

    std::string videoId;
    if (!ExtractFirstJsonValueString(liveHtml.body, "videoId", videoId) || videoId.empty()) {
        // Fallback: look for watch?v=VIDEOID (11 chars)
        // Common patterns: /watch?v=XXXXXXXXXXX or "watch?v=XXXXXXXXXXX"
        size_t w = liveHtml.body.find("watch?v=");
        if (w != std::string::npos && w + 8 + 11 <= liveHtml.body.size()) {
            std::string cand = liveHtml.body.substr(w + 8, 11);
            auto ok = [](char c) {
                return (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '_' || c == '-';
                };
            bool good = true;
            for (char c : cand) if (!ok(c)) { good = false; break; }
            if (good) videoId = cand;
        }
    }

    if (videoId.empty()) {
        Log(L"YOUTUBE: could not find videoId on /live page (are they live?)");
        running_.store(false);
        return;
    }
    Log(L"YOUTUBE: live videoId=" + ToW(videoId));

    // 2) Load live_chat page for INNERTUBE + continuation
    std::string chatPath = "/live_chat?is_popout=1&v=" + videoId;

    HttpResult chatHtml = WinHttpRequestUtf8(L"GET", L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT,
        ToW(chatPath), L"", "", true);

    if (chatHtml.status == 200 && !chatHtml.body.empty() && LooksLikeConsentWall(chatHtml.body)) {
        std::wstring cookieHdr = L"Cookie: SOCS=CAI; CONSENT=YES+1\r\n";
        chatHtml = WinHttpRequestUtf8(L"GET", L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT,
            ToW(chatPath), cookieHdr, "", true);
    }

    if (!running_.load()) return;

    if (chatHtml.status != 200 || chatHtml.body.empty()) {
        Log(L"YOUTUBE: failed to load live_chat page (status=" + std::to_wstring(chatHtml.status) + L")");
        running_.store(false);
        return;
    }

    std::string apiKey, clientVersion, visitorData;
    if (!ExtractYtCfg(chatHtml.body, apiKey, clientVersion, visitorData) || apiKey.empty()) {
        Log(L"YOUTUBE: could not find INNERTUBE_API_KEY in live_chat page");
        running_.store(false);
        return;
    }
    if (clientVersion.empty()) clientVersion = "2.20250101.00.00";

    std::string continuation;
    if (!ExtractInitialContinuation(chatHtml.body, continuation) || continuation.empty()) {
        Log(L"YOUTUBE: could not find initial continuation token");
        running_.store(false);
        return;
    }

    Log(L"YOUTUBE: got apiKey/clientVersion/continuation; entering poll loop.");

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
            L"Referer: https://www.youtube.com/\r\n"
            L"X-Youtube-Client-Name: 1\r\n"
            L"X-Youtube-Client-Version: " + ToW(clientVersion) + L"\r\n"
            L"Cookie: SOCS=CAI; CONSENT=YES+1\r\n";

        if (!visitorData.empty()) {
            headers += L"X-Goog-Visitor-Id: " + ToW(visitorData) + L"\r\n";
        }

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
            Log(L"YOUTUBE: poll failed status=" + std::to_wstring(r.status) + L" winerr=" + std::to_wstring(r.winerr));
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            continue;
        }

        json j;
        try { j = json::parse(r.body); }
        catch (...) {
            Log(L"YOUTUBE: poll returned non-JSON; backing off");
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            continue;
        }

        std::vector<ChatMessage> msgs;
        ExtractChatMessages(j, msgs);
        for (auto& m : msgs) {
            if (chat) chat->Add(std::move(m));
        }

        // Extract YouTube paid/membership events into AppState (separate from chat)
        if (state) {
            std::vector<EventItem> evs;
            ExtractYouTubeEvents(j, evs);

            for (const auto& e : evs) {
                // Build a stable-ish key for this YouTube event
                std::string key =
                    e.type + "|" + e.user + "|" + std::to_string(e.ts_ms) + "|" + e.message;

                {
                    std::lock_guard<std::mutex> lk(yt_seen_mu);
                    if (!yt_seen.insert(key).second) {
                        continue; // already seen this event
                    }
                }

                state->push_youtube_event(e);

                if (chat) {
                    ChatMessage m;
                    m.platform = "youtube";
                    m.user = e.user;
                    m.message = "[" + e.type + "] " + e.message;
                    m.ts_ms = e.ts_ms;

                    chat->Add(std::move(m));
                }
            }

            int timeoutMs = sleepMs;
            std::string nextCont;
            if (ExtractContinuationAndTimeout(j, nextCont, timeoutMs) && !nextCont.empty()) {
                continuation = std::move(nextCont);
            }

            if (timeoutMs < 250) timeoutMs = 250;
            if (timeoutMs > 10000) timeoutMs = 10000;

            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        }

        running_.store(false);
        Log(L"YOUTUBE: stopped");
    }
}