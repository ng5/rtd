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
#include <stop_token>
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
    std::jthread workerThread;
    std::mutex topicsMutex;
    std::mutex handlesMutex;                                   // protects hSession/hConnect/hWebSocket
    std::map<long, std::shared_ptr<TopicSubscription>> topics; // topicId -> subscription

    ConnectionData() : connected(false), shouldStop(false), hSession(nullptr), hConnect(nullptr), hWebSocket(nullptr) {}

    ~ConnectionData() {
        try {
            // Signal stop cooperatively
            shouldStop = true;
            try {
                workerThread.request_stop();
            } catch (const std::exception &e) {
                GetLogger().LogError(e.what());
            }

            // Close handles under handlesMutex to avoid races with worker
            {
                std::lock_guard<std::mutex> hlock(handlesMutex);

                // If WebSocket handle exists, attempt graceful close then close handle
                if (hWebSocket) {
                    // Best-effort graceful close; ignore errors
                    WinHttpWebSocketClose(hWebSocket, 1000, nullptr, 0);
                    WinHttpCloseHandle(hWebSocket);
                    hWebSocket = NULL;
                }

                // Close SESSION handle FIRST - this forcefully aborts all pending operations
                if (hSession) {
                    WinHttpCloseHandle(hSession);
                    hSession = NULL;
                }

                if (hConnect) {
                    WinHttpCloseHandle(hConnect);
                    hConnect = NULL;
                }
            }

            // Join worker thread if possible (avoid joining self)
            if (workerThread.joinable()) {
                if (workerThread.get_id() == std::this_thread::get_id()) {
                    // Do not join self; leave the jthread to be destroyed by runtime
                    // Reset the jthread to avoid join in its destructor here
                    try {
                        workerThread = std::jthread();
                    } catch (const std::exception &e) {
                        GetLogger().LogError(e.what());
                    }
                } else {
                    try {
                        workerThread.join();
                    } catch (const std::exception &e) {
                        GetLogger().LogError(e.what());
                    }
                }
            }
        } catch (const std::exception &e) {
            GetLogger().LogError(e.what());
        }
    }
};

class WebSocketManager {
  private:
    std::map<std::wstring, std::shared_ptr<ConnectionData>> m_connections; // url -> connection
    std::map<long, std::wstring> m_topicToUrl;                             // topicId -> url
    std::mutex m_mutex;
    HWND m_notifyWindow;
    CComPtr<IRTDUpdateEvent> m_callback;

    static void WebSocketWorker(std::stop_token stopToken, const std::shared_ptr<ConnectionData> &connData,
                                HWND notifyWindow) {
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
        HINTERNET hSession =
            WinHttpOpen(L"RTD WebSocket Client/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);

        if (!hSession)
            return;

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);

        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            return;
        }

        // Create request for WebSocket
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, nullptr, nullptr,
                                                isSecure ? WINHTTP_FLAG_SECURE : 0);

        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return;
        }

        // Upgrade to WebSocket
        BOOL result = WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

        if (!result) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return;
        }

        // Send request
        result = WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0);

        if (!result) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return;
        }

        // Receive response
        result = WinHttpReceiveResponse(hRequest, nullptr);
        if (!result) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return;
        }

        // Complete upgrade
        HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
        WinHttpCloseHandle(hRequest);

        if (!hWebSocket) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return;
        }

        // Store handles into connection data under lock
        {
            std::lock_guard<std::mutex> hlock(connData->handlesMutex);
            connData->hSession = hSession;
            connData->hConnect = hConnect;
            connData->hWebSocket = hWebSocket;
        }

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

                    // Copy hWebSocket under handle lock to avoid races with shutdown
                    HINTERNET localWebSocket = nullptr;
                    {
                        std::lock_guard<std::mutex> hlock(connData->handlesMutex);
                        localWebSocket = connData->hWebSocket;
                    }
                    if (localWebSocket) {
                        WinHttpWebSocketSend(localWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                             PVOID(subscribeMsg.c_str()), static_cast<DWORD>(subscribeMsg.length()));
                    }

                    sentSubscriptions.insert(val->topicFilter);
                }
            }
        }

        // Receive loop
        BYTE buffer[4096];
        while (!connData->shouldStop && !stopToken.stop_requested()) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;

            HINTERNET localWebSocket = nullptr;
            {
                std::lock_guard<std::mutex> hlock(connData->handlesMutex);
                localWebSocket = connData->hWebSocket;
            }
            if (!localWebSocket)
                break; // no websocket, exit

            DWORD error = WinHttpWebSocketReceive(localWebSocket, buffer, sizeof(buffer), &bytesRead, &bufferType);

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
                    if (anyUpdate && notifyWindow && IsWindow(notifyWindow)) {
                        PostMessage(notifyWindow, WM_WEBSOCKET_DATA, 0, 0);
                    }
                } catch (const std::exception &e) {
                    GetLogger().LogError(e.what());
                }
            } else if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                break;
            }
        }

        connData->connected = false;

        // Clean up handles on exit
        {
            std::lock_guard<std::mutex> hlock(connData->handlesMutex);
            if (connData->hWebSocket) {
                WinHttpWebSocketClose(connData->hWebSocket, 1000, nullptr, 0);
                WinHttpCloseHandle(connData->hWebSocket);
                connData->hWebSocket = nullptr;
            }
            if (connData->hConnect) {
                WinHttpCloseHandle(connData->hConnect);
                connData->hConnect = nullptr;
            }
            if (connData->hSession) {
                WinHttpCloseHandle(connData->hSession);
                connData->hSession = nullptr;
            }
        }

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
                WideCharToMultiByte(CP_UTF8, 0, topicFilter.c_str(), -1, topicUtf8.data(), len, nullptr, nullptr);

                std::string subscribeMsg = R"({"subscribe":")" + topicUtf8 + "\"}";

                // Use a local copy of the websocket handle under lock
                HINTERNET localWebSocket = nullptr;
                {
                    std::lock_guard<std::mutex> hlock(connIt->second->handlesMutex);
                    localWebSocket = connIt->second->hWebSocket;
                }
                if (localWebSocket) {
                    WinHttpWebSocketSend(localWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         PVOID(subscribeMsg.c_str()), static_cast<DWORD>(subscribeMsg.length()));
                }
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

            // Start worker thread (std::jthread will provide a stop_token)
            connData->workerThread = std::jthread(WebSocketWorker, connData, m_notifyWindow);
        }
    }

    void Unsubscribe(long topicId) {
        std::shared_ptr<ConnectionData> connToJoin = nullptr;

        {
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

                    // If no more topics, signal connection to stop and remove from map.
                    if (connIt->second->topics.empty()) {
                        connIt->second->shouldStop = true;
                        // Request cooperative stop
                        try {
                            connIt->second->workerThread.request_stop();
                        } catch (const std::exception &e) {
                            GetLogger().LogError(e.what());
                        }

                        // Close handles to abort any blocking WinHTTP operations
                        {
                            std::lock_guard<std::mutex> hlock(connIt->second->handlesMutex);
                            if (connIt->second->hWebSocket) {
                                WinHttpWebSocketClose(connIt->second->hWebSocket, 1000, nullptr, 0);
                                WinHttpCloseHandle(connIt->second->hWebSocket);
                                connIt->second->hWebSocket = nullptr;
                            }
                            if (connIt->second->hSession) {
                                WinHttpCloseHandle(connIt->second->hSession);
                                connIt->second->hSession = nullptr;
                            }
                            if (connIt->second->hConnect) {
                                WinHttpCloseHandle(connIt->second->hConnect);
                                connIt->second->hConnect = nullptr;
                            }
                        }
                        // Take a copy so we can join the thread outside the mutex
                        connToJoin = connIt->second;
                        m_connections.erase(connIt);
                    }
                }
            }
        }

        // Join the worker thread outside of the mutex to avoid deadlocks.
        if (connToJoin) {
            try {
                if (connToJoin->workerThread.joinable()) {
                    if (connToJoin->workerThread.get_id() == std::this_thread::get_id()) {
                        // Avoid joining self; reset workerThread so destructor won't join here
                        connToJoin->workerThread = std::jthread();
                    } else {
                        connToJoin->workerThread.join();
                    }
                }
            } catch (const std::exception &e) {
                GetLogger().LogError(e.what());
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
        std::vector<std::shared_ptr<ConnectionData>> conns;
        {
            std::lock_guard lock(m_mutex);
            for (auto &val : m_connections | std::views::values) {
                val->shouldStop = true;
                // Request cooperative stop
                try {
                    val->workerThread.request_stop();
                } catch (const std::exception &e) {
                    GetLogger().LogError(e.what());
                }
                // Close handles to abort blocking operations
                {
                    std::lock_guard<std::mutex> hlock(val->handlesMutex);
                    if (val->hWebSocket) {
                        WinHttpWebSocketClose(val->hWebSocket, 1000, nullptr, 0);
                        WinHttpCloseHandle(val->hWebSocket);
                        val->hWebSocket = nullptr;
                    }
                    if (val->hSession) {
                        WinHttpCloseHandle(val->hSession);
                        val->hSession = nullptr;
                    }
                    if (val->hConnect) {
                        WinHttpCloseHandle(val->hConnect);
                        val->hConnect = nullptr;
                    }
                }
                conns.push_back(val);
            }
            m_connections.clear();
            m_topicToUrl.clear();
        }

        // Join worker threads outside the mutex
        for (auto &conn : conns) {
            try {
                if (conn->workerThread.joinable()) {
                    if (conn->workerThread.get_id() == std::this_thread::get_id()) {
                        conn->workerThread = std::jthread();
                    } else {
                        conn->workerThread.join();
                    }
                }
            } catch (const std::exception &e) {
                GetLogger().LogError(e.what());
            }
        }
    }
};
