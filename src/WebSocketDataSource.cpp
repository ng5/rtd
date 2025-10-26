#include "WebSocketDataSource.h"
#include <atlbase.h>
#include <atlwin.h>
#include <map>

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

struct WebSocketDataSource::Impl {
    WebSocketManager wsManager;
    WebSocketNotifyWindow notifyWindow;
    DataAvailableCallback callback;

    Impl() {}
    ~Impl() {}
};

WebSocketDataSource::WebSocketDataSource() : pImpl(std::make_unique<Impl>()) {}
WebSocketDataSource::~WebSocketDataSource() {
    try {
        pImpl->wsManager.Shutdown();
        pImpl->wsManager.SetNotifyWindow(nullptr);
        if (pImpl->notifyWindow.m_hWnd)
            pImpl->notifyWindow.DestroyWindow();
    } catch (const std::exception &e) {
        GetLogger().LogError(e.what());
    }
}

void WebSocketDataSource::Initialize(DataAvailableCallback callback) {
    pImpl->callback = callback;
    if (pImpl->notifyWindow.CreateNow()) {
        pImpl->notifyWindow.SetCallback(callback);
        pImpl->wsManager.SetNotifyWindow(pImpl->notifyWindow.m_hWnd);
    }
}

bool WebSocketDataSource::Subscribe(long topicId, const TopicParams &params, double &initialValue) {
    GetLogger().LogSubscription(topicId, params.param1, params.param2);
    if (!pImpl->wsManager.Subscribe(topicId, params.param1, params.param2)) {
        std::string msg = "WEBSOCKET SUBSCRIBE FAILED: URL='" + params.param1 + "', Topic='" + params.param2 + "'";
        GetLogger().LogError(msg);
        return false;
    }
    initialValue = 0.0;
    return true;
}

void WebSocketDataSource::Unsubscribe(long topicId) { pImpl->wsManager.Unsubscribe(topicId); }

std::vector<TopicUpdate> WebSocketDataSource::GetNewData() {
    std::vector<TopicUpdate> updates;
    std::map<long, VARIANT> wsUpdates;
    pImpl->wsManager.GetAllNewData(wsUpdates);
    for (auto &[fst, snd] : wsUpdates) {
        auto &v = snd;
        if (v.vt == VT_R8) {
            updates.emplace_back(TopicUpdate{.topicId = fst, .value = v.dblVal});
            GetLogger().LogDataReceived(fst, v.dblVal, "WebSocket");
        }
        VariantClear(&v);
    }
    return updates;
}

bool WebSocketDataSource::CanHandle(const TopicParams &params) const {
    return params.param1.rfind("ws://", 0) == 0 || params.param1.rfind("wss://", 0) == 0;
}

void WebSocketDataSource::Shutdown() { pImpl->wsManager.Shutdown(); }

std::string WebSocketDataSource::GetSourceName() const { return "WebSocket"; }
