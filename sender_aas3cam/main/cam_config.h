#ifndef CAM_CONFIG_H
#define CAM_CONFIG_H

#include <driver/gpio.h>

/* 以下凭据不入库：编译时用 -DCAM_WIFI_SSID=\"...\" 覆盖，或本地填写（勿提交） */
#ifndef CAM_WIFI_SSID
#define CAM_WIFI_SSID       "YOUR_WIFI_SSID"
#endif
#ifndef CAM_WIFI_PASSWORD
#define CAM_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#endif

#define CAM_PIN_D0          GPIO_NUM_11
#define CAM_PIN_D1          GPIO_NUM_9
#define CAM_PIN_D2          GPIO_NUM_8
#define CAM_PIN_D3          GPIO_NUM_10
#define CAM_PIN_D4          GPIO_NUM_12
#define CAM_PIN_D5          GPIO_NUM_18
#define CAM_PIN_D6          GPIO_NUM_17
#define CAM_PIN_D7          GPIO_NUM_16
#define CAM_PIN_XCLK        GPIO_NUM_15
#define CAM_PIN_PCLK        GPIO_NUM_13
#define CAM_PIN_VSYNC       GPIO_NUM_6
#define CAM_PIN_HREF        GPIO_NUM_7
#define CAM_PIN_SIOC        GPIO_NUM_5
#define CAM_PIN_SIOD        GPIO_NUM_4
#define CAM_PIN_PWDN        GPIO_NUM_NC
#define CAM_PIN_RESET       GPIO_NUM_NC

#define CAM_XCLK_HZ         10000000
#define CAM_CAPTURE_FPS     15
#define CAM_FRAME_CAP       (128 * 1024)
#define CAM_FOLLOW_MS       300

#define CAM_UDP_DISCOVER    8092
#define CAM_UDP_FOLLOW      8093
#define CAM_UDP_HELLO       8094
#define CAM_DISCOVER_TAG    "XIAOZHI_CAM_DISCOVER"
#define CAM_HELLO_TAG       "AHUHU_HELLO"
#define CAM_FOLLOW_ON_TAG   "FOLLOW_ON"
#define CAM_FOLLOW_OFF_TAG  "FOLLOW_OFF"

/* ── 云端视觉校准（大脑：硅基流动 Qwen3-VL）── */
#define CAM_VLM_URL       "https://api.siliconflow.cn/v1/chat/completions"
#ifndef CAM_VLM_KEY
#define CAM_VLM_KEY       "YOUR_VLM_API_KEY"   /* 硅基流动 API Key，编译时 -D 注入 */
#endif
#define CAM_VLM_MODEL     "Qwen/Qwen3-VL-8B-Instruct"
#define CAM_VLM_CALIB_MS  2500   // 云端校准周期
#define CAM_VLM_VALID_MS  6000   // 校准结果有效期

/* ── 跟随决策 ── */
#define CAM_TURN_INVERT   1      // 1=反转转向方向（摄像头朝后安装画面左右镜像时用）

#endif