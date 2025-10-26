#include "ScalarSource.h"
#include <IDataSource.h>
#include <Logger.h>
#include <Windows.h>
#include <atlbase.h>
#include <atlwin.h>
#include <exception>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <sysinfoapi.h>
#include <vector>

class ScalarTimerWindow : public CWindowImpl<ScalarTimerWindow, CWindow, CWinTraits<>> {
    DataAvailableCallback m_callback{};

  public:
    BEGIN_MSG_MAP(ScalarTimerWindow)
    MESSAGE_HANDLER(WM_TIMER, OnTimer)
    END_MSG_MAP()

    void SetCallback(const DataAvailableCallback &callback) { m_callback = callback; }

    BOOL CreateNow() { return Create(nullptr) != nullptr; }

    void StartTimer(UINT ms) {
        if (m_hWnd)
            SetTimer(1, ms);
    }

    void StopTimer() {
        if (m_hWnd)
            KillTimer(1);
    }

    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL &) const {
        if (m_callback) {
            m_callback();
        }
        return 0;
    }
};

struct ScalarSource::Impl {
    ScalarTimerWindow timerWindow;
    DataAvailableCallback callback;
    std::set<long> topics;
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> dist;

    Impl() : rng(static_cast<unsigned int>(GetTickCount64())), dist(0.0, 1.0) {}

    double NextRand() { return dist(rng) * 100.0; }
};

ScalarSource::ScalarSource() : pImpl(std::make_unique<Impl>()) {}
ScalarSource::~ScalarSource() {
    try {
        pImpl->timerWindow.StopTimer();
        if (pImpl->timerWindow.m_hWnd)
            pImpl->timerWindow.DestroyWindow();
    } catch (const std::exception &e) {
        GetLogger().LogError(e.what());
    }
}

void ScalarSource::Initialize(DataAvailableCallback callback) {
    pImpl->callback = callback;
    if (pImpl->timerWindow.CreateNow()) {
        pImpl->timerWindow.SetCallback(callback);
    }
}

bool ScalarSource::Subscribe(long topicId, const TopicParams &params, double &initialValue) {
    GetLogger().LogSubscription(topicId, params.param1, "");
    pImpl->topics.insert(topicId);
    if (pImpl->topics.size() == 1)
        pImpl->timerWindow.StartTimer(1000);
    initialValue = pImpl->NextRand();
    return true;
}

void ScalarSource::Unsubscribe(long topicId) {
    GetLogger().LogUnsubscribe(topicId);
    pImpl->topics.erase(topicId);
    if (pImpl->topics.empty())
        pImpl->timerWindow.StopTimer();
}

std::vector<TopicUpdate> ScalarSource::GetNewData() {
    std::vector<TopicUpdate> updates;
    for (auto topicId : pImpl->topics) {
        auto u = TopicUpdate{.topicId = topicId, .value = pImpl->NextRand()};
        updates.push_back(u);
    }
    return updates;
}

bool ScalarSource::CanHandle(const TopicParams &params) const {
    return !(params.param1.starts_with("ws://") || params.param1.starts_with("wss://"));
}

void ScalarSource::Shutdown() {
    pImpl->timerWindow.StopTimer();
    pImpl->topics.clear();
}

std::string ScalarSource::GetSourceName() const { return "ScalarRandom"; }
