#pragma once
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

using DataAvailableCallback = std::function<void()>;

struct TopicParams {
    std::string param1;
    std::string param2;
};

struct TopicUpdate {
    long topicId;
    double value;
};

class IDataSource {
  public:
    virtual ~IDataSource() = default;

    virtual void Initialize(DataAvailableCallback callback) = 0;

    virtual bool Subscribe(long topicId, const TopicParams &params, double &initialValue) = 0;

    virtual void Unsubscribe(long topicId) = 0;

    virtual std::vector<TopicUpdate> GetNewData() = 0;

    [[nodiscard]] virtual bool CanHandle(const TopicParams &params) const = 0;

    virtual void Shutdown() = 0;

    [[nodiscard]] virtual std::string GetSourceName() const = 0;
};
