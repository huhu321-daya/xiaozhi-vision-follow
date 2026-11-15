#include "cam_http.h"
#include "cam_config.h"
#include "cam_camera.h"

#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"

#define TAG             "http"
#define BOUNDARY        "aas3camframe"
#define STREAM_CAP      (CAM_FRAME_CAP + 192)
#define HDR_MAX         160
#define MAX_STREAMS     2
#define NOFRAME_TIMEOUT_US  6000000

typedef struct {
    bool          active;
    volatile bool kill;
    int           fd;
    uint32_t      started_ms;
} stream_slot_t;

static stream_slot_t     s_slots[MAX_STREAMS];
static SemaphoreHandle_t s_slot_mutex;

static stream_slot_t *slot_acquire(int fd)
{
    stream_slot_t *slot = NULL;
    for (int tries = 0; tries < 30 && !slot; tries++) {
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_STREAMS; i++) {
            if (!s_slots[i].active) { slot = &s_slots[i]; break; }
        }
        if (!slot) {
            stream_slot_t *oldest = &s_slots[0];
            for (int i = 1; i < MAX_STREAMS; i++) {
                if (s_slots[i].started_ms < oldest->started_ms) oldest = &s_slots[i];
            }
            oldest->kill = true;
        }
        xSemaphoreGive(s_slot_mutex);
        if (!slot) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (slot) {
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        slot->active     = true;
        slot->kill       = false;
        slot->fd         = fd;
        slot->started_ms = (uint32_t)(esp_timer_get_time() / 1000);
        xSemaphoreGive(s_slot_mutex);
    }
    return slot;
}

static void slot_release(stream_slot_t *slot)
{
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    slot->active = false;
    xSemaphoreGive(s_slot_mutex);
}

static void stream_task(void *arg)
{
    httpd_req_t *req = arg;
    int fd = httpd_req_to_sockfd(req);
    stream_slot_t *slot = slot_acquire(fd);
    uint8_t *buf = slot ? heap_caps_malloc(CAM_FRAME_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;

    if (!slot || !buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            slot ? "no mem" : "too many streams");
        if (slot) slot_release(slot);
        heap_caps_free(buf);
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    struct linger lg = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    struct timeval snd_timeout = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    static const char headers[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send(fd, headers, sizeof(headers) - 1, 0) < 0) {
        heap_caps_free(buf);
        slot_release(slot);
        shutdown(fd, SHUT_RDWR);
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "stream fd=%d start", fd);

    uint32_t seen = 0;
    uint32_t sent = 0;
    int64_t  idle = esp_timer_get_time();

    for (;;) {
        if (slot->kill) break;

        if (cam_camera_seq() == seen) {
            if (esp_timer_get_time() - idle > NOFRAME_TIMEOUT_US) {
                ESP_LOGW(TAG, "stream fd=%d no new frame", fd);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        size_t len = cam_camera_copy(buf, CAM_FRAME_CAP);
        if (!len) {
            if (esp_timer_get_time() - idle > NOFRAME_TIMEOUT_US) break;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        seen = cam_camera_seq();

        char part[96];
        int pl = snprintf(part, sizeof(part),
                          "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)len);
        if (send(fd, part, pl, 0) < 0)             break;
        if (send(fd, (const char *)buf, len, 0) < 0) break;
        if (send(fd, "\r\n", 2, 0) < 0)            break;
        sent++;
        idle = esp_timer_get_time();
    }

    heap_caps_free(buf);
    slot_release(slot);
    shutdown(fd, SHUT_RDWR);
    ESP_LOGI(TAG, "stream fd=%d closed, %u frames", fd, (unsigned)sent);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!cam_camera_ready()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not ready");
    }

    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) return ESP_FAIL;
    if (xTaskCreatePinnedToCore(stream_task, "stream", 8192, copy, 5, NULL, 1) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    uint8_t *buf = heap_caps_malloc(CAM_FRAME_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");

    size_t len = cam_camera_copy(buf, CAM_FRAME_CAP);
    if (!len) {
        heap_caps_free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t res = httpd_resp_send(req, (const char *)buf, len);
    heap_caps_free(buf);
    return res;
}

static esp_err_t healthz_handler(httpd_req_t *req)
{
    int8_t has;
    int8_t offset;
    int8_t dist;
    cam_camera_follow(&has, &offset, &dist);

    int64_t age = cam_camera_last_us() ? (esp_timer_get_time() - cam_camera_last_us()) / 1000 : -1;

    char body[192];
    int n = snprintf(body, sizeof(body),
        "{\"uptime_s\":%" PRIu64 ",\"frame_age_ms\":%" PRId64 ","
        "\"frames\":%" PRIu32 ",\"follow\":[%d,%d,%d],"
        "\"heap\":%" PRIu32 ",\"psram\":%" PRIu32 "}\n",
        (uint64_t)(esp_timer_get_time() / 1000000), age,
        cam_camera_frames(), has, offset, dist,
        (uint32_t)esp_get_free_heap_size(),
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    httpd_resp_set_type(req, "application/json");
    if (n > 0) httpd_resp_send(req, body, n);
    return ESP_OK;
}

bool cam_http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 8192;
    cfg.send_wait_timeout = 10;
    cfg.recv_wait_timeout = 2;
    cfg.lru_purge_enable  = true;
    cfg.max_open_sockets  = 5;

    s_slot_mutex = xSemaphoreCreateMutex();
    if (!s_slot_mutex) {
        ESP_LOGE(TAG, "no mutex");
        return false;
    }

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return false;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler  },
        { .uri = "/camera",  .method = HTTP_GET, .handler = stream_handler  },
        { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler },
        { .uri = "/healthz", .method = HTTP_GET, .handler = healthz_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "ready: /stream /capture /healthz");
    return true;
}