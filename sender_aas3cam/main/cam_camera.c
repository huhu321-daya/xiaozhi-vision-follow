#include "cam_camera.h"
#include "cam_config.h"
#include "cam_detect.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "jpeg_decoder.h"

#define TAG         "cam"
#define FOLLOW_W    (320 / 2)
#define FOLLOW_H    (240 / 2)
#define REINIT_MAX  3

static SemaphoreHandle_t s_mutex;
static uint8_t          *s_slot;
static size_t            s_len;
static uint32_t          s_seq;
static uint32_t          s_frames;
static int64_t           s_last_us;
static volatile bool     s_ready;

static volatile int8_t   s_has;
static volatile int8_t   s_offset;
static volatile int8_t   s_dist;

static bool hw_init(void)
{
    camera_config_t c = {0};
    c.pin_d0        = CAM_PIN_D0;
    c.pin_d1        = CAM_PIN_D1;
    c.pin_d2        = CAM_PIN_D2;
    c.pin_d3        = CAM_PIN_D3;
    c.pin_d4        = CAM_PIN_D4;
    c.pin_d5        = CAM_PIN_D5;
    c.pin_d6        = CAM_PIN_D6;
    c.pin_d7        = CAM_PIN_D7;
    c.pin_xclk      = CAM_PIN_XCLK;
    c.pin_pclk      = CAM_PIN_PCLK;
    c.pin_vsync     = CAM_PIN_VSYNC;
    c.pin_href      = CAM_PIN_HREF;
    c.pin_sccb_sda  = CAM_PIN_SIOD;
    c.pin_sccb_scl  = CAM_PIN_SIOC;
    c.sccb_i2c_port = 0;
    c.pin_pwdn      = CAM_PIN_PWDN;
    c.pin_reset     = CAM_PIN_RESET;
    c.xclk_freq_hz  = CAM_XCLK_HZ;
    c.pixel_format  = PIXFORMAT_JPEG;
    c.frame_size    = FRAMESIZE_VGA;
    c.jpeg_quality  = 12;
    c.jpeg_buffer_size = 64 * 1024;
    c.fb_count      = 3;
    c.grab_mode     = CAMERA_GRAB_LATEST;
    c.fb_location   = CAMERA_FB_IN_PSRAM;

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "OV3660 VGA JPEG ready");
    return true;
}

static void cam_task(void *arg)
{
    (void)arg;
    int fails = 0, reinits = 0;

    for (;;) {
        if (!s_ready) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            if (++fails >= 30) {
                fails = 0;
                if (++reinits > REINIT_MAX) {
                    ESP_LOGE(TAG, "camera dead, restart");
                    esp_restart();
                }
                ESP_LOGW(TAG, "camera lost, re-init #%d", reinits);
                s_ready = false;
                esp_camera_deinit();
                vTaskDelay(pdMS_TO_TICKS(500));
                s_ready = hw_init();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        fails = 0;

        if (fb->format == PIXFORMAT_JPEG && fb->len) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (fb->len <= CAM_FRAME_CAP) {
                memcpy(s_slot, fb->buf, fb->len);
                s_len     = fb->len;
                s_seq++;
                s_frames++;
                s_last_us = esp_timer_get_time();
            }
            xSemaphoreGive(s_mutex);
        }
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(1000 / CAM_CAPTURE_FPS));
    }
}

static void follow_task(void *arg)
{
    (void)arg;
    static int64_t s_last_person_us = 0;  // 最后检测到人的时间戳
    uint8_t *jpg = heap_caps_malloc(CAM_FRAME_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *rgb = heap_caps_malloc(FOLLOW_W * FOLLOW_H * 3, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!jpg || !rgb) {
        ESP_LOGE(TAG, "follow no mem");
        heap_caps_free(jpg);
        heap_caps_free(rgb);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CAM_FOLLOW_MS));
        size_t n = cam_camera_copy(jpg, CAM_FRAME_CAP);
        if (!n) continue;
        esp_jpeg_image_cfg_t cfg = {
            .indata     = jpg,
            .indata_size = (uint32_t)n,
            .outbuf     = rgb,
            .outbuf_size = FOLLOW_W * FOLLOW_H * 3,
            .out_format = JPEG_IMAGE_FORMAT_RGB888,
            .out_scale  = JPEG_IMAGE_SCALE_1_4,
        };
        esp_jpeg_image_output_t out;
        if (esp_jpeg_decode(&cfg, &out) == ESP_OK) {
            int8_t has;
            int8_t offset;
            int8_t dist;
            cam_detect_run(rgb, out.width, out.height, &has, &offset, &dist);
            if (has) {
                s_last_person_us = esp_timer_get_time();
                s_has    = 1;
                s_offset = offset;
                s_dist   = dist;
            } else if (esp_timer_get_time() - s_last_person_us > 2000000) {
                s_has    = 0;
                s_offset = 0;
                s_dist   = 0;
            } else {
                // 目标短暂丢失（静默窗口内）：保持 has=1 但降为停止、偏移清零，
                // 避免继承旧 dist=3 继续后退或旧偏移继续转向
                s_dist   = 2;
                s_offset = 0;
            }
        }
    }
}

bool cam_camera_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_slot  = heap_caps_malloc(CAM_FRAME_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_slot) {
        ESP_LOGE(TAG, "no PSRAM for frame slot");
        return false;
    }
    s_ready = hw_init();
    if (!s_ready) return false;
    if (!cam_detect_init()) {
        ESP_LOGE(TAG, "pedestrian detect init failed");
        return false;
    }
    xTaskCreatePinnedToCore(cam_task, "cam", 4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(follow_task, "detect", 6144, NULL, 5, NULL, 0);
    return true;
}

bool cam_camera_ready(void) { return s_ready; }

size_t cam_camera_copy(uint8_t *dst, size_t cap)
{
    size_t n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_len && s_len <= cap) {
        memcpy(dst, s_slot, s_len);
        n = s_len;
    }
    xSemaphoreGive(s_mutex);
    return n;
}

uint32_t cam_camera_seq(void)
{
    uint32_t v;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    v = s_seq;
    xSemaphoreGive(s_mutex);
    return v;
}

int64_t  cam_camera_last_us(void) { return s_last_us; }
uint32_t cam_camera_frames(void)  { return s_frames; }

void cam_camera_follow(int8_t *has, int8_t *offset, int8_t *dist)
{
    if (has)    *has    = s_has;
    if (offset) *offset = s_offset;
    if (dist)   *dist   = s_dist;
}