#pragma once
#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <atlctl.h>
#include <atlwin.h>
#include <atlsafe.h>
#include <random>
#include <atomic>
#include <stdint.h>
#include "resource.h"
#include "RtdTickLib_i.h"

class TimerWindow : public CWindowImpl<TimerWindow, CWindow, CWinTraits<>>
{
    CComPtr<IRTDUpdateEvent> m_cb;
    std::atomic<bool>* m_stoppingFlag = nullptr;
public:
    BEGIN_MSG_MAP(TimerWindow)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
    END_MSG_MAP()

    void SetCallback(IRTDUpdateEvent* cb, std::atomic<bool>* stoppingFlag = nullptr)
    { m_cb = cb; m_stoppingFlag = stoppingFlag; }

    BOOL CreateNow() { return Create(0) != NULL; }
    void Start(UINT ms) { if (m_hWnd) SetTimer(1, ms); }
    void Stop()  { if (m_hWnd) KillTimer(1); }

    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&)
    {
        Stop(); // one-shot; re-arm from RefreshData
        if (m_stoppingFlag && *m_stoppingFlag) return 0;
        if (m_cb) (void)m_cb->UpdateNotify();
        return 0;
    }
};

// {C5D2C3F2-FA6B-4B3A-9B6E-7B8E07C54111}
class DECLSPEC_UUID("C5D2C3F2-FA6B-4B3A-9B6E-7B8E07C54111")
RtdTick :
  public CComObjectRootEx<CComSingleThreadModel>,   // STA
  public CComCoClass<RtdTick, &__uuidof(RtdTick)>,
  public IDispatchImpl<IRtdServer, &__uuidof(IRtdServer), &LIBID_RtdTickLib, 1, 0>
{
public:
  DECLARE_REGISTRY_RESOURCEID(IDR_RtdTick)

  BEGIN_COM_MAP(RtdTick)
    COM_INTERFACE_ENTRY(IRtdServer)
    COM_INTERFACE_ENTRY(IDispatch)
  END_COM_MAP()

  // ---- IRtdServer ----
  STDMETHOD(ServerStart)(IRTDUpdateEvent* cb, long* result) override
  {
    if (!result) return E_POINTER;
    m_stopping = false;
    m_cb = cb;
    if (m_timer.CreateNow()) {
        m_timer.SetCallback(cb, &m_stopping);
        m_timer.Start(1000);
    }
    if (m_cb) m_cb->UpdateNotify(); // prompt first RefreshData
    *result = 1;
    return S_OK;
  }

  STDMETHOD(ConnectData)(long topicId, SAFEARRAY** /*strings*/,
                         VARIANT_BOOL* getNewValues, VARIANT* value) override
  {
    m_topicId = topicId;
    if (getNewValues) *getNewValues = VARIANT_FALSE; // allow immediate use of this value
    VariantInit(value);
    value->vt = VT_R8;
    value->dblVal = NextRand();
    return S_OK;
  }

  STDMETHOD(RefreshData)(long* topicCount, SAFEARRAY** data) override
  {
    if (!topicCount || !data) return E_POINTER;

    CComSafeArrayBound b[2]; b[0].SetCount(2); b[1].SetCount(1); // 2x1
    CComSafeArray<VARIANT> sa; if (FAILED(sa.Create(b, 2))) return E_FAIL;

    VARIANT vTopic; VariantInit(&vTopic); vTopic.vt = VT_I4; vTopic.lVal = m_topicId;
    VARIANT vVal;   VariantInit(&vVal);   vVal.vt   = VT_R8; vVal.dblVal = NextRand();

    LONG idx[2];
    idx[0]=0; idx[1]=0; sa.MultiDimSetAt(idx, vTopic);
    idx[0]=1; idx[1]=0; sa.MultiDimSetAt(idx, vVal);

    *topicCount = 1;
    *data = sa.Detach();

    // Re-arm after Excel has pulled this batch
    m_timer.Start(1000);
    return S_OK;
  }

  STDMETHOD(DisconnectData)(long) override
  {
    m_timer.Stop();
    if (m_timer.m_hWnd) m_timer.DestroyWindow();
    return S_OK;
  }

  STDMETHOD(Heartbeat)(long* r) override
  {
    if (!r) return E_POINTER;
    *r = 1;
    return S_OK;
  }

  STDMETHOD(ServerTerminate)() override
  {
    m_stopping = true;
    m_timer.Stop();
    if (m_timer.m_hWnd) m_timer.DestroyWindow();
    m_cb.Release();
    return S_OK;
  }

  // Called when the COM object is finally released
  void FinalRelease()
  {
    m_stopping = true;
    m_timer.Stop();
    if (m_timer.m_hWnd) m_timer.DestroyWindow();
    m_cb.Release();
  }

private:
  TimerWindow m_timer;
  CComPtr<IRTDUpdateEvent> m_cb;
  long m_topicId = 0;
  std::atomic<bool> m_stopping{false};

  static double NextRand()
  {
    static std::mt19937_64 rng{ static_cast<uint64_t>(::GetTickCount64()) };
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng);
  }
};

OBJECT_ENTRY_AUTO(__uuidof(RtdTick), RtdTick)
