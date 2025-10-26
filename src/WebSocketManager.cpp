#include "WebSocketManager.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <libwebsockets.h>
#include <sstream>
#include <thread>
#include <unordered_map>

struct ConnectionData {
    std::string url;
    std::atomic<bool> connected{false};
    std::atomic<bool> shouldStop{false};
    std::map<long, std::shared_ptr<TopicSubscription>> topics{};
    std::mutex topicsMutex;

    // lws objects
    struct lws_context *context{nullptr};
    struct lws *wsi{nullptr};
    std::thread workerThread;
};

// global mapping from wsi to ConnectionData*
static std::mutex g_wsi_map_mutex;
static std::unordered_map<struct lws *, ConnectionData *> g_wsi_map;

// Simple JSON helpers (very small, suitable for messages like {"topic":"EURUSD","value":1.0877})
static bool ExtractJsonString(const std::string &s, const std::string &key, std::string &out) {
    auto pos = s.find('"' + key + '"');
    if (pos == std::string::npos)
        return false;
    auto colon = s.find(':', pos);
    if (colon == std::string::npos)
        return false;
    auto p = colon + 1;
    while (p < s.size() && isspace(static_cast<unsigned char>(s[p])))
        ++p;
    if (p >= s.size() || s[p] != '"')
        return false;
    ++p;
    auto start = p;
    while (p < s.size() && s[p] != '"')
        ++p;
    if (p >= s.size())
        return false;
    out.assign(s.data() + start, p - start);
    return true;
}

static bool ExtractJsonNumber(const std::string &s, const std::string &key, double &out) {
    auto pos = s.find('"' + key + '"');
    if (pos == std::string::npos)
        return false;
    auto colon = s.find(':', pos);
    if (colon == std::string::npos)
        return false;
    auto p = colon + 1;
    while (p < s.size() && isspace(static_cast<unsigned char>(s[p])))
        ++p;
    if (p >= s.size())
        return false;
    auto start = p;
    while (p < s.size() && (isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.' || s[p] == '-' || s[p] == '+'))
        ++p;
    if (start == p)
        return false;
    try {
        size_t idx = 0;
        out = std::stod(std::string(s.data() + start, p - start), &idx);
        return idx > 0;
    } catch (...) {
        return false;
    }
}

// Protocol callback used per connection. We store the ConnectionData pointer in the user.
static int rtd_lws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    ConnectionData *conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_wsi_map_mutex);
        auto it = g_wsi_map.find(wsi);
        if (it != g_wsi_map.end())
            conn = it->second;
    }
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        GetLogger().LogInfo("LWS: client established");
        if (conn)
            conn->connected = true;
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (conn && in && len > 0) {
            std::string msg(reinterpret_cast<char *>(in), len);
            GetLogger().LogWebSocketMessage(conn->url, msg);
            // debug: log parsed topic/value
            std::string dbgTopic;
            double dbgVal = 0.0;
            bool dbgHaveTopic = ExtractJsonString(msg, "topic", dbgTopic);
            bool dbgHaveValue = ExtractJsonNumber(msg, "value", dbgVal);
            std::ostringstream dbgss;
            dbgss << "LWS_RECEIVE: url='" << conn->url << "' haveTopic=" << dbgHaveTopic << " topic='" << dbgTopic
                  << "' haveValue=" << dbgHaveValue << " val=" << dbgVal;
            GetLogger().LogInfo(dbgss.str());

            std::string parsedTopic;
            double parsedValue = 0.0;
            bool haveTopic = ExtractJsonString(msg, "topic", parsedTopic);
            bool haveValue = ExtractJsonNumber(msg, "value", parsedValue);

            std::lock_guard lock(conn->topicsMutex);
            bool anyMatched = false;
            for (auto &kv : conn->topics) {
                auto &sub = kv.second;
                bool match = false;
                if (sub->topicFilter.empty()) {
                    match = true;
                } else if (haveTopic) {
                    match = (sub->topicFilter == parsedTopic);
                } else {
                    match = (msg.find(sub->topicFilter) != std::string::npos);
                }
                // log the comparison
                std::ostringstream mss;
                mss << "MatchCheck: topicId=" << kv.first << " filter='" << sub->topicFilter << "' -> "
                    << (match ? "MATCH" : "NO_MATCH");
                GetLogger().LogInfo(mss.str());
                if (!match)
                    continue;

                if (haveValue) {
                    VariantClear(&sub->cachedValue);
                    sub->cachedValue.vt = VT_R8;
                    sub->cachedValue.dblVal = parsedValue;
                    sub->hasNewData = true;
                    // log matched numeric update
                    GetLogger().LogDataReceived(kv.first, parsedValue, "WebSocket");
                    anyMatched = true;
                } else {
                    VariantClear(&sub->cachedValue);
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
                    BSTR b = SysAllocStringLen(nullptr, static_cast<UINT>(wlen > 0 ? wlen - 1 : 0));
                    if (b) {
                        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, b, wlen);
                        sub->cachedValue.vt = VT_BSTR;
                        sub->cachedValue.bstrVal = b;
                        sub->hasNewData = true;
                    }
                }
            }

            // Post notify
            // Find HWND and IRTD callback from owner manager? We cannot access here. Instead, schedule notify via
            // lws_cancel_service via context pointer - use a simplistic approach: call UpdateNotify if available via
            // global lookup is complex; instead rely on the manager after service loop to post.
        }
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        GetLogger().LogError("LWS: connection error");
        if (conn)
            conn->connected = false;
        // remove mapping
        {
            std::lock_guard<std::mutex> lock(g_wsi_map_mutex);
            g_wsi_map.erase(wsi);
        }
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        GetLogger().LogInfo("LWS: client closed");
        if (conn)
            conn->connected = false;
        {
            std::lock_guard<std::mutex> lock(g_wsi_map_mutex);
            g_wsi_map.erase(wsi);
        }
        break;
    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {{"rtd-protocol", rtd_lws_callback, 0, 65536}, {nullptr, nullptr, 0, 0}};

WebSocketManager::WebSocketManager() {}
WebSocketManager::~WebSocketManager() { Shutdown(); }

void WebSocketManager::SetNotifyWindow(HWND hwnd) { m_notifyWindow.store(hwnd, std::memory_order_relaxed); }
void WebSocketManager::SetCallback(IRTDUpdateEvent *callback) { m_callback = callback; }
void WebSocketManager::SetConnectTimeoutSeconds(int s) { m_connectTimeoutSeconds = (s > 0) ? s : 2; }

bool WebSocketManager::Subscribe(long topicId, const std::string &url, const std::string &topicFilter) {
    try {
        std::lock_guard lock(m_mutex);
        auto subscription = std::make_shared<TopicSubscription>();
        subscription->topicFilter = topicFilter;
        m_topicToUrl[topicId] = url;
        auto it = m_connections.find(url);
        if (it == m_connections.end()) {
            auto conn = std::make_shared<ConnectionData>();
            conn->url = url;
            conn->topics[topicId] = subscription;
            m_connections[url] = conn;
            conn->workerThread = std::thread([this, conn]() { ConnectionWorker(conn); });
        } else {
            std::lock_guard tlock(it->second->topicsMutex);
            it->second->topics[topicId] = subscription;
        }
        return true;
    } catch (const std::exception &e) {
        GetLogger().LogError(e.what());
        return false;
    }
}

void WebSocketManager::Unsubscribe(long topicId) {
    try {
        std::lock_guard lock(m_mutex);
        auto urlIt = m_topicToUrl.find(topicId);
        if (urlIt == m_topicToUrl.end())
            return;
        auto url = urlIt->second;
        m_topicToUrl.erase(urlIt);
        auto connIt = m_connections.find(url);
        if (connIt == m_connections.end())
            return;
        {
            std::lock_guard tlock(connIt->second->topicsMutex);
            connIt->second->topics.erase(topicId);
            if (connIt->second->topics.empty()) {
                connIt->second->shouldStop = true;
                if (connIt->second->workerThread.joinable())
                    connIt->second->workerThread.join();
                if (connIt->second->context)
                    lws_context_destroy(connIt->second->context);
                m_connections.erase(connIt);
            }
        }
    } catch (const std::exception &e) {
        GetLogger().LogError(e.what());
    }
}

void WebSocketManager::GetAllNewData(std::map<long, VARIANT> &updates) {
    std::lock_guard lock(m_mutex);
    for (auto &val : m_connections) {
        auto &conn = val.second;
        std::lock_guard tlock(conn->topicsMutex);
        for (auto &kv : conn->topics) {
            if (kv.second->hasNewData) {
                VARIANT v;
                VariantInit(&v);
                VariantCopy(&v, &kv.second->cachedValue);
                updates[kv.first] = v;
                kv.second->hasNewData = false;
            }
        }
    }
}

void WebSocketManager::Shutdown() {
    GetLogger().LogInfo("WebSocketManager: Shutdown requested");
    m_shuttingDown.store(true, std::memory_order_release);
    std::lock_guard lock(m_mutex);
    for (auto &kv : m_connections) {
        kv.second->shouldStop = true;
    }
    for (auto &kv : m_connections) {
        if (kv.second->workerThread.joinable())
            kv.second->workerThread.join();
        if (kv.second->context)
            lws_context_destroy(kv.second->context);
    }
    m_connections.clear();
    GetLogger().LogInfo("WebSocketManager: Shutdown complete");
}

void WebSocketManager::SetAllTopicsToString(ConnectionData *conn, const std::string &msg) {
    if (!conn)
        return;
    std::lock_guard tlock(conn->topicsMutex);
    for (auto &kv : conn->topics) {
        auto &sub = kv.second;
        VariantClear(&sub->cachedValue);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
        BSTR b = SysAllocStringLen(nullptr, static_cast<UINT>(wlen > 0 ? wlen - 1 : 0));
        if (b) {
            MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, b, wlen);
            sub->cachedValue.vt = VT_BSTR;
            sub->cachedValue.bstrVal = b;
            sub->hasNewData = true;
        }
    }

    // notify: only use window post; do not call UpdateNotify() from worker thread to avoid COM crashes
    if (m_shuttingDown.load(std::memory_order_acquire)) {
        GetLogger().LogInfo("WebSocketManager: suppressing notify during shutdown");
        return;
    }
    if (HWND hwnd = m_notifyWindow.load(std::memory_order_relaxed); hwnd && IsWindow(hwnd)) {
        PostMessage(hwnd, WM_WEBSOCKET_DATA, 0, 0);
    } else {
        // no notify window available; do nothing. Excel will poll or external code should set callback via
        // SetNotifyWindow.
    }
}

void WebSocketManager::ConnectionWorker(std::shared_ptr<ConnectionData> conn) {
    // parse URL
    bool secure = false;
    std::wstring hostw;
    std::wstring pathw;
    unsigned short port = 0;
    // simple parse: ws://host:port/path or ws://host/path
    std::string url = conn->url;
    if (url.rfind("wss://", 0) == 0) {
        secure = true;
        url = url.substr(6);
    } else if (url.rfind("ws://", 0) == 0) {
        secure = false;
        url = url.substr(5);
    }

    auto slash = url.find('/');
    std::string hostport = (slash == std::string::npos) ? url : url.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);

    auto colon = hostport.find(':');
    std::string host = (colon == std::string::npos) ? hostport : hostport.substr(0, colon);
    std::string portStr = (colon == std::string::npos) ? std::string() : hostport.substr(colon + 1);
    if (!portStr.empty()) {
        try {
            port = static_cast<unsigned short>(std::stoi(portStr));
        } catch (...) {
            port = secure ? 443 : 80;
        }
    } else {
        port = secure ? 443 : 80;
    }

    hostw = std::wstring(host.begin(), host.end());
    pathw = std::wstring(path.begin(), path.end());

    // create context
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = 0;

    conn->context = lws_create_context(&info);
    if (!conn->context) {
        GetLogger().LogError("Failed to create lws context");
        SetAllTopicsToString(conn.get(), "server down");
        return;
    }

    // connect
    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = conn->context;
    ccinfo.address = host.c_str();
    ccinfo.port = port;
    ccinfo.path = path.c_str();
    ccinfo.host = host.c_str();
    ccinfo.origin = host.c_str();
    ccinfo.protocol = protocols[0].name;
    ccinfo.ssl_connection = secure ? LCCSCF_USE_SSL : 0;
    ccinfo.ietf_version_or_minus_one = -1;
    conn->wsi = lws_client_connect_via_info(&ccinfo);
    if (!conn->wsi) {
        GetLogger().LogError("lws_client_connect_via_info failed");
        SetAllTopicsToString(conn.get(), "server down");
        lws_context_destroy(conn->context);
        conn->context = nullptr;
        return;
    }
    // insert mapping for callbacks
    {
        std::lock_guard<std::mutex> lock(g_wsi_map_mutex);
        g_wsi_map[conn->wsi] = conn.get();
    }

    // Run event loop
    auto start = std::chrono::steady_clock::now();
    while (!conn->shouldStop && !m_shuttingDown.load(std::memory_order_acquire)) {
        lws_service(conn->context, 100);
        // notify RTD if any data present
        bool any = false;
        {
            std::lock_guard tlock(conn->topicsMutex);
            for (auto &kv : conn->topics) {
                if (kv.second->hasNewData) {
                    any = true;
                    break;
                }
            }
        }
        if (any) {
            if (m_shuttingDown.load(std::memory_order_acquire)) {
                GetLogger().LogInfo("WebSocketManager: suppressing post because shutting down");
            } else if (HWND hwnd = m_notifyWindow.load(std::memory_order_relaxed); hwnd && IsWindow(hwnd)) {
                std::ostringstream ss;
                ss << "Posting WM_WEBSOCKET_DATA to hwnd=" << hwnd << " (url='" << conn->url << "')";
                GetLogger().LogInfo(ss.str());
                PostMessage(hwnd, WM_WEBSOCKET_DATA, 0, 0);
            } else {
                // no notify window: cannot safely call UpdateNotify from worker thread
                GetLogger().LogInfo("WebSocketManager: data available but no notify window set");
            }
        }
    }

    if (m_shuttingDown.load(std::memory_order_acquire)) {
        GetLogger().LogInfo(std::string("ConnectionWorker exiting due to manager shutdown for URL='") + conn->url +
                            "'");
    } else if (conn->shouldStop) {
        GetLogger().LogInfo(std::string("ConnectionWorker exiting due to conn->shouldStop for URL='") + conn->url +
                            "'");
    }

    lws_context_destroy(conn->context);
    conn->context = nullptr;
}
