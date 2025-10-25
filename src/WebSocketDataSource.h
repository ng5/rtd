#pragma once
#include "IDataSource.h"
#include "Logger.h"
#include "WebSocketManager.h"
#include <atlbase.h>
#include <atlwin.h>

// Window for receiving WebSocket notifications
class WebSocketNotifyWindow : public CWindowImpl<WebSocketNotifyWindow, CWindow, CWinTraits<>> {
    DataAvailableCallback m_callback;

  public:
    BEGIN_MSG_MAP(WebSocketNotifyWindow)
    MESSAGE_HANDLER(WM_WEBSOCKET_DATA, OnWebSocketData)
    END_MSG_MAP()

    void SetCallback(const DataAvailableCallback &callback) { m_callback = callback; }

    BOOL CreateNow() { return Create(nullptr) != nullptr; }

    LRESULT OnWebSocketData(UINT, WPARAM, LPARAM, BOOL &) const {
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

    ~WebSocketDataSource() override {
        try {
            // Ensure manager is shutdown
            m_wsManager.Shutdown();

            // Destroy the notification window
            if (m_notifyWindow.m_hWnd) {
                m_notifyWindow.DestroyWindow();
            }
        } catch (const std::exception &e) {
            GetLogger().LogError(e.what());
        }
    }

    void Initialize(DataAvailableCallback callback) override {
        m_callback = callback;

        if (m_notifyWindow.CreateNow()) {
            m_notifyWindow.SetCallback(callback);
            m_wsManager.SetNotifyWindow(m_notifyWindow.m_hWnd);
        }
    }

    bool Subscribe(long topicId, const TopicParams &params, double &initialValue) override {
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

        for (auto &[fst, snd] : wsUpdates) {
            if (snd.vt == VT_R8) {
                TopicUpdate update;
                update.topicId = fst;
                update.value = snd.dblVal;
                updates.push_back(update);

                GetLogger().LogDataReceived(fst, snd.dblVal, L"WebSocket");
            }
            VariantClear(&snd);
        }

        return updates;
    }

    bool CanHandle(const TopicParams &params) const override {
        // Check if param1 starts with ws:// or wss://
        return params.param1.find(L"ws://") == 0 || params.param1.find(L"wss://") == 0;
    }

    void Shutdown() override {
        // Just shutdown WebSocket manager (stops threads)
        // DON'T destroy the window - let it be cleaned up by the destructor
        m_wsManager.Shutdown();
    }

    std::wstring GetSourceName() const override { return L"WebSocket"; }
};
