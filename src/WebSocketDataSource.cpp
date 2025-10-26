#include "WebSocketDataSource.h"
#include <IDataSource.h>
#include <Logger.h>
#include <WebSocketManager.h>
#include <Windows.h>
#include <atlbase.h>
#include <atlwin.h>
#include <exception>
#include <map>
#include <memory>
#include <oaidl.h>
#include <oleauto.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

class WebSocketNotifyWindow : public CWindowImpl<WebSocketNotifyWindow, CWindow, CWinTraits<>> {
    DataAvailableCallback m_callback{};
    WebSocketManager *m_manager{nullptr};
    std::atomic<int> m_activeHandlers{0};
    std::atomic<bool> m_shutdownRequested{false};

  public:
    BEGIN_MSG_MAP(WebSocketNotifyWindow)
    MESSAGE_HANDLER(WM_WEBSOCKET_DATA, OnWebSocketData)
    MESSAGE_HANDLER(WM_USER + 101, OnWebSocketShutdown)
    END_MSG_MAP()

    void SetCallback(const DataAvailableCallback &callback) { m_callback = callback; }
    void SetManager(WebSocketManager *m) { m_manager = m; }

    BOOL CreateNow() { return Create(nullptr) != nullptr; }

    void WaitForHandlersToDrain(int timeoutMs = 1000) {
        const int interval = 10;
        int waited = 0;
        while (m_activeHandlers.load(std::memory_order_acquire) > 0 && waited < timeoutMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            waited += interval;
        }
    }

    LRESULT OnWebSocketData(UINT, WPARAM, LPARAM, BOOL &) const {
        // mark active handler
        const_cast<WebSocketNotifyWindow *>(this)->m_activeHandlers.fetch_add(1, std::memory_order_acq_rel);
        try {
            if (m_callback) {
                m_callback();
            }
        } catch (const std::exception &e) {
            GetLogger().LogError(e.what());
        }
        const_cast<WebSocketNotifyWindow *>(this)->m_activeHandlers.fetch_sub(1, std::memory_order_acq_rel);
        return 0;
    }

    LRESULT OnWebSocketShutdown(UINT, WPARAM, LPARAM, BOOL &) {
        // Idempotent: only run shutdown-on-window-thread actions here (destroy window)
        bool already = m_shutdownRequested.exchange(true);
        if (already)
            return 0;
        try {
            // prevent further callbacks
            SetCallback(nullptr);
            // drain any in-flight handlers
            WaitForHandlersToDrain(2000);
            // destroy the window (will return from SendMessage caller)
            if (m_hWnd) {
                DestroyWindow();
            }
        } catch (const std::exception &e) {
            GetLogger().LogError(e.what());
        }
        return 0;
    }
};

struct WebSocketDataSource::Impl {
    WebSocketManager wsManager;
    WebSocketNotifyWindow notifyWindow;
    DataAvailableCallback callback;
    std::atomic<int> subscriptionCount{0};

    Impl() {}
    ~Impl() {}
};

WebSocketDataSource::WebSocketDataSource() : pImpl(std::make_unique<Impl>()) {}
WebSocketDataSource::~WebSocketDataSource() {
    try {
        // Perform strong shutdown: stop workers first, then destroy window on its thread
        // Clear callback to avoid window handler invoking Excel code
        if (pImpl->notifyWindow.m_hWnd) {
            pImpl->notifyWindow.SetCallback(nullptr);
        }
        // Unset notify window so workers stop posting
        pImpl->wsManager.SetNotifyWindow(nullptr);
        // Shutdown manager and join worker threads
        pImpl->wsManager.Shutdown();

        // Remove any pending WM_WEBSOCKET_DATA messages for this window
        HWND hwnd = pImpl->notifyWindow.m_hWnd;
        if (hwnd) {
            MSG msg;
            while (PeekMessage(&msg, hwnd, WM_WEBSOCKET_DATA, WM_WEBSOCKET_DATA, PM_REMOVE)) {
            }
        }

        // Wait for handlers to drain on window
        pImpl->notifyWindow.WaitForHandlersToDrain(2000);

        // Ask window to destroy itself on its own thread (synchronously)
        if (hwnd) {
            SendMessage(hwnd, WM_USER + 101, 0, 0);
        }

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
        pImpl->notifyWindow.SetManager(&pImpl->wsManager);
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
    // increment active subscription count
    pImpl->subscriptionCount.fetch_add(1, std::memory_order_relaxed);
    initialValue = 0.0;
    return true;
}

void WebSocketDataSource::Unsubscribe(long topicId) {
    pImpl->wsManager.Unsubscribe(topicId);
    // decrement subscription count and if zero, perform immediate clean shutdown of notify plumbing
    int remaining = pImpl->subscriptionCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining <= 0) {
        try {
            // capture hwnd
            HWND hwnd = pImpl->notifyWindow.m_hWnd;
            // prevent window callbacks
            if (pImpl->notifyWindow.m_hWnd) {
                pImpl->notifyWindow.SetCallback(nullptr);
            }
            // unset manager notify window so workers stop posting
            pImpl->wsManager.SetNotifyWindow(nullptr);
            // shutdown manager and join worker threads deterministically here
            pImpl->wsManager.Shutdown();

            // remove pending WM_WEBSOCKET_DATA messages
            if (hwnd) {
                MSG msg;
                while (PeekMessage(&msg, hwnd, WM_WEBSOCKET_DATA, WM_WEBSOCKET_DATA, PM_REMOVE)) {
                }
            }

            // wait for any in-flight handlers to finish
            pImpl->notifyWindow.WaitForHandlersToDrain(2000);

            // request window to destroy itself on window thread; SendMessage is synchronous
            if (hwnd) {
                SendMessage(hwnd, WM_USER + 101, 0, 0);
            } else {
                if (pImpl->notifyWindow.m_hWnd)
                    pImpl->notifyWindow.DestroyWindow();
            }
        } catch (const std::exception &e) {
            GetLogger().LogError(e.what());
        }
    }
}

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
