#pragma once
#include "../src/third_party/simdjson.h"
#include "Logger.h"
#include <atlbase.h>
#include <atlcom.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <windows.h>
#include <winhttp.h>

// Custom window message for websocket updates
#define WM_WEBSOCKET_DATA (WM_USER + 100)

struct TopicSubscription {
    std::wstring topicFilter;
    VARIANT cachedValue;
    bool hasNewData;

    TopicSubscription() : hasNewData(false) { VariantInit(&cachedValue); }

    ~TopicSubscription() { VariantClear(&cachedValue); }
};

struct ConnectionData {
    std::wstring url;
    std::atomic<bool> connected;
    std::atomic<bool> shouldStop;
    HINTERNET hSession;
    HINTERNET hConnect;
    HINTERNET hWebSocket;
    std::thread workerThread;
    std::mutex topicsMutex;
    std::map<long, std::shared_ptr<TopicSubscription>> topics; // topicId -> subscription

    ConnectionData() : connected(false), shouldStop(false), hSession(nullptr), hConnect(nullptr), hWebSocket(nullptr) {}

    ~ConnectionData() {
        shouldStop = true;
        if (workerThread.joinable()) {
            workerThread.join();
        }
        if (hWebSocket)
            WinHttpCloseHandle(hWebSocket);
        if (hConnect)
            WinHttpCloseHandle(hConnect);
        if (hSession)
            WinHttpCloseHandle(hSession);
    }
};

class WebSocketManager {
  private:
    std::map<std::wstring, std::shared_ptr<ConnectionData>> m_connections; // url -> connection
    std::map<long, std::wstring> m_topicToUrl;                             // topicId -> url
    std::mutex m_mutex;
    HWND m_notifyWindow;
    CComPtr<IRTDUpdateEvent> m_callback;

    static void WebSocketWorker(const std::shared_ptr<ConnectionData> &connData, HWND notifyWindow) {
        // Parse URL
        std::wstring url = connData->url;
        std::wstring host, path;
        INTERNET_PORT port = 80;
        bool isSecure = false;

        // Simple URL parsing (ws://host:port/path)
        if (url.starts_with(L"wss://")) {
            isSecure = true;
            url = url.substr(6);
            port = 443;
        } else if (url.starts_with(L"ws://")) {
            url = url.substr(5);
        }

        if (size_t slashPos = url.find(L'/'); slashPos != std::wstring::npos) {
            host = url.substr(0, slashPos);
            path = url.substr(slashPos);
        } else {
            host = url;
            path = L"/";
        }

        // Check for port in host
        if (size_t colonPos = host.find(L':'); colonPos != std::wstring::npos) {
            port = static_cast<INTERNET_PORT>(_wtoi(host.substr(colonPos + 1).c_str()));
            host = host.substr(0, colonPos);
        }

        // Initialize WinHTTP
        connData->hSession =
            WinHttpOpen(L"RTD WebSocket Client/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);

        if (!connData->hSession)
            return;

        connData->hConnect = WinHttpConnect(connData->hSession, host.c_str(), port, 0);

        if (!connData->hConnect)
            return;

        // Create request for WebSocket
        HINTERNET hRequest = WinHttpOpenRequest(connData->hConnect, L"GET", path.c_str(), nullptr, nullptr, nullptr,
                                                isSecure ? WINHTTP_FLAG_SECURE : 0);

        if (!hRequest)
            return;

        // Upgrade to WebSocket
        BOOL result = WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

        if (!result) {
            WinHttpCloseHandle(hRequest);
            return;
        }

        // Send request
        result = WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0);

        if (!result) {
            WinHttpCloseHandle(hRequest);
            return;
        }

        // Receive response
        result = WinHttpReceiveResponse(hRequest, nullptr);
        if (!result) {
            WinHttpCloseHandle(hRequest);
            return;
        }

        // Complete upgrade
        connData->hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
        WinHttpCloseHandle(hRequest);

        if (!connData->hWebSocket)
            return;

        connData->connected = true;

        // Log WebSocket connection
        GetLogger().LogWebSocketConnect(connData->url);

        // Send subscription messages for all topics
        {
            std::lock_guard lock(connData->topicsMutex);
            std::set<std::wstring> sentSubscriptions;
            for (auto &val : connData->topics | std::views::values) {
                if (!val->topicFilter.empty() && !sentSubscriptions.contains(val->topicFilter)) {
                    // Convert wide string to UTF-8
                    int len =
                        WideCharToMultiByte(CP_UTF8, 0, val->topicFilter.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    std::string topicUtf8(len - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, val->topicFilter.c_str(), -1, topicUtf8.data(), len, nullptr,
                                        nullptr);

                    std::string subscribeMsg = R"({"subscribe":")" + topicUtf8 + "\"}";
                    WinHttpWebSocketSend(connData->hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         PVOID(subscribeMsg.c_str()), static_cast<DWORD>(subscribeMsg.length()));

                    sentSubscriptions.insert(val->topicFilter);
                }
            }
        }

        // Receive loop
        BYTE buffer[4096];
        while (!connData->shouldStop) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;

            DWORD error =
                WinHttpWebSocketReceive(connData->hWebSocket, buffer, sizeof(buffer), &bytesRead, &bufferType);

            if (error != NO_ERROR)
                break;

            if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                bufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {

                std::string message(reinterpret_cast<char *>(buffer), bytesRead);

                // Log incoming WebSocket message (optional - can be commented out for
                // production) GetLogger().LogWebSocketMessage(connData->url, message);

                try {
                    // Parse JSON with simdjson
                    simdjson::ondemand::parser parser;
                    simdjson::padded_string json_str(message);
                    simdjson::ondemand::document doc = parser.iterate(json_str);

                    // Extract value
                    double value = 0.0;
                    std::wstring msgTopic;

                    // Try to get topic field
                    if (auto topicResult = doc["topic"].get_string(); !topicResult.error()) {
                        std::string_view topicView;
                        topicView = topicResult.value();
                        msgTopic = std::wstring(topicView.begin(), topicView.end());
                    }

                    // Try to get value field
                    if (auto valueResult = doc["value"].get_double(); !valueResult.error()) {
                        value = valueResult.value();
                    } else {
                        // Try value as string
                        if (auto valueStrResult = doc["value"].get_string(); !valueStrResult.error()) {
                            std::string_view valueStr = valueStrResult.value();
                            value = std::stod(std::string(valueStr));
                        } else {
                            // Try document as direct number
                            if (auto numResult = doc.get_double(); !numResult.error()) {
                                value = numResult.value();
                            }
                        }
                    }

                    // Route to all matching topics
                    std::lock_guard lock(connData->topicsMutex);
                    bool anyUpdate = false;
                    for (auto &val : connData->topics | std::views::values) {
                        auto &subscription = val;

                        // Match if: no topic in message, no filter set, or topic matches
                        // filter
                        if (msgTopic.empty() || subscription->topicFilter.empty() ||
                            msgTopic == subscription->topicFilter) {

                            // Update cached value
                            VariantClear(&subscription->cachedValue);
                            subscription->cachedValue.vt = VT_R8;
                            subscription->cachedValue.dblVal = value;
                            subscription->hasNewData = true;
                            anyUpdate = true;
                        }
                    }

                    // Notify if any topic was updated
                    if (anyUpdate && notifyWindow) {
                        PostMessage(notifyWindow, WM_WEBSOCKET_DATA, 0, 0);
                    }
                } catch (const std::exception &e) {
                    // GetLogger().LogError(std::string{e.what()});
                    // JSON parse error - ignore
                }
            } else if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                break;
            }
        }

        connData->connected = false;

        // Log WebSocket disconnection
        GetLogger().LogWebSocketDisconnect(connData->url);
    }

  public:
    WebSocketManager() : m_notifyWindow(nullptr) {}

    void SetNotifyWindow(HWND hwnd) { m_notifyWindow = hwnd; }

    void SetCallback(IRTDUpdateEvent *callback) { m_callback = callback; }

    void Subscribe(long topicId, const std::wstring &url, const std::wstring &topicFilter) {
        std::lock_guard lock(m_mutex);

        // Create subscription
        auto subscription = std::make_shared<TopicSubscription>();
        subscription->topicFilter = topicFilter;

        // Track which URL this topic belongs to
        m_topicToUrl[topicId] = url;

        // Check if we already have a connection for this URL
        if (auto connIt = m_connections.find(url); connIt != m_connections.end()) {
            // Add topic to existing connection
            std::lock_guard topicLock(connIt->second->topicsMutex);
            connIt->second->topics[topicId] = subscription;

            // Send subscription message if connected
            if (connIt->second->connected && !topicFilter.empty()) {
                // Convert wide string to UTF-8
                int len = WideCharToMultiByte(CP_UTF8, 0, topicFilter.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string topicUtf8(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, topicFilter.c_str(), -1, &topicUtf8[0], len, nullptr, nullptr);

                std::string subscribeMsg = "{\"subscribe\":\"" + topicUtf8 + "\"}";
                WinHttpWebSocketSend(connIt->second->hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     PVOID(subscribeMsg.c_str()), static_cast<DWORD>(subscribeMsg.length()));
            }
        } else {
            // Create new connection
            auto connData = std::make_shared<ConnectionData>();
            connData->url = url;

            {
                std::lock_guard topicLock(connData->topicsMutex);
                connData->topics[topicId] = subscription;
            }

            m_connections[url] = connData;

            // Start worker thread
            connData->workerThread = std::thread(WebSocketWorker, connData, m_notifyWindow);
        }
    }

    void Unsubscribe(long topicId) {
        std::lock_guard lock(m_mutex);

        auto urlIt = m_topicToUrl.find(topicId);
        if (urlIt == m_topicToUrl.end())
            return;

        std::wstring url = urlIt->second;
        m_topicToUrl.erase(urlIt);

        if (auto connIt = m_connections.find(url); connIt != m_connections.end()) {
            {
                std::lock_guard topicLock(connIt->second->topicsMutex);
                connIt->second->topics.erase(topicId);

                // If no more topics, close connection
                if (connIt->second->topics.empty()) {
                    connIt->second->shouldStop = true;
                    m_connections.erase(connIt);
                }
            }
        }
    }

    void GetAllNewData(std::map<long, VARIANT> &updates) {
        std::lock_guard lock(m_mutex);

        for (auto &val : m_connections | std::views::values) {
            std::lock_guard topicLock(val->topicsMutex);
            for (auto &[fst, snd] : val->topics) {
                if (snd->hasNewData) {
                    VARIANT v;
                    VariantInit(&v);
                    VariantCopy(&v, &snd->cachedValue);
                    updates[fst] = v;
                    snd->hasNewData = false;
                }
            }
        }
    }

    void Shutdown() {
        std::lock_guard lock(m_mutex);
        for (auto &val : m_connections | std::views::values) {
            val->shouldStop = true;
        }
        m_connections.clear();
        m_topicToUrl.clear();
    }
};
