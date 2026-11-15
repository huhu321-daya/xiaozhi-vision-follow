#include "news_service.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "NewsService";

// 今日头条热搜API
static const char* TOUTIAO_API_URL = "https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc";

// 轮播间隔 15秒，更新间隔 30分钟 = 120个tick
static constexpr int kRotateIntervalMs = 15000;
static constexpr int kFetchTicks = (30 * 60 * 1000) / kRotateIntervalMs;  // 120

const std::vector<std::string> NewsService::kDefaultHeadlines = {
    "小智AI — 你的智能语音助手",
    "生活小贴士：每天喝8杯水，健康又美丽",
    "知识百科：太阳系最大的行星是木星",
    "励志金句：千里之行，始于足下",
    "健康提醒：久坐1小时后记得站起来活动",
    "趣味冷知识：蜂蜜是唯一不会变质的食物",
    "天气预报：点击查看今日天气详情",
    "科技前沿：AI技术正在改变我们的生活方式",
    "环保小贴士：随手关灯，节约每一度电",
    "读书推荐：每天阅读30分钟，遇见更好的自己",
};

NewsService* NewsService::GetInstance() {
    static NewsService instance;
    return &instance;
}

void NewsService::Init() {
    ESP_LOGI(TAG, "News service initialized");
    headlines_ = kDefaultHeadlines;
    current_index_ = 0;
}

std::string NewsService::GetHeadline() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (headlines_.empty()) {
        return "小智AI — 你的智能语音助手";
    }
    return headlines_[current_index_];
}

std::string NewsService::GetAllHeadlines() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (headlines_.empty()) {
        return "1：小智AI — 你的智能语音助手";
    }
    std::string result;
    result.reserve(headlines_.size() * 60);
    for (size_t i = 0; i < headlines_.size(); i++) {
        result += std::to_string(i + 1);
        result += "：";
        result += headlines_[i];
        result += "        ";  // 8空格分隔，起暂停效果
    }
    return result;
}

void NewsService::FetchNews() {
    ESP_LOGI(TAG, "Fetching news from toutiao...");

    esp_http_client_config_t config = {};
    config.url = TOUTIAO_API_URL;
    config.timeout_ms = 10000;
    config.skip_cert_common_name_check = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    std::string response;
    config.event_handler = [](esp_http_client_event_t* evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
            auto* buf = static_cast<std::string*>(evt->user_data);
            buf->append((char*)evt->data, evt->data_len);
        }
        return ESP_OK;
    };
    config.user_data = &response;

    bool fetch_ok = false;
    {
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGW(TAG, "Failed to init HTTP client");
        } else {
            // 头条需要User-Agent，否则可能被拒
            esp_http_client_set_header(client, "User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

            esp_err_t err = esp_http_client_perform(client);
            int status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            if (err != ESP_OK || status != 200 || response.empty()) {
                ESP_LOGW(TAG, "Fetch failed: err=%d status=%d", err, status);
            } else {
                cJSON* root = cJSON_Parse(response.c_str());
                if (root) {
                    cJSON* data = cJSON_GetObjectItem(root, "data");
                    if (data && cJSON_IsArray(data)) {
                        std::vector<std::string> new_headlines;
                        int count = cJSON_GetArraySize(data);
                        for (int i = 0; i < count && i < 30; i++) {
                            cJSON* item = cJSON_GetArrayItem(data, i);
                            if (item) {
                                cJSON* title = cJSON_GetObjectItem(item, "Title");
                                if (title && cJSON_IsString(title) && title->valuestring) {
                                    new_headlines.push_back(title->valuestring);
                                }
                            }
                        }
                        if (!new_headlines.empty()) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            headlines_ = new_headlines;
                            current_index_ = 0;
                            fetch_ok = true;
                            ESP_LOGI(TAG, "Fetched %d headlines from toutiao", (int)new_headlines.size());
                        }
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGW(TAG, "JSON parse failed");
                }
            }
        }
    }
    if (!fetch_ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_index_ = (current_index_ + 1) % headlines_.size();
    }
}

void NewsService::StartAutoUpdate(uint32_t interval_ms) {
    if (enabled_) return;
    enabled_ = true;

    // 立即获取一次
    FetchNews();

    TaskHandle_t task_handle = nullptr;
    xTaskCreatePinnedToCore([](void* param) {
        auto* svc = static_cast<NewsService*>(param);
        int tick = 0;
        while (svc->enabled_) {
            vTaskDelay(pdMS_TO_TICKS(kRotateIntervalMs));
            if (!svc->enabled_) break;
            tick++;
            if (tick % kFetchTicks == 0) {
                svc->FetchNews();
            } else {
                std::lock_guard<std::mutex> lock(svc->mutex_);
                if (!svc->headlines_.empty()) {
                    svc->current_index_ = (svc->current_index_ + 1) % svc->headlines_.size();
                }
            }
        }
        vTaskDelete(nullptr);
    }, "News", 4096, this, 1, &task_handle, tskNO_AFFINITY);
}

bool NewsService::IsValid() const {
    return !headlines_.empty();
}