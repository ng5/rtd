#pragma once
#include <windows.h>
#include <winhttp.h>
#include <atlbase.h>
#include <atlcom.h>
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include "../src/third_party/json.hpp"

using json = nlohmann::json;

// Custom window message for websocket updates
#define WM_WEBSOCKET_DATA (WM_USER + 100)

struct TopicSubscription {
    std::wstring topicFilter;
    VARIANT cachedValue;
    bool hasNewData;

    TopicSubscription() : hasNewData(false) {
        VariantInit(&cachedValue);
    }

    ~TopicSubscription() {
        VariantClear(&cachedValue);
    }
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

    ConnectionData() : connected(false), shouldStop(false),
                       hSession(NULL), hConnect(NULL), hWebSocket(NULL) {}

    ~ConnectionData() {
        shouldStop = true;
        if (workerThread.joinable()) {
            workerThread.join();
        }
        if (hWebSocket) WinHttpCloseHandle(hWebSocket);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
    }
};

class WebSocketManager {
private:
    std::map<std::wstring, std::shared_ptr<ConnectionData>> m_connections; // url -> connection
    std::map<long, std::wstring> m_topicToUrl; // topicId -> url
    std::mutex m_mutex;
    HWND m_notifyWindow;
    CComPtr<IRTDUpdateEvent> m_callback;

    static void WebSocketWorker(std::shared_ptr<ConnectionData> connData, HWND notifyWindow) {
        // Parse URL
        std::wstring url = connData->url;
        std::wstring host, path;
        INTERNET_PORT port = 80;
        bool isSecure = false;

        // Simple URL parsing (ws://host:port/path)
        if (url.find(L"wss://") == 0) {
            isSecure = true;
            url = url.substr(6);
            port = 443;
        } else if (url.find(L"ws://") == 0) {
            url = url.substr(5);
        }

        size_t slashPos = url.find(L'/');
        if (slashPos != std::wstring::npos) {
            host = url.substr(0, slashPos);
            path = url.substr(slashPos);
        } else {
            host = url;
            path = L"/";
        }

        // Check for port in host
        size_t colonPos = host.find(L':');
        if (colonPos != std::wstring::npos) {
            port = (INTERNET_PORT)_wtoi(host.substr(colonPos + 1).c_str());
            host = host.substr(0, colonPos);
        }

        // Initialize WinHTTP
        connData->hSession = WinHttpOpen(
            L"RTD WebSocket Client/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);

        if (!connData->hSession) return;

        connData->hConnect = WinHttpConnect(
            connData->hSession,
            host.c_str(),
            port,
            0);

        if (!connData->hConnect) return;

        // Create request for WebSocket
        HINTERNET hRequest = WinHttpOpenRequest(
            connData->hConnect,
            L"GET",
            path.c_str(),
            NULL,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            isSecure ? WINHTTP_FLAG_SECURE : 0);

        if (!hRequest) return;

        // Upgrade to WebSocket
        BOOL result = WinHttpSetOption(
            hRequest,
            WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
            NULL,
            0);

        if (!result) {
            WinHttpCloseHandle(hRequest);
            return;
        }

        // Send request
        result = WinHttpSendRequest(
            hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0);

        if (!result) {
            WinHttpCloseHandle(hRequest);
            return;
        }

        // Receive response
        result = WinHttpReceiveResponse(hRequest, NULL);
        if (!result) {
            WinHttpCloseHandle(hRequest);
            return;
        }

        // Complete upgrade
        connData->hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
        WinHttpCloseHandle(hRequest);

        if (!connData->hWebSocket) return;

        connData->connected = true;

        // Send subscription messages for all topics
        {
            std::lock_guard<std::mutex> lock(connData->topicsMutex);
            std::set<std::wstring> sentSubscriptions;
            for (auto& pair : connData->topics) {
                if (!pair.second->topicFilter.empty() &&
                    sentSubscriptions.find(pair.second->topicFilter) == sentSubscriptions.end()) {
                    // Convert wide string to UTF-8
                    int len = WideCharToMultiByte(CP_UTF8, 0, pair.second->topicFilter.c_str(), -1, NULL, 0, NULL, NULL);
                    std::string topicUtf8(len - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, pair.second->topicFilter.c_str(), -1, &topicUtf8[0], len, NULL, NULL);

                    std::string subscribeMsg = "{\"subscribe\":\"" + topicUtf8 + "\"}";
                    WinHttpWebSocketSend(
                        connData->hWebSocket,
                        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                        (PVOID)subscribeMsg.c_str(),
                        (DWORD)subscribeMsg.length());

                    sentSubscriptions.insert(pair.second->topicFilter);
                }
            }
        }

        // Receive loop
        BYTE buffer[4096];
        while (!connData->shouldStop) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;

            DWORD error = WinHttpWebSocketReceive(
                connData->hWebSocket,
                buffer,
                sizeof(buffer),
                &bytesRead,
                &bufferType);

            if (error != NO_ERROR) break;

            if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                bufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {

                std::string message((char*)buffer, bytesRead);

                try {
                    // Parse JSON
                    json j = json::parse(message);

                    // Extract value
                    double value = 0.0;
                    std::wstring msgTopic;

                    if (j.contains("topic")) {
                        std::string topicStr = j["topic"];
                        msgTopic = std::wstring(topicStr.begin(), topicStr.end());
                    }

                    if (j.contains("value")) {
                        if (j["value"].is_number()) {
                            value = j["value"];
                        } else if (j["value"].is_string()) {
                            value = std::stod(j["value"].get<std::string>());
                        }
                    } else if (j.is_number()) {
                        value = j;
                    }

                    // Route to all matching topics
                    std::lock_guard<std::mutex> lock(connData->topicsMutex);
                    bool anyUpdate = false;
                    for (auto& pair : connData->topics) {
                        auto& subscription = pair.second;

                        // Match if: no topic in message, no filter set, or topic matches filter
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
                } catch (...) {
                    // JSON parse error - ignore
                }
            } else if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                break;
            }
        }

        connData->connected = false;
    }

public:
    WebSocketManager() : m_notifyWindow(NULL) {}

    void SetNotifyWindow(HWND hwnd) {
        m_notifyWindow = hwnd;
    }

    void SetCallback(IRTDUpdateEvent* callback) {
        m_callback = callback;
    }

    void Subscribe(long topicId, const std::wstring& url, const std::wstring& topicFilter) {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Create subscription
        auto subscription = std::make_shared<TopicSubscription>();
        subscription->topicFilter = topicFilter;

        // Track which URL this topic belongs to
        m_topicToUrl[topicId] = url;

        // Check if we already have a connection for this URL
        auto connIt = m_connections.find(url);
        if (connIt != m_connections.end()) {
            // Add topic to existing connection
            std::lock_guard<std::mutex> topicLock(connIt->second->topicsMutex);
            connIt->second->topics[topicId] = subscription;

            // Send subscription message if connected
            if (connIt->second->connected && !topicFilter.empty()) {
                // Convert wide string to UTF-8
                int len = WideCharToMultiByte(CP_UTF8, 0, topicFilter.c_str(), -1, NULL, 0, NULL, NULL);
                std::string topicUtf8(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, topicFilter.c_str(), -1, &topicUtf8[0], len, NULL, NULL);

                std::string subscribeMsg = "{\"subscribe\":\"" + topicUtf8 + "\"}";
                WinHttpWebSocketSend(
                    connIt->second->hWebSocket,
                    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                    (PVOID)subscribeMsg.c_str(),
                    (DWORD)subscribeMsg.length());
            }
        } else {
            // Create new connection
            auto connData = std::make_shared<ConnectionData>();
            connData->url = url;

            {
                std::lock_guard<std::mutex> topicLock(connData->topicsMutex);
                connData->topics[topicId] = subscription;
            }

            m_connections[url] = connData;

            // Start worker thread
            connData->workerThread = std::thread(WebSocketWorker, connData, m_notifyWindow);
        }
    }

    void Unsubscribe(long topicId) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto urlIt = m_topicToUrl.find(topicId);
        if (urlIt == m_topicToUrl.end()) return;

        std::wstring url = urlIt->second;
        m_topicToUrl.erase(urlIt);

        auto connIt = m_connections.find(url);
        if (connIt != m_connections.end()) {
            {
                std::lock_guard<std::mutex> topicLock(connIt->second->topicsMutex);
                connIt->second->topics.erase(topicId);

                // If no more topics, close connection
                if (connIt->second->topics.empty()) {
                    connIt->second->shouldStop = true;
                    m_connections.erase(connIt);
                }
            }
        }
    }

    void GetAllNewData(std::map<long, VARIANT>& updates) {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& connPair : m_connections) {
            std::lock_guard<std::mutex> topicLock(connPair.second->topicsMutex);
            for (auto& topicPair : connPair.second->topics) {
                if (topicPair.second->hasNewData) {
                    VARIANT v;
                    VariantInit(&v);
                    VariantCopy(&v, &topicPair.second->cachedValue);
                    updates[topicPair.first] = v;
                    topicPair.second->hasNewData = false;
                }
            }
        }
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& pair : m_connections) {
            pair.second->shouldStop = true;
        }
        m_connections.clear();
        m_topicToUrl.clear();
    }
};
