#include "cam_detect.h"
#include "pedestrian_detect.hpp"
#include "esp_log.h"

#define TAG "detect"

static PedestrianDetect *s_detect = nullptr;

bool cam_detect_init(void)
{
    if (s_detect) return true;
    s_detect = new PedestrianDetect(PedestrianDetect::PICO_S8_V1, false);
    if (!s_detect) {
        ESP_LOGE(TAG, "alloc failed");
        return false;
    }
    ESP_LOGI(TAG, "pedestrian model ready");
    return true;
}

void cam_detect_run(const uint8_t *rgb, int w, int h, int8_t *has, int8_t *offset, int8_t *dist)
{
    *has = 0;
    *offset = 0;
    *dist = 0;
    if (!s_detect || !rgb || w <= 0 || h <= 0) return;

    dl::image::img_t img;
    img.data = (void *)rgb;
    img.width = (uint16_t)w;
    img.height = (uint16_t)h;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    auto &results = s_detect->run(img);

    const dl::detect::result_t *best = nullptr;
    for (const auto &r : results) {
        if (r.box.size() < 4) continue;
        // 人体是竖长框，过滤横宽框（整帧误检时宽会超过高）
        float bw = r.box[2] - r.box[0];
        float bh = r.box[3] - r.box[1];
        if (bw <= 0 || bh <= 0 || bw > bh) continue;
        if (!best || r.score > best->score) best = &r;
    }

    // 置信度阈值 + 连续帧确认：过滤低分误检、抑制单帧抖动
    static int    s_confirm = 0;      // 连续有人/无人计数，[-2, 2]
    static int8_t s_last_has = 0;
    static int8_t s_last_offset = 0;
    static int8_t s_last_dist = 0;
    static float  s_cx = -1, s_bw = -1, s_bh = -1;

    bool cur_person = best && best->score >= 0.50f;
    if (cur_person) {
        if (s_confirm < 0) s_confirm = 0;
        if (s_confirm < 2) s_confirm++;
    } else {
        if (s_confirm > 0) s_confirm = 0;
        if (s_confirm > -2) s_confirm--;
    }

    if (s_confirm >= 2) {
        // 确认有人：框中心/宽高 EMA 平滑，计算水平偏差与距离
        float cx = (best->box[0] + best->box[2]) / 2.0f;
        float bw = (float)(best->box[2] - best->box[0]);
        float bh = (float)(best->box[3] - best->box[1]);
        if (s_cx < 0) { s_cx = cx; s_bw = bw; s_bh = bh; }
        else { s_cx = s_cx * 0.5f + cx * 0.5f; s_bw = s_bw * 0.5f + bw * 0.5f; s_bh = s_bh * 0.5f + bh * 0.5f; }

        // 水平偏差：平滑中心偏离画面中心的比例，-100(极左) ~ +100(极右)
        int dev = (int)((s_cx - w / 2.0f) * 100.0f / (w / 2.0f));
        if (dev < -100) dev = -100;
        if (dev > 100)  dev = 100;
        int8_t off = (int8_t)dev;

        // 距离用框宽占画面比例判断（框高在3米内已饱和顶格，框宽仍线性可用）
        // 实测3米≈wratio0.27，反推2米≈0.40
        float w_ratio = s_bw / (float)w;
        float h_ratio = s_bh / (float)h;
        int8_t d;
        if (w_ratio >= 0.44f)       d = 3;  // <1.8米 → 后退
        else if (w_ratio >= 0.36f)  d = 2;  // 1.8~2.2米 → 静止
        else                        d = 1;  // >2.2米 → 前进

        s_last_has = 1;
        s_last_offset = off;
        s_last_dist = d;
        *has = 1;
        *offset = off;
        *dist = d;
        ESP_LOGI(TAG, "person score=%.2f box=[%d,%d,%d,%d] wratio=%.2f hratio=%.2f off=%d dist=%d",
                 best->score, (int)best->box[0], (int)best->box[1], (int)best->box[2], (int)best->box[3],
                 w_ratio, h_ratio, off, d);
    } else if (s_confirm <= -2) {
        // 确认无人：重置平滑状态，下次检测从新位置开始
        s_last_has = 0;
        s_last_offset = 0;
        s_last_dist = 0;
        s_cx = -1;
        s_bw = -1;
        s_bh = -1;
    } else {
        // 中间态：维持上次判断，避免目标闪烁
        if (s_last_has) {
            *has = 1;
            *offset = s_last_offset;
            *dist = s_last_dist;
        }
    }
}