#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

// Callback for notifying when new data is available
using DataAvailableCallback = std::function<void()>;

// Structure to hold topic subscription parameters
struct TopicParams {
    std::wstring param1;  // e.g., URL for WebSocket, topic name for Legacy
    std::wstring param2;  // e.g., topic filter for WebSocket
};

// Structure to hold updated data
struct TopicUpdate {
    long topicId;
    double value;
};

// Abstract interface for data sources
// Data sources should NOT know about Excel COM interfaces
class IDataSource {
public:
    virtual ~IDataSource() = default;

    // Initialize the data source with a callback for when new data is available
    virtual void Initialize(DataAvailableCallback callback) = 0;

    // Subscribe to a topic
    // Returns initial value (0.0 if no immediate data available)
    // Returns true if subscription succeeded
    virtual bool Subscribe(long topicId, const TopicParams& params, double& initialValue) = 0;

    // Unsubscribe from a topic
    virtual void Unsubscribe(long topicId) = 0;

    // Get all new data since last call
    // Returns list of topic updates
    virtual std::vector<TopicUpdate> GetNewData() = 0;

    // Check if this source can handle the given parameters
    // This allows RtdTick to route subscriptions to the right source
    virtual bool CanHandle(const TopicParams& params) const = 0;

    // Shutdown the data source
    virtual void Shutdown() = 0;

    // Get a descriptive name for logging
    virtual std::wstring GetSourceName() const = 0;
};
