#pragma once

// Prevent windows.h from pulling winsock.h which conflicts with winsock2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>

#include "Logger.h"
#include "RtdTickLib_i.h"

#include <atlbase.h>
#include <atlcom.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <oaidl.h>

// Message posted to the notify window when websocket data arrives
#define WM_WEBSOCKET_DATA (WM_USER +100)

struct TopicSubscription {
 std::string topicFilter;
 VARIANT cachedValue{};
 bool hasNewData;

 TopicSubscription() : hasNewData(false) { VariantInit(&cachedValue); }
 ~TopicSubscription() { VariantClear(&cachedValue); }
};

struct ConnectionData;

class WebSocketManager {
 public:
 WebSocketManager();
 ~WebSocketManager();

 void SetNotifyWindow(HWND hwnd);
 void SetCallback(IRTDUpdateEvent *callback);
 void SetConnectTimeoutSeconds(int s);

 bool Subscribe(long topicId, const std::string &url, const std::string &topicFilter);
 void Unsubscribe(long topicId);
 void GetAllNewData(std::map<long, VARIANT> &updates);
 void Shutdown();

 private:
 std::map<std::string, std::shared_ptr<ConnectionData>> m_connections;
 std::map<long, std::string> m_topicToUrl;
 std::mutex m_mutex;
 std::atomic<HWND> m_notifyWindow{nullptr};
 CComPtr<IRTDUpdateEvent> m_callback{};

 int m_connectTimeoutSeconds{2};
 std::atomic<bool> m_shuttingDown{false};

 // internal helpers
 void ConnectionWorker(std::shared_ptr<ConnectionData> conn);
 void SetAllTopicsToString(ConnectionData *conn, const std::string &msg);
};
