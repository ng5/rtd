#pragma once
#include "IDataSource.h"
#include "WebSocketManager.h"
#include "Logger.h"
#include <atlbase.h>
#include <atlwin.h>

// Window for receiving WebSocket notifications
class WebSocketNotifyWindow : public CWindowImpl<WebSocketNotifyWindow, CWindow, CWinTraits<>> {
    DataAvailableCallback m_callback;

public:
    BEGIN_MSG_MAP(WebSocketNotifyWindow)
        MESSAGE_HANDLER(WM_WEBSOCKET_DATA, OnWebSocketData)
    END_MSG_MAP()

    void SetCallback(DataAvailableCallback callback) {
        m_callback = callback;
    }

    BOOL CreateNow() {
        return Create(nullptr) != nullptr;
    }

    LRESULT OnWebSocketData(UINT, WPARAM, LPARAM, BOOL&) {
        if (m_callback) {
            m_callback();
        }
        return 0;
    }
};

// WebSocket data source implementation
class WebSocketDataSource : public IDataSource {
private:
    WebSocketManager m_wsManager;
    WebSocketNotifyWindow m_notifyWindow;
    DataAvailableCallback m_callback;

public:
    WebSocketDataSource() = default;
    ~WebSocketDataSource() override = default;

    void Initialize(DataAvailableCallback callback) override {
        m_callback = callback;

        if (m_notifyWindow.CreateNow()) {
            m_notifyWindow.SetCallback(callback);
            m_wsManager.SetNotifyWindow(m_notifyWindow.m_hWnd);
        }
    }

    bool Subscribe(long topicId, const TopicParams& params, double& initialValue) override {
        GetLogger().LogSubscription(topicId, params.param1, params.param2);

        m_wsManager.Subscribe(topicId, params.param1, params.param2);

        // WebSocket doesn't have immediate data
        initialValue = 0.0;
        return true;
    }

    void Unsubscribe(long topicId) override {
        GetLogger().LogUnsubscribe(topicId);
        m_wsManager.Unsubscribe(topicId);
    }

    std::vector<TopicUpdate> GetNewData() override {
        std::vector<TopicUpdate> updates;

        // Get data from WebSocket manager
        std::map<long, VARIANT> wsUpdates;
        m_wsManager.GetAllNewData(wsUpdates);

        for (auto& pair : wsUpdates) {
            if (pair.second.vt == VT_R8) {
                TopicUpdate update;
                update.topicId = pair.first;
                update.value = pair.second.dblVal;
                updates.push_back(update);

                GetLogger().LogDataReceived(pair.first, pair.second.dblVal, L"WebSocket");
            }
            VariantClear(&pair.second);
        }

        return updates;
    }

    bool CanHandle(const TopicParams& params) const override {
        // Check if param1 starts with ws:// or wss://
        return params.param1.find(L"ws://") == 0 || params.param1.find(L"wss://") == 0;
    }

    void Shutdown() override {
        m_wsManager.Shutdown();
        if (m_notifyWindow.m_hWnd) {
            m_notifyWindow.DestroyWindow();
        }
    }

    std::wstring GetSourceName() const override {
        return L"WebSocket";
    }
};
