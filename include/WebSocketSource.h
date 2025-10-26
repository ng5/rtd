#pragma once
#include "IDataSource.h"
#include "Logger.h"
#include <memory>

class WebSocketSource : public IDataSource {
  public:
    WebSocketSource();
    ~WebSocketSource() override;

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
