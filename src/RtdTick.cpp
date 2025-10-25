#pragma once
#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <atlctl.h>
#include <atlwin.h>
#include <atlsafe.h>
#include <atomic>
#include <map>
#include <set>
#include <random>
#include "resource.h"
#include "RtdTickLib_i.h"
#include "WebSocketManager.h"

class NotifyWindow : public CWindowImpl<NotifyWindow, CWindow, CWinTraits<> > {
  CComPtr<IRTDUpdateEvent> m_cb;
  std::atomic<bool> *m_stoppingFlag = nullptr;

public:
  BEGIN_MSG_MAP(NotifyWindow)
      MESSAGE_HANDLER(WM_WEBSOCKET_DATA, OnWebSocketData)
      MESSAGE_HANDLER(WM_TIMER, OnTimer)
  END_MSG_MAP()

  void SetCallback(IRTDUpdateEvent *cb, std::atomic<bool> *stoppingFlag = nullptr) {
    m_cb = cb;
    m_stoppingFlag = stoppingFlag;
  }

  BOOL CreateNow() { return Create(nullptr) != nullptr; }
  void StartTimer(UINT ms) { if (m_hWnd) SetTimer(1, ms); }
  void StopTimer() { if (m_hWnd) KillTimer(1); }

  LRESULT OnWebSocketData(UINT, WPARAM wParam, LPARAM, BOOL &) {
    if (m_stoppingFlag && *m_stoppingFlag) return 0;
    if (m_cb) (void) m_cb->UpdateNotify();
    return 0;
  }

  LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL &) {
    if (m_stoppingFlag && *m_stoppingFlag) return 0;
    if (m_cb) (void) m_cb->UpdateNotify();
    return 0;
  }
};

// {C5D2C3F2-FA6B-4B3A-9B6E-7B8E07C54111}
class DECLSPEC_UUID("C5D2C3F2-FA6B-4B3A-9B6E-7B8E07C54111")
    RtdTick :
    public CComObjectRootEx<CComSingleThreadModel>, // STA
    public CComCoClass<RtdTick, &__uuidof(RtdTick)>,
    public IDispatchImpl<IRtdServer, &__uuidof(IRtdServer), &LIBID_RtdTickLib, 1, 0> {
public:
  DECLARE_REGISTRY_RESOURCEID(IDR_RtdTick)

  BEGIN_COM_MAP(RtdTick)
    COM_INTERFACE_ENTRY(IRtdServer)
    COM_INTERFACE_ENTRY(IDispatch)
  END_COM_MAP()

  // ---- IRtdServer ----
  STDMETHOD(ServerStart)(IRTDUpdateEvent *cb, long *result) override {
    if (!result) return E_POINTER;
    m_stopping = false;
    m_cb = cb;

    // Create notification window for both websocket and timer callbacks
    if (m_notifyWindow.CreateNow()) {
      m_notifyWindow.SetCallback(cb, &m_stopping);
      m_wsManager.SetNotifyWindow(m_notifyWindow.m_hWnd);
      m_wsManager.SetCallback(cb);
    }

    *result = 1;
    return S_OK;
  }

  STDMETHOD(ConnectData)(long topicId, SAFEARRAY **strings,
                         VARIANT_BOOL *getNewValues, VARIANT *value) override {
    if (!strings || !getNewValues || !value) return E_POINTER;

    // Parse parameters from Excel
    // WebSocket: =RTD("ProgID",, "ws://url", "topic")
    // Legacy: =RTD("ProgID",, "RAND1S")
    SAFEARRAY* sa = *strings;
    std::wstring firstParam, secondParam;
    LONG lBound = 0, uBound = 0;

    // Get bounds
    SafeArrayGetLBound(sa, 1, &lBound);
    SafeArrayGetUBound(sa, 1, &uBound);

    // First parameter
    if (lBound <= uBound) {
      VARIANT v;
      VariantInit(&v);
      LONG idx = lBound;
      SafeArrayGetElement(sa, &idx, &v);
      if (v.vt == VT_BSTR) {
        firstParam = v.bstrVal;
      }
      VariantClear(&v);
    }

    // Second parameter (optional)
    if (lBound + 1 <= uBound) {
      VARIANT v;
      VariantInit(&v);
      LONG idx = lBound + 1;
      SafeArrayGetElement(sa, &idx, &v);
      if (v.vt == VT_BSTR) {
        secondParam = v.bstrVal;
      }
      VariantClear(&v);
    }

    // Check if this is WebSocket mode (URL starts with ws:// or wss://)
    bool isWebSocket = (firstParam.find(L"ws://") == 0 || firstParam.find(L"wss://") == 0);

    if (isWebSocket) {
      // WebSocket mode
      m_topicIds.insert(topicId);
      m_wsManager.Subscribe(topicId, firstParam, secondParam);

      if (getNewValues) *getNewValues = VARIANT_TRUE; // wait for data
      VariantInit(value);
      value->vt = VT_EMPTY;
    } else {
      // Legacy mode - random numbers with timer
      m_legacyTopics.insert(topicId);
      m_topicIds.insert(topicId);

      // Start timer if this is the first legacy topic
      if (m_legacyTopics.size() == 1) {
        m_notifyWindow.StartTimer(1000); // 1 second
      }

      // Return initial random value
      if (getNewValues) *getNewValues = VARIANT_FALSE; // use this value immediately
      VariantInit(value);
      value->vt = VT_R8;
      value->dblVal = NextRand() * 100;
    }

    return S_OK;
  }

  STDMETHOD(RefreshData)(long *topicCount, SAFEARRAY **data) override {
    if (!topicCount || !data) return E_POINTER;

    // Collect updates from both websocket and legacy topics
    std::map<long, VARIANT> updates;

    // Get websocket updates
    m_wsManager.GetAllNewData(updates);

    // Get legacy topic updates (generate random numbers)
    for (long topicId : m_legacyTopics) {
      VARIANT v;
      VariantInit(&v);
      v.vt = VT_R8;
      v.dblVal = NextRand() * 100;
      updates[topicId] = v;
    }

    if (updates.empty()) {
      *topicCount = 0;
      *data = nullptr;
      return S_OK;
    }

    // Build 2D array: rows = 2 (topic ID, value), cols = number of updates
    CComSafeArrayBound bounds[2];
    bounds[0].SetCount(2);  // rows
    bounds[1].SetCount((ULONG)updates.size());  // columns
    CComSafeArray<VARIANT> sa;
    if (FAILED(sa.Create(bounds, 2))) return E_FAIL;

    LONG col = 0;
    for (auto& pair : updates) {
      VARIANT vTopic;
      VariantInit(&vTopic);
      vTopic.vt = VT_I4;
      vTopic.lVal = pair.first;

      LONG idx[2];
      idx[0] = 0;  // row 0 = topic ID
      idx[1] = col;
      sa.MultiDimSetAt(idx, vTopic);

      idx[0] = 1;  // row 1 = value
      sa.MultiDimSetAt(idx, pair.second);

      VariantClear(&pair.second);
      col++;
    }

    *topicCount = (long)updates.size();
    *data = sa.Detach();

    return S_OK;
  }

  STDMETHOD(DisconnectData)(long topicId) override {
    // Remove from WebSocket manager
    m_wsManager.Unsubscribe(topicId);

    // Remove from legacy topics
    m_legacyTopics.erase(topicId);

    // Stop timer if no more legacy topics
    if (m_legacyTopics.empty()) {
      m_notifyWindow.StopTimer();
    }

    m_topicIds.erase(topicId);
    return S_OK;
  }

  STDMETHOD(Heartbeat)(long *r) override {
    if (!r) return E_POINTER;
    *r = 1;
    return S_OK;
  }

  STDMETHOD(ServerTerminate)() override {
    m_stopping = true;
    m_notifyWindow.StopTimer();
    m_wsManager.Shutdown();
    if (m_notifyWindow.m_hWnd) m_notifyWindow.DestroyWindow();
    m_cb.Release();
    return S_OK;
  }

  // Called when the COM object is finally released
  void FinalRelease() {
    m_stopping = true;
    m_notifyWindow.StopTimer();
    m_wsManager.Shutdown();
    if (m_notifyWindow.m_hWnd) m_notifyWindow.DestroyWindow();
    m_cb.Release();
  }

private:
  NotifyWindow m_notifyWindow;
  WebSocketManager m_wsManager;
  CComPtr<IRTDUpdateEvent> m_cb;
  std::set<long> m_topicIds;
  std::set<long> m_legacyTopics;  // Topics using legacy random number mode
  std::atomic<bool> m_stopping{false};

  static double NextRand() {
    static std::mt19937_64 rng{(unsigned int)GetTickCount64()};
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng);
  }
};

OBJECT_ENTRY_AUTO(__uuidof(RtdTick), RtdTick)
