#include "twitch/TwitchEventSubWsClient.h"
#include "AppState.h"

#include <windows.h>
#include <winhttp.h>
#include "json.hpp"

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

TwitchEventSubWsClient::TwitchEventSubWsClient() = default;

TwitchEventSubWsClient::~TwitchEventSubWsClient()
{
    Stop();
}

void TwitchEventSubWsClient::Start(
    const std::string& clientId,
    const std::string& userAccessToken,
    const std::string& broadcasterId,
    ChatCallback onChatEvent)
{
    Stop();

    client_id_ = clientId;
    access_token_ = userAccessToken;
    broadcaster_id_ = broadcasterId;
    on_chat_event_ = std::move(onChatEvent);

    running_ = true;
    worker_ = std::thread(&TwitchEventSubWsClient::Run, this);
}

void TwitchEventSubWsClient::Stop()
{
    running_ = false;

    if (worker_.joinable())
        worker_.join();
}

void TwitchEventSubWsClient::Run()
{
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;
    HINTERNET hWebSocket = nullptr;

    hSession = WinHttpOpen(
        L"ModeS-Twitch-EventSub/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!hSession)
        goto done;

    hConnect = WinHttpConnect(
        hSession,
        L"eventsub.wss.twitch.tv",
        INTERNET_DEFAULT_HTTPS_PORT,
        0);

    if (!hConnect)
        goto done;

    hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        L"/ws",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest)
        goto done;

    if (!WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0))
        goto done;

    if (!WinHttpReceiveResponse(hRequest, nullptr))
        goto done;

    hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    hRequest = nullptr;

    if (!hWebSocket)
        goto done;

    ReceiveLoop(hWebSocket);

done:
    if (hWebSocket)
        WinHttpCloseHandle(hWebSocket);
    if (hRequest)
        WinHttpCloseHandle(hRequest);
    if (hConnect)
        WinHttpCloseHandle(hConnect);
    if (hSession)
        WinHttpCloseHandle(hSession);
}

void TwitchEventSubWsClient::ReceiveLoop(void* ws)
{
    HINTERNET hWebSocket = static_cast<HINTERNET>(ws);
    BYTE buffer[8192];

    while (running_)
    {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;

        if (WinHttpWebSocketReceive(
            hWebSocket,
            buffer,
            sizeof(buffer),
            &bytesRead,
            &bufferType) != NO_ERROR)
        {
            break;
        }

        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            break;

        if (bufferType != static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(0))
            continue;

        std::string payload(reinterpret_cast<char*>(buffer), bytesRead);
        HandleMessage(payload);
    }
}

void TwitchEventSubWsClient::HandleMessage(const std::string& payload)
{
    json j;
    try
    {
        j = json::parse(payload);
    }
    catch (...)
    {
        return;
    }

    if (!j.contains("metadata") || !j.contains("payload"))
        return;

    const std::string type =
        j["metadata"].value("message_type", "");

    if (type == "notification")
    {
        HandleNotification(&j["payload"]);
    }
}

void TwitchEventSubWsClient::HandleNotification(const void* payloadPtr)
{
    const json& payload = *static_cast<const json*>(payloadPtr);

    if (!payload.contains("subscription") || !payload.contains("event"))
        return;

    const std::string subType =
        payload["subscription"].value("type", "");

    const json& ev = payload["event"];

    ChatMessage msg;
    msg.platform = "twitch";

    if (subType == "channel.follow")
    {
        msg.user = ev.value("user_name", "");
        msg.message = "followed";
    }
    else if (subType == "channel.subscribe")
    {
        msg.user = ev.value("user_name", "");
        msg.message = "subscribed";
    }
    else if (subType == "channel.subscription.gift")
    {
        msg.user = ev.value("user_name", "");
        int count = ev.value("total", 1);
        msg.message = "gifted " + std::to_string(count) + " subs";
    }
    else
    {
        return;
    }

    if (on_chat_event_)
        on_chat_event_(msg);
}
