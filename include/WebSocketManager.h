#pragma once
#include "Logger.h"
#include "RtdTickLib_i.h"

#include <atlbase.h>
#include <atlcom.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <ranges>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

// Custom window message for websocket updates
#define WM_WEBSOCKET_DATA (WM_USER + 100)

struct TopicSubscription {
    std::string topicFilter;
    VARIANT cachedValue;
    bool hasNewData;

    TopicSubscription() : hasNewData(false) { VariantInit(&cachedValue); }
    ~TopicSubscription() { VariantClear(&cachedValue); }
};

struct ConnectionData {
    std::string url;
    std::atomic<bool> connected;
    std::atomic<bool> shouldStop;
    std::map<long, std::shared_ptr<TopicSubscription>> topics;
    std::mutex topicsMutex;

    ConnectionData() : connected(false), shouldStop(false) {}
};

// Minimal WebSocketManager stub: keeps subscriptions and simulates periodic data updates.
// This adds a background worker that generates synthetic values so the RTD pipeline ticks
// for WebSocket topics while you replace with a full WebSocket implementation later.
class WebSocketManager {
  public:
    WebSocketManager() : m_workerRunning(false), m_rng(static_cast<unsigned int>(GetTickCount())) {}
    ~WebSocketManager() { Shutdown(); }

    void SetNotifyWindow(HWND hwnd) { m_notifyWindow.store(hwnd, std::memory_order_relaxed); }
    void SetCallback(IRTDUpdateEvent *callback) { m_callback = callback; }

    // Return true if connection established (or existing connection ready), false on immediate failure
    bool Subscribe(long topicId, const std::string &url, const std::string &topicFilter) {
        try {
            std::lock_guard lock(m_mutex);

            auto subscription = std::make_shared<TopicSubscription>();
            subscription->topicFilter = topicFilter;

            m_topicToUrl[topicId] = url;

            auto it = m_connections.find(url);
            if (it == m_connections.end()) {
                auto conn = std::make_shared<ConnectionData>();
                conn->url = url;
                conn->connected = true; // simulate immediate connection
                conn->topics[topicId] = subscription;
                m_connections[url] = conn;
            } else {
                std::lock_guard tlock(it->second->topicsMutex);
                it->second->topics[topicId] = subscription;
            }

            // Ensure background worker is running to generate ticks
            StartWorkerIfNeeded();

            // No immediate data for websocket; caller expects initialValue to be0.0
            return true;
        } catch (const std::exception &e) {
            GetLogger().LogError(e.what());
            return false;
        }
    }

    void Unsubscribe(long topicId) {
        try {
            std::lock_guard lock(m_mutex);
            auto urlIt = m_topicToUrl.find(topicId);
            if (urlIt == m_topicToUrl.end())
                return;
            auto &url = urlIt->second;
            m_topicToUrl.erase(urlIt);

            auto connIt = m_connections.find(url);
            if (connIt == m_connections.end())
                return;

            {
                std::lock_guard tlock(connIt->second->topicsMutex);
                connIt->second->topics.erase(topicId);
                if (connIt->second->topics.empty()) {
                    m_connections.erase(connIt);
                }
            }
        } catch (const std::exception &e) {
            GetLogger().LogError(e.what());
        }
    }

    void GetAllNewData(std::map<long, VARIANT> &updates) {
        try {
            std::lock_guard lock(m_mutex);
            for (auto &val : m_connections | std::views::values) {
                const auto &conn = val;
                std::lock_guard tlock(conn->topicsMutex);
                for (auto &[fst, snd] : conn->topics) {
                    if (snd->hasNewData) {
                        VARIANT v;
                        VariantInit(&v);
                        VariantCopy(&v, &snd->cachedValue);
                        updates[fst] = v;
                        snd->hasNewData = false;
                    }
                }
            }
        } catch (const std::exception &e) {
            GetLogger().LogError(e.what());
        }
    }

    void Shutdown() {
        // stop worker first
        StopWorker();

        std::lock_guard lock(m_mutex);
        m_connections.clear();
        m_topicToUrl.clear();
    }

  private:
    std::map<std::string, std::shared_ptr<ConnectionData>> m_connections;
    std::map<long, std::string> m_topicToUrl;
    std::mutex m_mutex;
    std::atomic<HWND> m_notifyWindow{nullptr};
    CComPtr<IRTDUpdateEvent> m_callback;

    // Background worker to generate synthetic ticks
    std::thread m_worker;
    std::atomic<bool> m_workerRunning;
    std::mutex m_workerMutex;
    std::mt19937 m_rng;

    void StartWorkerIfNeeded() {
        bool expected = false;
        if (m_workerRunning.compare_exchange_strong(expected, true)) {
            // start thread
            m_worker = std::thread([this]() { WorkerLoop(); });
        }
    }

    void StopWorker() {
        if (auto expected = true; m_workerRunning.compare_exchange_strong(expected, false)) {
            if (m_worker.joinable()) {
                try {
                    m_worker.join();
                } catch (...) {
                }
            }
        }
    }

    void WorkerLoop() {
        std::uniform_real_distribution<double> dist(0.0, 100.0);
        while (m_workerRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // snapshot connections
            auto snapshot = std::vector<std::pair<std::shared_ptr<ConnectionData>, std::vector<long>>>{};
            {
                std::lock_guard lock(m_mutex);
                for (auto &val : m_connections | std::views::values) {
                    auto &conn = val;
                    auto topicIds = std::vector<long>{};
                    {
                        std::lock_guard tlock(conn->topicsMutex);
                        for (const auto &key : conn->topics | std::views::keys)
                            topicIds.push_back(key);
                    }
                    if (!topicIds.empty())
                        snapshot.emplace_back(conn, std::move(topicIds));
                }
            }

            bool anyUpdate = false;
            for (auto &[fst, snd] : snapshot) {
                auto &conn = fst;
                for (long tid : snd) {
                    double value = dist(m_rng);
                    std::lock_guard tlock(conn->topicsMutex);
                    if (auto it = conn->topics.find(tid); it != conn->topics.end()) {
                        VariantClear(&it->second->cachedValue);
                        it->second->cachedValue.vt = VT_R8;
                        it->second->cachedValue.dblVal = value;
                        it->second->hasNewData = true;
                        anyUpdate = true;
                        GetLogger().LogDataReceived(tid, value, "WebSocket(stub)");
                    }
                }
            }

            if (anyUpdate) {
                if (HWND hwnd = m_notifyWindow.load(std::memory_order_relaxed); hwnd && IsWindow(hwnd)) {
                    PostMessage(hwnd, WM_WEBSOCKET_DATA, 0, 0);
                } else {
                    // fallback: if callback set, call UpdateNotify directly
                    if (auto &cb = m_callback) {
                        try {
                            cb->UpdateNotify();
                        } catch (const std::exception &e) {
                            GetLogger().LogError(e.what());
                        }
                    }
                }
            }
        }
    }
};
