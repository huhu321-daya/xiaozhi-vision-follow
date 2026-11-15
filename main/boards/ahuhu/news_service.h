#ifndef _NEWS_SERVICE_H_
#define _NEWS_SERVICE_H_

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

class NewsService {
public:
    static NewsService* GetInstance();

    void Init();
    void StartAutoUpdate(uint32_t interval_ms = 120000);
    std::string GetHeadline();
    std::string GetAllHeadlines();
    void FetchNews();
    bool IsValid() const;

private:
    NewsService() = default;
    ~NewsService() = default;

    bool enabled_ = false;
    bool update_task_started_ = false;
    int current_index_ = 0;
    std::vector<std::string> headlines_;
    std::mutex mutex_;

    static const std::vector<std::string> kDefaultHeadlines;
};

#endif // _NEWS_SERVICE_H_