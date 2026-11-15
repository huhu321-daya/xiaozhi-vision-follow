#include "cam_net.h"
#include "cam_config.h"
#include "cam_camera.h"
#include "cam_vlm.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "lwip/sockets.h"

#define TAG "net"

static volatile bool s_up;

// 跟随开关：由接收板通过 FOLLOW_ON/FOLLOW_OFF 控制
volatile bool cam_follow_active = false;

/* ── 接收板握手状态（AHUHU 每 1s 广播 AHUHU_HELLO 到 8094）── */
static volatile bool     s_peer_online;
static volatile uint32_t s_peer_last_ms;
static char              s_peer_ip[16];

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected reason=%d", d ? d->reason : -1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        s_up = true;
        ESP_LOGI(TAG, "ip=" IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static bool scan_24g(wifi_ap_record_t *out)
{
    for (int round = 0; round < 4; round++) {
        wifi_scan_config_t sc = {0};
        sc.ssid        = (uint8_t *)CAM_WIFI_SSID;
        sc.show_hidden = false;
        if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint16_t cnt = 20;
        wifi_ap_record_t recs[20];
        if (esp_wifi_scan_get_ap_records(&cnt, recs) == ESP_OK) {
            for (int i = 0; i < cnt; i++) {
                ESP_LOGI(TAG, "ap ch=%d rssi=%d auth=%d", recs[i].primary, recs[i].rssi, recs[i].authmode);
                if (recs[i].primary >= 1 && recs[i].primary <= 14) {
                    *out = recs[i];
                    return true;
                }
            }
        }
    }
    return false;
}

bool cam_net_wifi_start(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CAM_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CAM_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;

    wifi_ap_record_t ap = {0};
    if (scan_24g(&ap)) {
        memcpy(wc.sta.bssid, ap.bssid, 6);
        wc.sta.bssid_set = true;
        wc.sta.channel   = ap.primary;
        if (ap.authmode == WIFI_AUTH_OPEN) wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "lock 2.4G ch=%d auth=%d rssi=%d", ap.primary, ap.authmode, ap.rssi);
    } else {
        ESP_LOGW(TAG, "2.4G '%s' not found, connect by ssid", CAM_WIFI_SSID);
    }

    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);

    ESP_LOGI(TAG, "connecting '%s'...", CAM_WIFI_SSID);
    esp_wifi_connect();

    for (int i = 0; i < 40 && !s_up; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return s_up;
}

static void discover_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(CAM_UDP_DISCOVER);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %d failed", CAM_UDP_DISCOVER);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "discover on %d", CAM_UDP_DISCOVER);

    char buf[80];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = '\0';
        if (!strstr(buf, CAM_DISCOVER_TAG)) continue;

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            char reply[20];
            int rl = snprintf(reply, sizeof(reply), IPSTR, IP2STR(&ip.ip));
            sendto(sock, reply, rl, 0, (struct sockaddr *)&from, fl);
            ESP_LOGI(TAG, "discover -> %s", reply);
        }
    }
}

static void follow_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(CAM_UDP_FOLLOW);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    int sweep_step = 0;          // 无目标扫描步数
    int sweep_dir  = 1;          // 扫描方向（1=右转 3=左转）
    uint32_t last_seen_ms = 0;   // 最后有目标时刻，用于扫描前先停顿
    int last_dir = -1, last_speed = -1, last_dur = -1;

    for (;;) {
        if (cam_follow_active) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            int8_t has, offset, dist;
            cam_camera_follow(&has, &offset, &dist);

            // 云端校准结果（大脑）
            int v_has = -1, v_zone = 0, v_dist = -1;
            uint32_t v_last = 0;
            cam_vlm_get(&v_has, &v_zone, &v_dist, &v_last);
            uint32_t vlm_age = now - v_last;

            // ── 决策 ──
            int dir = 0, speed = 0, dur = 0;
            if (!has) {
                // 无目标：左右交替扫描找人，朝一个方向扫几步后换向，来回扫直到发现人
                if (now - last_seen_ms >= 1200) {
                    last_seen_ms = now;
                    dir = sweep_dir; speed = 70; dur = 150;
                    sweep_step++;
                    if (sweep_step >= 5) {           // 朝一个方向扫 5 步后换向
                        sweep_step = 0;
                        sweep_dir = (sweep_dir == 1) ? 3 : 1;
                    }
                }
            } else {
                sweep_step = 0;      // 找到人，重置扫描计数
                last_seen_ms = now;

                if (v_has == 0 && vlm_age < CAM_VLM_VALID_MS) {
                    // 云端判无人（6s 内有效）：本地误检，停住不追
                    dir = 0;
                } else {
                    // 距离以本地框高 hratio 为准（实时硬测量），云端 dist 仅作日志参考
                    if (offset > 15)       sweep_dir = 1;   // 目标偏右 → 右转找人
                    else if (offset < -15) sweep_dir = 3;   // 目标偏左 → 左转找人
                    int a = offset < 0 ? -offset : offset;
                    if (a <= 15) {
                        if (dist >= 3)      { dir = 4; speed = 60; dur = 250; }  // 近→前进（朝后装，前进=远离）
                        else if (dist <= 1) { dir = 2; speed = 90; dur = 250; }  // 远→后退（朝后装，后退=靠近）
                        else                 dir = 0;                            // 中→停止
                    } else {
                        dur = 220 * a / 100;
                        if (dur < 50) dur = 50;
#if CAM_TURN_INVERT
                        bool turn_left = (offset > 0);
#else
                        bool turn_left = (offset < 0);
#endif
                        dir = turn_left ? 3 : 1;
                        speed = 70;
                    }
                }
            }

            // ── 发指令：动作态每次发（脉冲式），停止态仅在动作变化时发 ──
            bool is_stop = (dir == 0 && speed == 0 && dur == 0);
            if (!is_stop || last_dir != 0) {
                char msg[32];
                int n = snprintf(msg, sizeof(msg), "MOTOR %d %d %d\n", dir, speed, dur);
                sendto(sock, msg, n, 0, (struct sockaddr *)&dst, sizeof(dst));
            }
            // 动作变化时打日志：大小脑状态 + 指令
            if (dir != last_dir || speed != last_speed || dur != last_dur) {
                last_dir = dir; last_speed = speed; last_dur = dur;
                ESP_LOGI(TAG, "DECIDE local[has=%d off=%d dist=%d] brain[has=%d dist=%d] -> MOTOR d=%d s=%d ms=%d%s",
                         has, offset, dist, v_has, v_dist, dir, speed, dur,
                         s_peer_online ? "" : " (no receiver)");
            }

            if (has) vTaskDelay(pdMS_TO_TICKS(300));  // 有目标动作后短静默
        } else {
            sweep_step = 0;
            last_dir = last_speed = last_dur = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(220));
    }
}

static void hello_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(CAM_UDP_HELLO);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %d failed", CAM_UDP_HELLO);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "hello listen on %d (waiting receiver...)", CAM_UDP_HELLO);

    char buf[40];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, CAM_HELLO_TAG)) {
                const char *ip = inet_ntoa(from.sin_addr);
                bool first = !s_peer_online || strcmp(s_peer_ip, ip) != 0;
                s_peer_online = true;
                strlcpy(s_peer_ip, ip, sizeof(s_peer_ip));
                s_peer_last_ms = now;
                if (first) {
                    ESP_LOGI(TAG, "receiver online ip=%s", ip);
                }
            } else if (strstr(buf, CAM_FOLLOW_ON_TAG)) {
                if (!cam_follow_active) {
                    cam_follow_active = true;
                    ESP_LOGI(TAG, "FOLLOW ON (receiver %s)", inet_ntoa(from.sin_addr));
                }
            } else if (strstr(buf, CAM_FOLLOW_OFF_TAG)) {
                if (cam_follow_active) {
                    cam_follow_active = false;
                    ESP_LOGI(TAG, "FOLLOW OFF (receiver %s)", inet_ntoa(from.sin_addr));
                }
            }
        }

        // 心跳丢失 5 秒判离线
        if (s_peer_online && (now - s_peer_last_ms) > 5000) {
            ESP_LOGW(TAG, "receiver offline (ip=%s)", s_peer_ip);
            s_peer_online = false;
        }
    }
}

void cam_net_udp_start(void)
{
    xTaskCreate(discover_task, "disc",   3072, NULL, 5, NULL);
    xTaskCreate(follow_task,   "follow", 4096, NULL, 3, NULL);
    xTaskCreate(hello_task,    "hello",  3072, NULL, 3, NULL);
}