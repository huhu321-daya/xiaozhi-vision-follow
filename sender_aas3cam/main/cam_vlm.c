#include "cam_vlm.h"
#include "cam_config.h"
#include "cam_camera.h"
#include "cam_net.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "mbedtls/base64.h"
#include "cJSON.h"

#define TAG "brain"

static volatile int      s_has = -1;
static volatile int      s_zone = 0;
static volatile int      s_dist = -1;
static volatile uint32_t s_last_ms = 0;

static const char *kPrompt =
    "你是小车跟随系统的视觉校准器。看这张照片，判断画面里是否有一个真实的人（忽略雕像、海报、屏幕里的人像）。"
    "只输出一行 JSON，不要任何解释：{\"has\":1,\"zone\":\"center\",\"dist\":2}。"
    "has 取 0（无人）或 1（有人）；zone 取 left/center/right，表示人相对画面左右位置；"
    "dist 取 1（人很远、全身很小）、2（中等距离、半身）、3（很近、占满画面）。只输出 JSON。";

// 从模型松散文本容错提取 has/zone/dist（不依赖严格 JSON）
static bool parse_follow_vlm(const char *text, int *has, int *zone, int *dist)
{
    *has = -1; *zone = 0; *dist = -1;
    const char *p;

    p = strstr(text, "\"has\"");
    if (!p) p = strstr(text, "has");
    if (p) {
        const char *c = strchr(p, ':');
        if (c) {
            c++;
            while (*c && (*c < '0' || *c > '9')) c++;
            if (*c) *has = (*c == '1') ? 1 : (*c == '0' ? 0 : -1);
        }
    }

    if (strstr(text, "left") || strstr(text, "左")) *zone = -1;
    else if (strstr(text, "right") || strstr(text, "右")) *zone = 1;
    else *zone = 0;

    p = strstr(text, "\"dist\"");
    if (!p) p = strstr(text, "dist");
    if (p) {
        const char *c = strchr(p, ':');
        if (c) {
            c++;
            while (*c && (*c < '0' || *c > '9')) c++;
            if (*c && *c >= '1' && *c <= '3') *dist = *c - '0';
        }
    }
    return (*has >= 0);
}

// JPEG -> 硅基流动 Qwen3-VL -> 提取 content 文本（失败返回 false）
static bool ask_vlm(const uint8_t *jpeg, size_t len, char *out, size_t out_cap)
{
    out[0] = '\0';

    // 1) base64
    size_t b64_len = 0, olen = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, jpeg, len);
    char *b64 = heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64) { ESP_LOGE(TAG, "b64 malloc fail"); return false; }
    if (mbedtls_base64_encode((unsigned char *)b64, b64_len, &olen, jpeg, len) != 0) {
        heap_caps_free(b64);
        return false;
    }
    b64[olen] = '\0';

    // 2) 构造请求体
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CAM_VLM_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", 200);
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(msg, "content");

    cJSON *tp = cJSON_CreateObject();
    cJSON_AddStringToObject(tp, "type", "text");
    cJSON_AddStringToObject(tp, "text", kPrompt);
    cJSON_AddItemToArray(content, tp);

    cJSON *ip = cJSON_CreateObject();
    cJSON_AddStringToObject(ip, "type", "image_url");
    cJSON *iu = cJSON_CreateObject();
    char *data_url = heap_caps_malloc(32 + olen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data_url) { cJSON_Delete(root); heap_caps_free(b64); return false; }
    snprintf(data_url, 32 + olen + 1, "data:image/jpeg;base64,%s", b64);
    cJSON_AddStringToObject(iu, "url", data_url);
    heap_caps_free(data_url);
    cJSON_AddItemToObject(ip, "image_url", iu);
    cJSON_AddItemToArray(content, ip);
    cJSON_AddItemToArray(messages, msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    heap_caps_free(b64);
    if (!body) return false;

    // 3) HTTPS POST（open/fetch_headers/read，Content-Length 模式，避免 chunked 超时）
    esp_http_client_config_t cfg = {
        .url = CAM_VLM_URL,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { cJSON_free(body); return false; }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " CAM_VLM_KEY);
    esp_http_client_set_post_field(client, body, strlen(body));

    bool ok = false;
    esp_err_t e = esp_http_client_open(client, strlen(body));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "http open fail: %s", esp_err_to_name(e));
    } else if (esp_http_client_write(client, body, strlen(body)) <= 0) {
        ESP_LOGE(TAG, "http write body fail");
    } else {
        int content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (content_len < 0) {
            ESP_LOGE(TAG, "fetch headers fail: %d", content_len);
        }
        int cap = content_len > 0 ? content_len + 1 : 8192;
        char *resp = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (resp) {
            int total = 0, read = 0;
            while (total < cap - 1 && (read = esp_http_client_read(client, resp + total, cap - total - 1)) > 0) {
                total += read;
            }
            resp[total] = '\0';
            if (status == 200) {
                cJSON *rj = cJSON_Parse(resp);
                if (rj) {
                    cJSON *choices = cJSON_GetObjectItem(rj, "choices");
                    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                        cJSON *first = cJSON_GetArrayItem(choices, 0);
                        cJSON *m = cJSON_GetObjectItem(first, "message");
                        cJSON *c = cJSON_GetObjectItem(m, "content");
                        if (cJSON_IsString(c) && c->valuestring) {
                            strlcpy(out, c->valuestring, out_cap);
                            ok = true;
                        }
                    }
                    cJSON_Delete(rj);
                }
                if (!ok) ESP_LOGW(TAG, "resp parse fail: %.300s", resp);
            } else {
                ESP_LOGW(TAG, "http status=%d: %.200s", status, resp);
            }
            heap_caps_free(resp);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    cJSON_free(body);
    return ok;
}

void cam_vlm_get(int *has, int *zone, int *dist, uint32_t *last_ms)
{
    *has = s_has; *zone = s_zone; *dist = s_dist; *last_ms = s_last_ms;
}

static void vlm_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CAM_VLM_CALIB_MS));
        if (!cam_follow_active) continue;  // 未跟随不校准，省流量

        if (!cam_camera_ready()) continue;
        size_t cap = CAM_FRAME_CAP;
        uint8_t *jpeg = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpeg) continue;
        size_t len = cam_camera_copy(jpeg, cap);
        if (len == 0) { heap_caps_free(jpeg); continue; }

        int64_t t0 = esp_timer_get_time();
        char answer[256];
        bool ok = ask_vlm(jpeg, len, answer, sizeof(answer));
        heap_caps_free(jpeg);
        int64_t cost_ms = (esp_timer_get_time() - t0) / 1000;

        if (!ok) {
            ESP_LOGW(TAG, "request failed (cost=%lldms)", cost_ms);
            continue;
        }

        int has, zone, dist;
        if (!parse_follow_vlm(answer, &has, &zone, &dist)) {
            ESP_LOGW(TAG, "parse fail: %.200s (cost=%lldms)", answer, cost_ms);
            continue;
        }

        s_has = has; s_zone = zone; s_dist = dist;
        s_last_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "calib ok: has=%d zone=%d dist=%d (cost=%lldms)", has, zone, dist, cost_ms);
    }
}

void cam_vlm_start(void)
{
    xTaskCreate(vlm_task, "vlm", 8192, NULL, 3, NULL);
}