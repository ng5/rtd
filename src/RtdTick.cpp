#pragma once
#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <atlctl.h>
#include <atlwin.h>
#include <atlsafe.h>
#include <atomic>
#include <map>
#include <memory>
#include <vector>
#include "resource.h"
#include "RtdTickLib_i.h"
#include "IDataSource.h"
#include "WebSocketDataSource.h"
#include "LegacyRandomDataSource.h"
#include "Logger.h"

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
    m_callback = cb;

    GetLogger().LogServerStart();

    // Register available data sources
    RegisterDataSources();

    *result = 1;
    return S_OK;
  }

  STDMETHOD(ConnectData)(long topicId, SAFEARRAY **strings,
                         VARIANT_BOOL *getNewValues, VARIANT *value) override {
    if (!strings || !getNewValues || !value) return E_POINTER;

    // Parse parameters from Excel
    TopicParams params = ParseTopicParams(*strings);

    // Find appropriate data source
    IDataSource* source = FindDataSource(params);
    if (!source) {
      return E_INVALIDARG;
    }

    // Subscribe via the data source
    double initialValue = 0.0;
    if (!source->Subscribe(topicId, params, initialValue)) {
      return E_FAIL;
    }

    // Track which source handles this topic
    m_topicSources[topicId] = source;

    // Return initial value
    VariantInit(value);
    if (initialValue != 0.0) {
      // Legacy mode - return immediate value
      *getNewValues = VARIANT_FALSE;
      value->vt = VT_R8;
      value->dblVal = initialValue;
    } else {
      // WebSocket mode - wait for data
      *getNewValues = VARIANT_TRUE;
      value->vt = VT_EMPTY;
    }

    return S_OK;
  }

  STDMETHOD(RefreshData)(long *topicCount, SAFEARRAY **data) override {
    if (!topicCount || !data) return E_POINTER;

    // Collect updates from all data sources
    std::vector<TopicUpdate> allUpdates;
    for (auto& source : m_dataSources) {
      std::vector<TopicUpdate> updates = source->GetNewData();
      allUpdates.insert(allUpdates.end(), updates.begin(), updates.end());
    }

    if (allUpdates.empty()) {
      *topicCount = 0;
      *data = nullptr;
      return S_OK;
    }

    // Build 2D SAFEARRAY for Excel
    CComSafeArrayBound bounds[2];
    bounds[0].SetCount(2);  // rows (topic ID, value)
    bounds[1].SetCount((ULONG)allUpdates.size());  // columns
    CComSafeArray<VARIANT> sa;
    if (FAILED(sa.Create(bounds, 2))) return E_FAIL;

    LONG col = 0;
    for (const auto& update : allUpdates) {
      // Row 0: Topic ID
      VARIANT vTopic;
      VariantInit(&vTopic);
      vTopic.vt = VT_I4;
      vTopic.lVal = update.topicId;

      LONG idx[2];
      idx[0] = 0;
      idx[1] = col;
      sa.MultiDimSetAt(idx, vTopic);

      // Row 1: Value
      VARIANT vValue;
      VariantInit(&vValue);
      vValue.vt = VT_R8;
      vValue.dblVal = update.value;

      idx[0] = 1;
      sa.MultiDimSetAt(idx, vValue);

      col++;
    }

    *topicCount = (long)allUpdates.size();
    *data = sa.Detach();

    return S_OK;
  }

  STDMETHOD(DisconnectData)(long topicId) override {
    auto it = m_topicSources.find(topicId);
    if (it != m_topicSources.end()) {
      it->second->Unsubscribe(topicId);
      m_topicSources.erase(it);
    }
    return S_OK;
  }

  STDMETHOD(Heartbeat)(long *r) override {
    if (!r) return E_POINTER;
    *r = 1;
    return S_OK;
  }

  STDMETHOD(ServerTerminate)() override {
    GetLogger().LogServerTerminate();

    m_stopping = true;

    // Shutdown all data sources
    for (auto& source : m_dataSources) {
      source->Shutdown();
    }

    m_dataSources.clear();
    m_topicSources.clear();
    m_callback.Release();

    return S_OK;
  }

  void FinalRelease() {
    m_stopping = true;
    for (auto& source : m_dataSources) {
      source->Shutdown();
    }
    m_dataSources.clear();
    m_topicSources.clear();
    m_callback.Release();
  }

private:
  CComPtr<IRTDUpdateEvent> m_callback;
  std::atomic<bool> m_stopping{false};

  // Registered data sources
  std::vector<std::unique_ptr<IDataSource>> m_dataSources;

  // Map from topicId to the data source handling it
  std::map<long, IDataSource*> m_topicSources;

  void RegisterDataSources() {
    // Create callback that notifies Excel when data is available
    auto notifyCallback = [this]() {
      if (!m_stopping && m_callback) {
        m_callback->UpdateNotify();
      }
    };

    // Register WebSocket data source
    auto wsSource = std::make_unique<WebSocketDataSource>();
    wsSource->Initialize(notifyCallback);
    m_dataSources.push_back(std::move(wsSource));

    // Register Legacy random data source
    auto legacySource = std::make_unique<LegacyRandomDataSource>();
    legacySource->Initialize(notifyCallback);
    m_dataSources.push_back(std::move(legacySource));
  }

  TopicParams ParseTopicParams(SAFEARRAY* sa) {
    TopicParams params;

    LONG lBound = 0, uBound = 0;
    SafeArrayGetLBound(sa, 1, &lBound);
    SafeArrayGetUBound(sa, 1, &uBound);

    // First parameter
    if (lBound <= uBound) {
      VARIANT v;
      VariantInit(&v);
      LONG idx = lBound;
      SafeArrayGetElement(sa, &idx, &v);
      if (v.vt == VT_BSTR) {
        params.param1 = v.bstrVal;
      }
      VariantClear(&v);
    }

    // Second parameter
    if (lBound + 1 <= uBound) {
      VARIANT v;
      VariantInit(&v);
      LONG idx = lBound + 1;
      SafeArrayGetElement(sa, &idx, &v);
      if (v.vt == VT_BSTR) {
        params.param2 = v.bstrVal;
      }
      VariantClear(&v);
    }

    return params;
  }

  IDataSource* FindDataSource(const TopicParams& params) {
    for (auto& source : m_dataSources) {
      if (source->CanHandle(params)) {
        return source.get();
      }
    }
    return nullptr;
  }
};

OBJECT_ENTRY_AUTO(__uuidof(RtdTick), RtdTick)
