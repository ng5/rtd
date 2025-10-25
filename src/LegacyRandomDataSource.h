#pragma once
#include "IDataSource.h"
#include "Logger.h"
#include <atlbase.h>
#include <atlwin.h>
#include <random>
#include <set>

// Timer window for legacy random data
class LegacyTimerWindow : public CWindowImpl<LegacyTimerWindow, CWindow, CWinTraits<>> {
    DataAvailableCallback m_callback;

public:
    BEGIN_MSG_MAP(LegacyTimerWindow)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
    END_MSG_MAP()

    void SetCallback(DataAvailableCallback callback) {
        m_callback = callback;
    }

    BOOL CreateNow() {
        return Create(nullptr) != nullptr;
    }

    void StartTimer(UINT ms) {
        if (m_hWnd) SetTimer(1, ms);
    }

    void StopTimer() {
        if (m_hWnd) KillTimer(1);
    }

    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&) {
        if (m_callback) {
            m_callback();
        }
        return 0;
    }
};

// Legacy random number generator data source
class LegacyRandomDataSource : public IDataSource {
private:
    LegacyTimerWindow m_timerWindow;
    DataAvailableCallback m_callback;
    std::set<long> m_topics;
    std::mt19937_64 m_rng;
    std::uniform_real_distribution<double> m_dist;

    double NextRand() {
        return m_dist(m_rng) * 100.0;  // 0-100 range
    }

public:
    LegacyRandomDataSource()
        : m_rng(static_cast<unsigned int>(GetTickCount64())), m_dist(0.0, 1.0) {
    }

    ~LegacyRandomDataSource() override = default;

    void Initialize(DataAvailableCallback callback) override {
        m_callback = callback;

        if (m_timerWindow.CreateNow()) {
            m_timerWindow.SetCallback(callback);
        }
    }

    bool Subscribe(long topicId, const TopicParams& params, double& initialValue) override {
        GetLogger().LogSubscription(topicId, params.param1, L"");

        m_topics.insert(topicId);

        // Start timer if this is the first topic
        if (m_topics.size() == 1) {
            m_timerWindow.StartTimer(1000);  // 1 second interval
        }

        // Return immediate random value
        initialValue = NextRand();
        return true;
    }

    void Unsubscribe(long topicId) override {
        GetLogger().LogUnsubscribe(topicId);

        m_topics.erase(topicId);

        // Stop timer if no more topics
        if (m_topics.empty()) {
            m_timerWindow.StopTimer();
        }
    }

    std::vector<TopicUpdate> GetNewData() override {
        std::vector<TopicUpdate> updates;

        // Generate random value for each topic
        for (long topicId : m_topics) {
            TopicUpdate update;
            update.topicId = topicId;
            update.value = NextRand();
            updates.push_back(update);

            GetLogger().LogDataReceived(topicId, update.value, L"Legacy");
        }

        return updates;
    }

    bool CanHandle(const TopicParams& params) const override {
        // Legacy handles anything that's NOT a WebSocket URL
        return !(params.param1.find(L"ws://") == 0 || params.param1.find(L"wss://") == 0);
    }

    void Shutdown() override {
        m_timerWindow.StopTimer();
        if (m_timerWindow.m_hWnd) {
            m_timerWindow.DestroyWindow();
        }
        m_topics.clear();
    }

    std::wstring GetSourceName() const override {
        return L"Legacy";
    }
};
