#include "weather_service.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "miniz.h"
#include <cmath>
#include <string>
#include <vector>

static const char* TAG = "HeFengWeather";

WeatherService* WeatherService::GetInstance() {
    static WeatherService instance;
    return &instance;
}

void WeatherService::Init(const char* api_token, double lat, double lon) {
    api_token_ = api_token;
    // 使用固定的漯河位置
    lat_ = 33.569578;
    lon_ = 114.075365;
    ESP_LOGI(TAG, "Init HeFeng Weather Service: %s, lat: %.6f, lon: %.6f", api_token, lat_, lon_);
}

void WeatherService::FetchWeather() {
    char url[256];
    // 使用和风天气API（自定义网关）
    snprintf(url, sizeof(url), "https://m65khx44db.re.qweatherapi.com/v7/weather/now?location=101181501&key=%s",
             api_token_.c_str());

    ESP_LOGI(TAG, "Fetching: %s", url);

    std::vector<uint8_t> raw_response;
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 10000;
    config.event_handler = [](esp_http_client_event_t* evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
            auto* buf = static_cast<std::vector<uint8_t>*>(evt->user_data);
            buf->insert(buf->end(), (uint8_t*)evt->data, (uint8_t*)evt->data + evt->data_len);
        }
        return ESP_OK;
    };
    config.user_data = &raw_response;
    config.skip_cert_common_name_check = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || raw_response.empty()) {
        ESP_LOGE(TAG, "Failed: err=%d, status=%d, response empty=%s", err, status,
                 raw_response.empty() ? "true" : "false");
        if (!raw_response.empty() && raw_response.size() < 256) {
            ESP_LOGE(TAG, "Response: %s", std::string(raw_response.begin(), raw_response.end()).c_str());
        }
        return;
    }

    // 检查是否为gzip压缩数据（前两个字节为0x1f 0x8b）
    std::string json_str;
    if (raw_response.size() >= 2 && raw_response[0] == 0x1f && raw_response[1] == 0x8b) {
        ESP_LOGI(TAG, "Response is gzip compressed, decompressing...");

        // 正确解析gzip头（可能有扩展字段），找到raw deflate数据的起始位置
        const uint8_t* data = raw_response.data();
        size_t len = raw_response.size();
        if (len < 18 || data[2] != 0x08) {  // 至少10字节头+8字节尾, CM=8(deflate)
            ESP_LOGE(TAG, "bad gzip header");
            return;
        }

        size_t hdr = 10;  // 基础头长度
        uint8_t flg = data[3];
        if (flg & 0x04) {  // FEXTRA
            if (hdr + 2 > len) { ESP_LOGE(TAG, "gzip hdr err"); return; }
            size_t xlen = data[hdr] | ((size_t)data[hdr+1] << 8);
            hdr += 2 + xlen;
        }
        if (flg & 0x08) {  // FNAME
            while (hdr < len && data[hdr] != 0) hdr++;
            if (hdr < len) hdr++;
        }
        if (flg & 0x10) {  // FCOMMENT
            while (hdr < len && data[hdr] != 0) hdr++;
            if (hdr < len) hdr++;
        }
        if (flg & 0x02) {  // FHCRC
            hdr += 2;
        }
        if (hdr + 8 > len) {
            ESP_LOGE(TAG, "gzip no deflate data");
            return;
        }

        // 流式解压，用tinfl（结构>10KB，必须堆分配）
        tinfl_decompressor* decomp = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
        if (!decomp) { ESP_LOGE(TAG, "OOM decomp"); return; }
        tinfl_init(decomp);

        const size_t OUT_SZ = 4096;
        uint8_t* outbuf = (uint8_t*)malloc(OUT_SZ);
        if (!outbuf) { free(decomp); ESP_LOGE(TAG, "OOM buf"); return; }

        const uint8_t* in_ptr = data + hdr;
        size_t in_left = len - hdr - 8;  // 排除CRC32+ISIZE尾部
        size_t out_pos = 0;

        tinfl_status status;
        do {
            size_t in_chunk = in_left;
            size_t out_free = OUT_SZ - out_pos;
            status = tinfl_decompress(decomp, in_ptr, &in_chunk,
                                       outbuf, outbuf + out_pos, &out_free,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            if (status < TINFL_STATUS_DONE && status != TINFL_STATUS_HAS_MORE_OUTPUT) {
                ESP_LOGE(TAG, "tinfl fail: %d", status);
                free(outbuf); free(decomp);
                return;
            }
            in_ptr += in_chunk;
            in_left -= in_chunk;
            out_pos += out_free;
        } while (status != TINFL_STATUS_DONE);

        json_str.assign((char*)outbuf, out_pos);
        free(outbuf);
        free(decomp);
        ESP_LOGI(TAG, "Decompressed %zu bytes -> %zu bytes", raw_response.size(), json_str.size());
    } else {
        // 未压缩，直接使用
        json_str.assign(raw_response.begin(), raw_response.end());
    }

    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return;
    }

    // 检查API状态
    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") != 0) {
        cJSON* msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "API Error: %s (code: %s)", msg ? msg->valuestring : "Unknown", code->valuestring);
        cJSON_Delete(root);
        return;
    }

    // 解析实况天气数据 - 根据实际API返回，字段名为"now"而非"fxNow"
    cJSON* nowData = cJSON_GetObjectItem(root, "now");
    if (nowData) {
        // 温度
        cJSON* temp = cJSON_GetObjectItem(nowData, "temp");
        if (temp && cJSON_IsString(temp)) {
            weather_.temperature = atoi(temp->valuestring);
        }

        // 天气描述
        cJSON* text = cJSON_GetObjectItem(nowData, "text");
        if (text && cJSON_IsString(text)) {
            weather_.description = text->valuestring;
        }

        // 湿度
        cJSON* humidity = cJSON_GetObjectItem(nowData, "humidity");
        if (humidity && cJSON_IsString(humidity)) {
            weather_.humidity = atoi(humidity->valuestring);
        }

        // 风向
        cJSON* windDir = cJSON_GetObjectItem(nowData, "windDir");
        if (windDir && cJSON_IsString(windDir)) {
            weather_.wind_direction = windDir->valuestring;
        }

        // 风力等级
        cJSON* windScale = cJSON_GetObjectItem(nowData, "windScale");
        if (windScale && cJSON_IsString(windScale)) {
            weather_.wind_level = atoi(windScale->valuestring);
        }

        weather_.valid = true;
        ESP_LOGI(TAG, "Updated: %s %d°C %s %d级 %d%%",
                weather_.description.c_str(),
                weather_.temperature,
                (weather_.wind_direction + " " + std::to_string(weather_.wind_level) + "级").c_str(),
                weather_.wind_level,
                weather_.humidity);
    } else {
        ESP_LOGE(TAG, "Could not find 'now' field in response");
    }

    cJSON_Delete(root);
}

WeatherService::WeatherInfo WeatherService::GetWeather() {
    std::lock_guard<std::mutex> lock(mutex_);
    return weather_;
}

void WeatherService::StartAutoUpdate(uint32_t interval_ms) {
    if (enabled_) return;
    
    interval_ms_ = interval_ms;
    enabled_ = true;
    
    // 立即获取一次天气数据
    FetchWeather();

    // 创建自动更新任务
    TaskHandle_t task_handle = nullptr;
    xTaskCreatePinnedToCore([](void* param) {
        auto* svc = static_cast<WeatherService*>(param);
        while (svc->enabled_) {
            vTaskDelay(pdMS_TO_TICKS(svc->interval_ms_));
            if (svc->enabled_) {
                svc->FetchWeather();
            }
        }
        vTaskDelete(nullptr);
    }, "Weather", 4096, this, 2, &task_handle, tskNO_AFFINITY);
}

bool WeatherService::IsTokenValid() const {
    return !api_token_.empty();
}

bool WeatherService::IsWeatherValid() const {
    return weather_.valid;
}