#pragma once
#include "IDataSource.h"
#include "Logger.h"
#include "WebSocketManager.h"
#include <memory>

// WebSocket data source implementation
class WebSocketDataSource : public IDataSource {
  public:
    WebSocketDataSource();
    ~WebSocketDataSource() override;

    void Initialize(DataAvailableCallback callback) override;
    bool Subscribe(long topicId, const TopicParams &params, double &initialValue) override;
    void Unsubscribe(long topicId) override;
    std::vector<TopicUpdate> GetNewData() override;
    [[nodiscard]] bool CanHandle(const TopicParams &params) const override;
    void Shutdown() override;
    [[nodiscard]] std::string GetSourceName() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
