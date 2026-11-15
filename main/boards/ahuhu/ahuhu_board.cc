
#include "ahuhu_board.h"
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "avi_player.h"
#include "mjpeg_player.h"
#include "config.h"
#include "sdkconfig.h"
#ifdef DISPLAY_TYPE_ST7735S
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "display/lvgl_display/lvgl_image.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "backlight.h"
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#else
#include "display/oled_display.h"
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#endif
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "mcp_server.h"
#include "led/circular_strip.h"
#include "assets/lang_config.h"
#include "weather_service.h"
#include "news_service.h"
#include "timer_service.h"
#include "music.h"
#include "settings.h"
#include "web_server.h"
#if CONFIG_ENABLE_MESSAGE_PUSHER
#include "message_pusher.h"
#endif
#include <wifi_manager.h>
#include <dirent.h>
#include <font_awesome.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_netif.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <cJSON.h>
#include <atomic>
#include <time.h>
#include <sys/time.h>
#include <utility>
#include <string>
#include <vector>
#include <map>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <arpa/inet.h>

#ifndef DISPLAY_TYPE_ST7735S
#include "esp32_camera.h"
#include "display/lvgl_display/jpg/image_to_jpeg.h"
#include "esp_video_init.h"
#include <linux/videodev2.h>
#endif

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "AhuhuBoard"

// 视觉大模型（大脑）：拉取 AAS3-CAM 画面后调用硅基流动 Qwen3-VL 做语义理解
#define ZHIPU_VLM_URL   "https://api.siliconflow.cn/v1/chat/completions"
#ifndef ZHIPU_VLM_KEY
#define ZHIPU_VLM_KEY   "YOUR_VLM_API_KEY"   /* 编译时 -DZHIPU_VLM_KEY=\"sk-...\" 注入 */
#endif
#define ZHIPU_VLM_MODEL "Qwen/Qwen3-VL-8B-Instruct"

static constexpr size_t kStandbyViewChars = 16;
static constexpr TickType_t kStandbyTickMs = pdMS_TO_TICKS(1000);

extern "C" void HandleMotorActionForApplication(int direction, int speed, int duration_ms, int priority);
static void SetMotorDirectionGpio(int direction);

// 大灯模式标志：设为 true 时跳过 OnStateChanged，保持大灯颜色不被状态机覆盖
static std::atomic<bool> g_lamp_mode_active{false};

// 摄像头 XCLK 占用 GPIO48 标志：设为 true 时跳过 OnStateChanged，防止 LED 抢夺 GPIO48
static std::atomic<bool> g_camera_xclk_active{false};

static std::string g_control_server_ip;

/* ── 视觉跟随状态（决策在发送板，本板只执行电机指令并转发开关）── */
static std::atomic<bool> g_follow_active{false};   // 跟随开关（语音 follow.start/stop 控制，转发给发送板）

// 广播跟随开关到发送板（8094），发送板据此启动/停止跟随决策
static void SendFollowControl(bool on) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;
    int b = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &b, sizeof(b));
    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(8094);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char* msg = on ? "FOLLOW_ON" : "FOLLOW_OFF";
    sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&dst, sizeof(dst));
    close(sock);
    ESP_LOGI(TAG, "send %s to camera", msg);
}

static void StartFollowService() {
    // 1) 接收发送板下发的电机指令（MOTOR dir speed duration_ms）
    xTaskCreate([](void*) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) vTaskDelete(nullptr);
        struct sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(8093);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr*)&local, sizeof(local)) != 0) vTaskDelete(nullptr);
        struct timeval tv = {1, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[64];
        while (true) {
            int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
            if (n > 0) {
                buf[n] = '\0';
                if (strncmp(buf, "MOTOR", 5) == 0) {
                    int dir = 0, speed = 0, dur = 0;
                    if (sscanf(buf, "MOTOR %d %d %d", &dir, &speed, &dur) >= 3) {
                        HandleMotorActionForApplication(dir, speed, dur, 2);
                        static int s_last_dir = -1, s_last_speed = -1, s_last_dur = -1;
                        if (dir != s_last_dir || speed != s_last_speed || dur != s_last_dur) {
                            s_last_dir = dir; s_last_speed = speed; s_last_dur = dur;
                            ESP_LOGI(TAG, "MOTOR cmd: dir=%d speed=%d ms=%d", dir, speed, dur);
                        }
                    }
                }
            }
        }
    }, "cam_follow_recv", 3072, nullptr, 4, nullptr);

    // 2) 握手心跳：每 1s 广播 AHUHU_HELLO 到 8094，让发送板日志显示"接收板已连接"
    xTaskCreate([](void*) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) vTaskDelete(nullptr);
        int on = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

        struct sockaddr_in dst = {};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(8094);
        dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        const char* msg = "AHUHU_HELLO";
        while (true) {
            sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&dst, sizeof(dst));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }, "cam_follow_hello", 3072, nullptr, 3, nullptr);

    ESP_LOGI(TAG, "Motor executor started (recv UDP:8093 + hello UDP:8094)");
}

static std::string DiscoverControlServer() {
    if (!g_control_server_ip.empty()) return g_control_server_ip;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGW(TAG, "Discovery: socket failed, using default");
        g_control_server_ip = "192.168.1.66";
        return g_control_server_ip;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(8091);
    dest.sin_addr.s_addr = inet_addr("255.255.255.255");

    const char* msg = "XIAOZHI_DISCOVER";
    sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&dest, sizeof(dest));

    char buf[256];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&src, &src_len);
    close(sock);

    if (n > 0) {
        buf[n] = '\0';
        cJSON* root = cJSON_Parse(buf);
        if (root) {
            cJSON* host = cJSON_GetObjectItem(root, "host");
            if (cJSON_IsString(host) && host->valuestring) {
                g_control_server_ip = host->valuestring;
                ESP_LOGI(TAG, "Discovered control server: %s", g_control_server_ip.c_str());
            }
            cJSON_Delete(root);
        }
    }
    if (g_control_server_ip.empty()) {
        g_control_server_ip = "192.168.1.66";
        ESP_LOGW(TAG, "Discovery failed, using default IP");
    }
    return g_control_server_ip;
}

static MjpegPlayer& GetMjpegPlayer() {
    static MjpegPlayer instance;
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        instance.SetOnEnd([]() {
            Application::GetInstance().Schedule([]() {
                ESP_LOGW(TAG, "Mjpeg playback ended, restoring standby");
                auto* lcd = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
                if (lcd == nullptr) return;
                Application::GetInstance().GetAudioService().SetMusicPlaying(false);
                Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
                Application::GetInstance().SetVideoQuietMode(false);
                lcd->SetMusicMode(false);
                lcd->Resume();
                lcd->ShowStandbyContent();
                lcd->ShowStatusBar();
            });
        });
    }
    return instance;
}

// 从 HTTP 下载响应体到内存（用于拍照拉取单帧 JPEG）
static bool HttpGetToMemory(const char* url, std::vector<uint8_t>& out) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    out.clear();
    if (content_len > 0) out.reserve((size_t)content_len);

    char buf[1024];
    int read = 0;
    while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        out.insert(out.end(), buf, buf + read);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return !out.empty();
}

// 大脑：把 AAS3-CAM 拉到的 JPEG 交给智谱 glm-4v-flash，返回画面理解结果（JSON）
static std::string AskZhipu(const std::vector<uint8_t>& jpeg, const std::string& question) {
    // 1. JPEG -> base64
    size_t b64_len = 0, olen = 0;
    mbedtls_base64_encode(nullptr, 0, &b64_len, jpeg.data(), jpeg.size());
    std::string b64(b64_len, '\0');
    int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&b64[0]),
                                   b64_len, &olen, jpeg.data(), jpeg.size());
    if (rc != 0) {
        return "{\"success\": false, \"message\": \"图片编码失败\"}";
    }
    if (!b64.empty() && b64.back() == '\0') b64.pop_back();

    // 2. 构造智谱 OpenAI 兼容请求体
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", ZHIPU_VLM_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", 200);
    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON* content = cJSON_AddArrayToObject(msg, "content");
    cJSON* text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text",
                            question.empty() ? "请用一句中文简要描述这张照片的内容，控制在50个字以内" : question.c_str());
    cJSON_AddItemToArray(content, text_part);
    cJSON* img_part = cJSON_CreateObject();
    cJSON_AddStringToObject(img_part, "type", "image_url");
    cJSON* img_url = cJSON_CreateObject();
    std::string data_url = "data:image/jpeg;base64," + b64;
    cJSON_AddStringToObject(img_url, "url", data_url.c_str());
    cJSON_AddItemToObject(img_part, "image_url", img_url);
    cJSON_AddItemToArray(content, img_part);
    cJSON_AddItemToArray(messages, msg);
    char* json_str = cJSON_PrintUnformatted(root);
    std::string body(json_str ? json_str : "");
    cJSON_free(json_str);
    cJSON_Delete(root);
    if (body.empty()) {
        return "{\"success\": false, \"message\": \"请求构造失败\"}";
    }

    // 3. HTTPS 请求智谱
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return "{\"success\": false, \"message\": \"网络不可用\"}";
    }
    auto http = network->CreateHttp(3);
    if (!http) {
        return "{\"success\": false, \"message\": \"网络连接创建失败\"}";
    }
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Authorization", "Bearer " ZHIPU_VLM_KEY);
    http->SetTimeout(30000);
    http->SetContent(std::move(body));
    if (!http->Open("POST", ZHIPU_VLM_URL)) {
        return "{\"success\": false, \"message\": \"连接智谱服务失败\"}";
    }
    int status = http->GetStatusCode();
    std::string result = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "AskZhipu: status=%d result_len=%d", status, (int)result.size());
    if (status != 200) {
        ESP_LOGE(TAG, "AskZhipu: status=%d resp=%s", status, result.c_str());
        return "{\"success\": false, \"message\": \"视觉服务返回错误\"}";
    }

    // 4. 解析返回的 content
    std::string answer;
    cJSON* resp = cJSON_Parse(result.c_str());
    if (resp != nullptr) {
        cJSON* choices = cJSON_GetObjectItem(resp, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON* first = cJSON_GetArrayItem(choices, 0);
            cJSON* message = cJSON_GetObjectItem(first, "message");
            cJSON* c = cJSON_GetObjectItem(message, "content");
            if (cJSON_IsString(c)) answer = c->valuestring;
        }
        cJSON_Delete(resp);
    }
    if (answer.empty()) {
        ESP_LOGW(TAG, "AskZhipu: parse failed, resp=%.300s", result.c_str());
        return "{\"success\": false, \"message\": \"视觉服务未返回内容\"}";
    }
    ESP_LOGI(TAG, "AskZhipu: answer=%s", answer.c_str());

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "success", true);
    cJSON_AddStringToObject(out, "result", answer.c_str());
    char* out_str = cJSON_PrintUnformatted(out);
    std::string ret(out_str ? out_str : "{\"success\": false}");
    cJSON_free(out_str);
    cJSON_Delete(out);
    return ret;
}

// 看直播/摄像头画面时关闭语音唤醒，避免小智被电视声音误唤醒、插话打扰
static void SetVideoQuietMode(bool enable) {
    auto& app = Application::GetInstance();
    app.SetVideoQuietMode(enable);
    app.GetAudioService().EnableWakeWordDetection(!enable);

    auto* lcd = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    if (lcd != nullptr) {
        if (enable) {
            lcd->HideStatusBar();
        } else {
            lcd->ShowStatusBar();
        }
    }

    if (enable) {
        app.AbortSpeaking(kAbortReasonNone);
    } else if (app.GetDeviceState() != kDeviceStateIdle) {
        app.SetDeviceState(kDeviceStateIdle);
    }
}

// 局域网发现 AASXT 摄像头板：广播 XIAOZHI_CAM_DISCOVER，返回摄像头IP
static std::string DiscoverCamera() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "";

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(8092);
    dest.sin_addr.s_addr = inet_addr("255.255.255.255");

    const char* msg = "XIAOZHI_CAM_DISCOVER";
    // 部分路由器不转发 255.255.255.255 全局广播，需同时发到子网定向广播
    sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&dest, sizeof(dest));
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
        uint32_t bcast = (ip_info.ip.addr & ip_info.netmask.addr) | (~ip_info.netmask.addr);
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d.%d.%d.%d",
                 (int)((bcast >> 0) & 0xFF), (int)((bcast >> 8) & 0xFF),
                 (int)((bcast >> 16) & 0xFF), (int)((bcast >> 24) & 0xFF));
        dest.sin_addr.s_addr = inet_addr(tmp);
        sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&dest, sizeof(dest));
    }

    char buf[64];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    // 只取第一个有效响应，反复接收直到超时
    for (int i = 0; i < 3; i++) {
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&src, &src_len);
        if (n > 0) {
            buf[n] = '\0';
            std::string ip(buf);
            ESP_LOGI(TAG, "Discovered camera: %s", ip.c_str());
            close(sock);
            return ip;
        }
    }
    close(sock);
    ESP_LOGW(TAG, "Camera discovery timed out");
    return "";
}

// 包装 CircularStrip：大灯/摄像头激活时不响应设备状态变化，保持当前颜色
class LampAwareCircularStrip : public CircularStrip {
public:
    LampAwareCircularStrip(gpio_num_t gpio, uint8_t max_leds)
        : CircularStrip(gpio, max_leds) {}

    void OnStateChanged() override {
        if (g_lamp_mode_active || g_camera_xclk_active) {
            return;
        }
        CircularStrip::OnStateChanged();
    }
};

class AhuhuBoard : public WifiBoard {
private:
#ifdef DISPLAY_TYPE_ST7735S
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
#else
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
#endif
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

#ifndef DISPLAY_TYPE_ST7735S
    Esp32Camera* camera_ = nullptr;
    bool camera_preview_active_ = false;
    TaskHandle_t camera_preview_task_ = nullptr;
#endif

    bool standby_task_started_ = false;
    bool weather_started_ = false;
    WebServer* web_server_ = nullptr;
    esp_timer_handle_t screen_off_timer_ = nullptr;
    bool clock_mode_ = false;
    int clock_mode_ticks_ = 0;
    static constexpr int kScreenOffTimeoutSec = 300;

    // SD卡文件夹选择菜单
    bool sd_menu_mode_ = false;
    int sd_menu_index_ = 0;
    const std::vector<std::string> sd_folders_ = {"音乐歌曲", "国学", "儿歌", "相声", "戏曲", "故事", "轻音乐", "多多儿歌"};
    const std::vector<std::string> sd_folder_names_ = {"音乐歌曲", "国学", "儿歌", "相声", "戏曲", "故事", "轻音乐", "多多儿歌"};

    // 歌曲选择菜单（第二级）
    bool sd_song_menu_ = false;
    int sd_song_index_ = 0;
    std::vector<std::string> sd_song_names_;
    std::string sd_selected_folder_;
    int64_t last_menu_display_update_ = 0;  // 防抖：上次显示更新时间

    static void WeatherInitTask(void* arg) {
        auto& app = Application::GetInstance();
        while (app.GetDeviceState() != kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        ESP_LOGI(TAG, "Trying to fetch weather data...");
        WeatherService::GetInstance()->FetchWeather();
        WeatherService::GetInstance()->StartAutoUpdate(3600000);

        NewsService::GetInstance()->Init();
        NewsService::GetInstance()->StartAutoUpdate(120000);

        auto* board = static_cast<AhuhuBoard*>(arg);
        board->weather_started_ = true;
        ESP_LOGI(TAG, "Weather+News init task completed");
        vTaskDelete(nullptr);
    }

    void UpdateSdMenuDisplay() {
        int64_t now = esp_timer_get_time();
        if (now - last_menu_display_update_ < 100000) return;  // 100ms 防抖
        last_menu_display_update_ = now;
        auto* display = GetDisplay();
        if (!display) return;
        // 3行×2列布局，128x64 OLED 14px字体恰好显示4行
        std::string menu_text;
        for (int row = 0; row < 3; row++) {
            int left_idx = row;
            int right_idx = row + 3;
            // 左列
            if (sd_menu_index_ == left_idx) {
                menu_text += "▶" + sd_folder_names_[left_idx];
            } else {
                menu_text += " " + sd_folder_names_[left_idx];
            }
            menu_text += " ";  // 列间距
            // 右列
            if (sd_menu_index_ == right_idx) {
                menu_text += "▶" + sd_folder_names_[right_idx];
            } else {
                menu_text += " " + sd_folder_names_[right_idx];
            }
            if (row < 2) menu_text += "\n";
        }
        menu_text += "\n←→选择 BOOT确定";
        display->ShowSdFolderMenu(menu_text.c_str());
    }

    void EnterSdCardMenuMode() {
        if (sd_menu_mode_) return;
        sd_menu_mode_ = true;
        sd_menu_index_ = 0;
        UpdateSdMenuDisplay();
        ESP_LOGI(TAG, "Entered SD card menu mode");
    }

    void ExitSdCardMenuMode() {
        if (!sd_menu_mode_) return;
        sd_menu_mode_ = false;
        auto* display = GetDisplay();
        if (display) {
            display->HideSdFolderMenu();
        }
        ESP_LOGI(TAG, "Exited SD card menu mode");
    }

    void UpdateSdSongDisplay() {
        int64_t now = esp_timer_get_time();
        if (now - last_menu_display_update_ < 100000) return;  // 100ms 防抖
        last_menu_display_update_ = now;
        auto* display = GetDisplay();
        if (!display || sd_song_names_.empty()) return;
        std::string text;
        int start = sd_song_index_ - (sd_song_index_ % 4);
        if (start < 0) start = 0;
        for (int i = start; i < (int)sd_song_names_.size() && i < start + 4; i++) {
            if (i == sd_song_index_) {
                text += "▶";
            } else {
                text += " ";
            }
            std::string name = sd_song_names_[i];
            if (name.length() > 24) {
                int len = 24;
                while (len > 0 && (name[len] & 0xC0) == 0x80) len--;
                name = name.substr(0, len) + "~";
            }
            text += name + "\n";
        }
        display->ShowSdFolderMenu(text.c_str());
    }

    void EnterSdSongMenu(const std::string& folder) {
        auto music = GetMusic();
        if (!music) return;
        sd_song_names_ = music->ScanSdCardSongNames(folder);
        if (sd_song_names_.empty()) {
            ESP_LOGW(TAG, "No songs found in %s", folder.c_str());
            return;
        }
        sd_selected_folder_ = folder;
        sd_song_menu_ = true;
        sd_song_index_ = 0;
        UpdateSdSongDisplay();
        ESP_LOGI(TAG, "Entered song menu: %s, %d songs", folder.c_str(), (int)sd_song_names_.size());
    }

    void ExitSdSongMenu() {
        if (!sd_song_menu_) return;
        sd_song_menu_ = false;
        sd_song_names_.clear();
        UpdateSdMenuDisplay();  // 回到文件夹菜单
        ESP_LOGI(TAG, "Exited song menu, back to folder menu");
    }

    #ifdef DISPLAY_TYPE_ST7735S
    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCLK;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
        ESP_LOGI(TAG, "SPI bus initialized");
    }

    void InitializeSt7735Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install ST7789 driver configured for ST7735S");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
        // 背光引脚 GPIO19 = OCTAL PSRAM D4 (CONFIG_SPIRAM_MODE_OCT=y)，
        // 被PSRAM硬件占用无法软件控制，跳过GPIO背光操作，
        // 由ST7735S的DISPOFF(0x28)/DISPON(0x29)命令+像素填充控制亮黑屏
        ESP_LOGI(TAG, "Skip GPIO%d BL pin control (reserved by OCTAL PSRAM D4). Using ST7735S DISPOFF/DISPON for on/off", DISPLAY_BACKLIGHT);
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // 创建LCD面板 - 使用ST7789驱动模拟ST7735S
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        
        // 方向配置由 OledDisplay 构造函数统一处理（使用 config.h 中的宏定义）
        // 此处不再重复设置，避免双重镜像问题
        ESP_LOGI(TAG, "DISPLAY_INVERT_COLOR=%d, but NOT applying globally - will toggle during screen-off only", DISPLAY_INVERT_COLOR);
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 0));
        ESP_LOGI(TAG, "Panel gap set to (0, 0)");

        Settings settings("display", true);
        settings.SetString("theme", "dark");
        ESP_LOGI(TAG, "Forced theme to 'dark' in NVS");

        // 使用ST7735S原生分辨率128x160 - LVGL旋转参数修正方向
        display_ = new SpiLcdDisplay(panel_io, panel,
            128, 160, 0, 0,
            1, 1, 0);
        ESP_LOGI(TAG, "ST7735 display initialized");

        panel_io_ = panel_io;
        panel_ = panel;

        auto& theme_manager = LvglThemeManager::GetInstance();
        auto theme = theme_manager.GetTheme("dark");
        if (theme != nullptr) {
            display_->SetTheme(theme);
            ESP_LOGI(TAG, "Set dark theme (no gradient)");
        }

        vTaskDelay(pdMS_TO_TICKS(500));

        if (!standby_task_started_) {
            standby_task_started_ = true;
            xTaskCreatePinnedToCore([](void* arg) {
                static_cast<AhuhuBoard*>(arg)->UpdateStandbyDisplayTask();
                vTaskDelete(nullptr);
            }, "StandbyDisplay", 8192, this, tskIDLE_PRIORITY + 1, nullptr, 0);
        }
    }
#endif

    #ifndef DISPLAY_TYPE_ST7735S
    void InitializeDisplayI2c() {
        ESP_LOGI(TAG, "InitializeDisplayI2c starting...");
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        esp_err_t err = i2c_new_master_bus(&bus_config, &display_i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "I2C bus created");
    }
#endif // DISPLAY_TYPE_ST7735S

    #ifndef DISPLAY_TYPE_ST7735S
    void InitializeCamera() {
        ESP_LOGI(TAG, "InitializeCamera starting...");

        if (!display_i2c_bus_) {
            ESP_LOGE(TAG, "I2C bus not initialized, cannot initialize camera");
            return;
        }

        // Wait for OLED to finish any in-progress I2C transaction before camera init
        vTaskDelay(pdMS_TO_TICKS(200));

        // 释放 GPIO48：LED(RMT) → 摄像头 XCLK（仅首次执行，避免重复复位破坏 XCLK 时钟输出）
        if (!g_camera_xclk_active) {
            g_camera_xclk_active = true;
            gpio_reset_pin(GPIO_NUM_48);
            ESP_LOGI(TAG, "GPIO48 reset for camera XCLK (first time)");
        }

        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = display_i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        // 尝试初始化摄像头，添加超时保护
        esp_err_t ret = ESP_OK;
        camera_ = new Esp32Camera(video_config);
        if (camera_->GetVideoFd() < 0) {
            ESP_LOGW(TAG, "Camera init failed - OV7670 may not be connected or pins are wrong");
            delete camera_;
            camera_ = nullptr;
        } else {
            ESP_LOGI(TAG, "Camera initialized successfully, video_fd=%d", camera_->GetVideoFd());
        }
    }
#endif

#ifndef DISPLAY_TYPE_ST7735S
    void StartCameraPreview() {
        if (!camera_) {
            ESP_LOGI(TAG, "Initializing camera on demand...");
            InitializeCamera();
        }
        if (!camera_) {
            ESP_LOGW(TAG, "Camera init failed, preview unavailable");
            GetDisplay()->ShowNotification("未检测到摄像头");
            return;
        }
        if (camera_preview_active_) {
            ESP_LOGI(TAG, "Camera preview already active");
            return;
        }
        camera_preview_active_ = true;
        espgui::OledDisplay::SetCameraPreviewActive(true);
        xTaskCreate([](void* arg) {
            static_cast<AhuhuBoard*>(arg)->CameraPreviewTask();
        }, "cam_preview", 8192, this, 5, &camera_preview_task_);
    }

    void StopCameraPreview() {
        camera_preview_active_ = false;
        espgui::OledDisplay::SetCameraPreviewActive(false);
        if (camera_preview_task_) {
            vTaskDelay(pdMS_TO_TICKS(100));
            camera_preview_task_ = nullptr;
        }
    }

    void CameraPreviewTask() {
        ESP_LOGI(TAG, "Camera preview task started");
        TickType_t last_frame = xTaskGetTickCount();
        int frame_count = 0;

        while (camera_preview_active_) {
            if (!camera_->Capture()) {
                static int capture_fail_count = 0;
                if (++capture_fail_count == 1) {
                    ESP_LOGW(TAG, "Camera capture failed, will retry...");
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            auto& frame = camera_->GetFrame();
            if (!frame.data || frame.format != V4L2_PIX_FMT_RGB565) {
                static int fmt_fail = 0;
                if (++fmt_fail == 1) {
                    ESP_LOGW(TAG, "Camera frame bad: data=%p fmt=0x%x (need 0x%x) w=%d h=%d",
                        frame.data, frame.format, V4L2_PIX_FMT_RGB565, frame.width, frame.height);
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            if (frame_count == 0) {
                ESP_LOGI(TAG, "Camera first frame OK: %dx%d fmt=0x%x", frame.width, frame.height, frame.format);
            }

            uint8_t oled_buf[128 * 64 / 8];
            memset(oled_buf, 0, sizeof(oled_buf));

            int src_w = frame.width;
            int src_h = frame.height;
            uint16_t* src = (uint16_t*)frame.data;

            for (int y = 0; y < 64; y++) {
                int src_y = y * src_h / 64;
                for (int x = 0; x < 128; x++) {
                    int src_x = x * src_w / 128;
                    uint16_t pixel = src[src_y * src_w + src_x];
                    uint8_t r = (pixel >> 11) & 0x1F;
                    uint8_t g = (pixel >> 5) & 0x3F;
                    uint8_t b = pixel & 0x1F;
                    uint8_t gray = (r * 77 + g * 150 + b * 29) >> 8;
                    if (gray > 3) {
                        int page = y / 8;
                        int bit = y % 8;
                        oled_buf[page * 128 + x] |= (1 << bit);
                    }
                }
            }

            esp_err_t err = esp_lcd_panel_draw_bitmap(panel_, 0, 0, 128, 64, oled_buf);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Camera draw_bitmap failed: %s", esp_err_to_name(err));
            } else if (++frame_count % 30 == 1) {
                ESP_LOGI(TAG, "Camera frame #%d drawn OK", frame_count);
            }

            TickType_t now = xTaskGetTickCount();
            int elapsed = (now - last_frame) * portTICK_PERIOD_MS;
            if (elapsed < 33) {
                vTaskDelay(pdMS_TO_TICKS(33 - elapsed));
            }
            last_frame = xTaskGetTickCount();
        }

        ESP_LOGI(TAG, "Camera preview task stopped");
        vTaskDelete(nullptr);
    }
#endif // DISPLAY_TYPE_ST7735S

    #ifndef DISPLAY_TYPE_ST7735S
    void InitializeSsd1306Display() {
        ESP_LOGI(TAG, "InitializeSsd1306Display starting...");
        
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400000,
        };

        esp_err_t err = esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(err));
            display_ = &no_display_;
            return;
        }
        ESP_LOGI(TAG, "Panel IO created");

        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = GPIO_NUM_NC,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 1,
        };

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        err = esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_);
#else
        err = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create panel: %s", esp_err_to_name(err));
            display_ = &no_display_;
            return;
        }
        ESP_LOGI(TAG, "SSD1306 driver installed");

        err = esp_lcd_panel_reset(panel_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reset panel: %s", esp_err_to_name(err));
            display_ = &no_display_;
            return;
        }

        err = esp_lcd_panel_init(panel_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display: %s", esp_err_to_name(err));
            display_ = &no_display_;
            return;
        }
        ESP_LOGI(TAG, "Panel initialized");

        err = esp_lcd_panel_invert_color(panel_, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to invert color: %s", esp_err_to_name(err));
        }

        uint8_t cmd_contrast[] = {0x81, 0xCF};
        err = esp_lcd_panel_io_tx_param(panel_io_, 0x81, cmd_contrast + 1, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set contrast: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Contrast set");
        
        err = esp_lcd_panel_disp_on_off(panel_, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to turn on display: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Display turned on");

        vTaskDelay(pdMS_TO_TICKS(50));

        // Test pattern: draw checkerboard to verify hardware
        uint8_t test_buf[128 * 64 / 8];
        memset(test_buf, 0, sizeof(test_buf));
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 128; x++) {
                if ((x / 8 + y / 8) % 2 == 0) {
                    int page = y / 8;
                    int bit = y % 8;
                    test_buf[page * 128 + x] |= (1 << bit);
                }
            }
        }
        esp_lcd_panel_draw_bitmap(panel_, 0, 0, 128, 64, test_buf);
        ESP_LOGI(TAG, "Test pattern drawn (checkerboard)");
        vTaskDelay(pdMS_TO_TICKS(500));

        display_ = new espgui::OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        // Force LVGL initialization and initial render at board init priority
        // (NOT from the low-priority standby task)
        if (display_) {
            auto* oled = static_cast<espgui::OledDisplay*>(display_);
            oled->ForceInit();
        }

        if (!standby_task_started_) {
            standby_task_started_ = true;
            xTaskCreatePinnedToCore([](void* arg) {
                static_cast<AhuhuBoard*>(arg)->UpdateStandbyDisplayTask();
                vTaskDelete(nullptr);
            }, "StandbyDisplay", 8192, this, tskIDLE_PRIORITY + 1, nullptr, 0);
        }
    }
#endif // DISPLAY_TYPE_ST7735S

    void UpdateStandbyDisplay() {
        if (!display_) return;

        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) return;
        if (display_->IsMusicMode()) return;
        if (app.GetAudioService().IsMusicPlaying()) return;
        if (GetMjpegPlayer().IsPlaying()) return;  // 播放直播/视频时不重绘待机界面，避免时间闪现

        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        // 时钟/箴言模式：9条箴言各5秒 → 时钟10秒 → 循环
        if (clock_mode_) {
            static int quotes_tick_ = 0;
            static int last_quote_idx_ = -1;
            static bool last_was_clock_ = false;
            const int kQuotesPerCycle = 9;
            const int kQuotesEachSec = 15;
            const int kClockSec = 15;
            const int kCycleTotal = kQuotesPerCycle * kQuotesEachSec + kClockSec;

            int tick = quotes_tick_ % kCycleTotal;
            int quote_idx = tick / kQuotesEachSec;
            bool is_clock = (tick >= kQuotesPerCycle * kQuotesEachSec);

            if (is_clock) {
                if (!last_was_clock_) {
                    display_->HideQuotesContent();
                }
                static char clock_last_minute[8] = "";
                char clock_time_buf[8];
                snprintf(clock_time_buf, sizeof(clock_time_buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
                if (strcmp(clock_time_buf, clock_last_minute) != 0) {
                    strncpy(clock_last_minute, clock_time_buf, sizeof(clock_last_minute) - 1);
                    display_->SetClockContent(clock_time_buf);
                }
            } else {
                if (quote_idx != last_quote_idx_) {
                    display_->SetQuotesContent(quote_idx);
                }
            }
            last_quote_idx_ = quote_idx;
            last_was_clock_ = is_clock;
            quotes_tick_++;
            return;
        }

        auto weather = WeatherService::GetInstance()->GetWeather();

        static const char* weekday_cn[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%d月%d日",
                 tm_now.tm_mon + 1, tm_now.tm_mday);

        char center_time_buf[16];
        strftime(center_time_buf, sizeof(center_time_buf), "%H:%M:%S", &tm_now);

        // WiFi图标
        const char* wifi_icon = GetNetworkStateIcon();

        char weather_line_buf[128];
        if (weather.valid) {
            const char* aqi_text = "空气优";
            if (weather.aqi > 0 && weather.aqi <= 50) aqi_text = "空气优";
            else if (weather.aqi <= 100) aqi_text = "空气良";
            else if (weather.aqi <= 150) aqi_text = "轻度污";
            else if (weather.aqi <= 200) aqi_text = "中度污";
            else if (weather.aqi > 200) aqi_text = "重度污";

            char wind_buf[24];
            snprintf(wind_buf, sizeof(wind_buf), "%s%d级",
                weather.wind_direction.c_str(),
                weather.wind_level > 0 ? weather.wind_level : 1);

            char temp_buf[16];
            snprintf(temp_buf, sizeof(temp_buf), "%d°C", weather.temperature);

            char humi_buf[16];
            snprintf(humi_buf, sizeof(humi_buf), "%d%%", weather.humidity);

            snprintf(weather_line_buf, sizeof(weather_line_buf), "%s|%s|%s|%s|%s",
                weather.description.c_str(), aqi_text, wind_buf, temp_buf, humi_buf);
        } else {
            snprintf(weather_line_buf, sizeof(weather_line_buf), "加载中|||");
        }

        static std::string s_news_scroll_text;
        static std::string s_last_headlines_hash;
        std::string all_headlines = NewsService::GetInstance()->GetAllHeadlines();
        if (all_headlines != s_last_headlines_hash) {
            s_last_headlines_hash = all_headlines;
            s_news_scroll_text = std::move(all_headlines);
        }

        display_->SetStandbyFullContent(
            "小智云聊",                     // 顶栏左
            "",                           // 顶栏中（删除"待命"）
            wifi_icon,                     // 顶栏右 WiFi图标(FontAwesome)
            date_buf,                      // 日期 "5月9日"
            weekday_cn[tm_now.tm_wday],    // 星期 "星期六"
            center_time_buf,               // 时间 "HH:MM:SS"
            weather_line_buf,              // 天气数据(管道分隔)
            "漯河市",                       // 城市
            s_news_scroll_text.c_str()           // 底部滚动新闻标题
        );
    }

    void UpdateStandbyDisplayTask() {
        TickType_t delay = kStandbyTickMs;
        while (true) {
            if (!display_) {
                vTaskDelay(delay);
                continue;
            }

            UpdateStandbyDisplay();
            vTaskDelay(delay);
        }
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();

            // 唤醒屏幕（如果在待机黑屏状态）
            WakeUp();

            // 直播模式：按BOOT键退出直播
            if (GetMjpegPlayer().IsPlaying()) {
                GetMjpegPlayer().Stop();
                SetVideoQuietMode(false);
                return;
            }

            // 跟随模式：按BOOT键停止跟随（语音"停"失效时的兜底）
            if (g_follow_active.load()) {
                g_follow_active = false;
                SendFollowControl(false);   // 通知发送板停止决策
                HandleMotorActionForApplication(0, 0, 0, 2);
                return;
            }
            
            // SD卡歌曲菜单模式：播放选中歌曲
            if (sd_song_menu_) {
                auto music = GetMusic();
                if (music) {
                    music->PlaySdCardSongByIndex(sd_song_index_);
                }
                ExitSdCardMenuMode();
                sd_song_menu_ = false;
                sd_song_names_.clear();
                return;
            }
            
            // SD卡菜单模式：确认选择 → 进入歌曲列表
            if (sd_menu_mode_) {
                EnterSdSongMenu(sd_folders_[sd_menu_index_]);
                return;
            }
            
            // 启动过程中单击 → 进入配网模式
            if (app.GetDeviceState() == kDeviceStateStarting) {
                Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_WIFICONFIG);
                EnterWifiConfigMode();
                return;
            }
            
            // 如果正在播放警惕音，单击→关闭提醒
            auto* timer_service = TimerService::GetInstance();
            if (timer_service->IsPlayingAlert()) {
                timer_service->DismissAlert();
                app.AbortSpeaking(kAbortReasonNone);
                return;
            }
            
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            // SD卡歌曲菜单：返回文件夹菜单
            if (sd_song_menu_) {
                ExitSdSongMenu();
                return;
            }
            // SD卡菜单模式：退出菜单
            if (sd_menu_mode_) {
                ExitSdCardMenuMode();
                return;
            }
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_WIFICONFIG);
            EnterWifiConfigMode();
        });

        boot_button_.OnDoubleClick([this]() {
            // 双击BOOT：进入SD卡目录菜单（离线时也能播放SD卡音乐）
            if (sd_menu_mode_ || sd_song_menu_) return;  // 已在菜单中，忽略
            EnterSdCardMenuMode();
        });

        volume_up_button_.OnClick([this]() {
            WakeUp();
            // SD卡歌曲菜单：上移
            if (sd_song_menu_) {
                if (sd_song_index_ > 0) {
                    sd_song_index_--;
                    UpdateSdSongDisplay();
                }
                return;
            }
            // SD卡菜单模式：上一个选项
            if (sd_menu_mode_) {
                if (sd_menu_index_ > 0) {
                    sd_menu_index_--;
                    UpdateSdMenuDisplay();
                }
                return;
            }
            auto music = GetMusic();
            if (music && (music->IsPlaying() || music->IsMusicMode())) {
                music->NextSong();
                return;
            }
            auto codec = GetAudioCodec();
            if (codec == nullptr) return;
            int volume = codec->output_volume() + 10;
            if (volume > 100) volume = 100;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            ESP_LOGI(TAG, "Volume up: %d", volume);
        });

        volume_up_button_.OnLongPress([this]() {
            if (sd_menu_mode_ || sd_song_menu_) return;
            auto codec = GetAudioCodec();
            if (codec == nullptr) return;
            codec->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
            ESP_LOGI(TAG, "Volume max");
        });

        volume_up_button_.OnDoubleClick([this]() {
            // GPIO40 已用作摄像头 HREF，音量+双击不可用
        });

        volume_down_button_.OnClick([this]() {
            WakeUp();
            // SD卡歌曲菜单：下移
            if (sd_song_menu_) {
                if (sd_song_index_ < (int)sd_song_names_.size() - 1) {
                    sd_song_index_++;
                    UpdateSdSongDisplay();
                }
                return;
            }
            // SD卡菜单模式：下一个选项
            if (sd_menu_mode_) {
                if (sd_menu_index_ < (int)sd_folders_.size() - 1) {
                    sd_menu_index_++;
                    UpdateSdMenuDisplay();
                }
                return;
            }
            auto music = GetMusic();
            if (music && (music->IsPlaying() || music->IsMusicMode())) {
                music->NextSong();
                return;
            }
            auto codec = GetAudioCodec();
            if (codec == nullptr) return;
            int volume = codec->output_volume() - 10;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            ESP_LOGI(TAG, "Volume down: %d", volume);
        });

        volume_down_button_.OnLongPress([this]() {
            if (sd_menu_mode_ || sd_song_menu_) return;
            auto codec = GetAudioCodec();
            if (codec == nullptr) return;
            codec->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            ESP_LOGI(TAG, "Volume muted");
        });

        volume_down_button_.OnDoubleClick([this]() {
            if (sd_menu_mode_ || sd_song_menu_) return;
#ifndef DISPLAY_TYPE_ST7735S
            if (camera_preview_active_) {
                StopCameraPreview();
                GetDisplay()->ShowNotification("摄像头已关闭");
            } else if (camera_) {
                StartCameraPreview();
                GetDisplay()->ShowNotification("摄像头预览中");
            } else {
                GetDisplay()->ShowNotification("未检测到摄像头");
            }
#endif
        });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("timer.set_timer",
            "设置一个倒计时提醒。参数：minutes(分钟数), seconds(秒数), title(提醒标题)",
            PropertyList({
                Property("minutes", kPropertyTypeInteger, 0),
                Property("seconds", kPropertyTypeInteger, 0),
                Property("title", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int minutes = properties["minutes"].value<int>();
                int seconds = properties["seconds"].value<int>();
                std::string title = properties["title"].value<std::string>();
                
                int total_seconds = minutes * 60 + seconds;
                int id = TimerService::GetInstance()->SetRelativeTimer(total_seconds, title);
                
                if (id < 0) {
                    return std::string("网络时间未同步，暂时无法设置提醒");
                }
                
                std::string response;
                if (minutes > 0) {
                    response = "已设置" + std::to_string(minutes) + "分钟后";
                    if (seconds > 0) response += std::to_string(seconds) + "秒";
                    response += "的提醒：" + title;
                } else {
                    response = "已设置" + std::to_string(seconds) + "秒后的提醒：" + title;
                }
                return response;
            });

        mcp_server.AddTool("timer.set_alarm",
            "设置一个闹钟，在指定时间触发。参数：hour(小时0-23), minute(分钟0-59), title(闹钟名称), is_repeating(是否每天重复，默认false)",
            PropertyList({
                Property("hour", kPropertyTypeInteger, 0, 23),
                Property("minute", kPropertyTypeInteger, 0, 59),
                Property("title", kPropertyTypeString),
                Property("is_repeating", kPropertyTypeBoolean, false)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int hour = properties["hour"].value<int>();
                int minute = properties["minute"].value<int>();
                std::string title = properties["title"].value<std::string>();
                bool repeating = properties["is_repeating"].value<bool>();
                
                TimerService::GetInstance()->SetAbsoluteTimer(hour, minute, title, repeating);
                
                char time_buf[16];
                snprintf(time_buf, sizeof(time_buf), "%02d:%02d", hour, minute);
                std::string response = "已设置" + std::string(time_buf);
                if (repeating) response += "每天";
                response += "的闹钟：" + title;
                return response;
            });

        mcp_server.AddTool("timer.dismiss_alert",
            "关闭当前正在响的警惕音/闹钟（只关闭当前警报声和屏幕提醒，不删除其他未执行的提醒任务）。当用户说'关闭提醒'、'停止警报'、'关闭警惕音'、'关闭闹钟'等时调用此工具。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto* timer_service = TimerService::GetInstance();
                auto& app = Application::GetInstance();
                
                if (!timer_service->IsPlayingAlert()) {
                    return std::string("当前没有正在响的警惕音或闹钟");
                }
                
                // 停止警惕音播放（只停止当前警报，不清除其他待执行提醒）
                timer_service->DismissAlert();
                app.GetAudioService().ResetDecoder();
                app.AbortSpeaking(kAbortReasonNone);
                
                // 清除显示上的提醒覆盖层
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    display->SetStatus(Lang::Strings::STANDBY);
                    display->SetEmotion("neutral");
                    display->SetChatMessage("system", "");
                    display->HideAlarmOverlay();
                }
                
                return std::string("已关闭当前警惕音");
            });

        mcp_server.AddTool("timer.cancel_reminder",
            "取消一个还未触发的提醒或闹钟。当用户说'取消提醒'、'删除提醒'、'取消XX提醒'、'不要提醒我了'时调用此工具。"
            "参数：title(提醒标题，必填)，系统会匹配标题来取消对应的提醒。"
            "如果用户没有指定具体提醒名称，请先调用timer.get_pending获取列表，让用户选择要取消哪个。",
            PropertyList({
                Property("title", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                std::string title = properties["title"].value<std::string>();
                auto* timer_service = TimerService::GetInstance();
                
                // 如果当前有警报在响，不允许取消（应当先关闭警报）
                if (timer_service->IsPlayingAlert()) {
                    return std::string("当前有警报正在响，请先关闭警报后再取消提醒");
                }
                
                if (timer_service->CancelTimerByTitle(title)) {
                    return "已取消提醒：" + title;
                } else {
                    // 没有精确匹配，尝试获取列表让用户选择
                    auto timers = timer_service->GetActiveTimers();
                    if (timers.empty()) {
                        return std::string("当前没有待执行的提醒或闹钟");
                    }
                    return "未找到名为\"" + title + "\"的提醒。当前有" + std::to_string(timers.size()) + "个待执行提醒，请指定要取消的提醒名称";
                }
            });

        mcp_server.AddTool("timer.get_pending",
            "获取当前系统中所有待执行的提醒任务和闹钟列表，包括触发倒计时和是否重复。当用户问'现在有哪些提醒'、'还有什么提醒没执行'、'查看提醒'、'还有几个闹钟'时调用此工具",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto timers = TimerService::GetInstance()->GetActiveTimers();
                
                if (timers.empty()) {
                    return std::string("当前没有待执行的提醒或闹钟");
                }
                
                time_t now_epoch = time(nullptr);
                uint64_t now_ms = static_cast<uint64_t>(now_epoch) * 1000ULL;
                
                std::string response = "当前共有" + std::to_string(timers.size()) + "个待执行任务：";
                for (size_t i = 0; i < timers.size(); i++) {
                    const auto& t = timers[i];
                    
                    int64_t remaining_ms = static_cast<int64_t>(t.trigger_time_ms - now_ms);
                    if (remaining_ms < 0) remaining_ms = 0;
                    
                    int total_seconds = static_cast<int>(remaining_ms / 1000);
                    int hours = total_seconds / 3600;
                    int minutes = (total_seconds % 3600) / 60;
                    int seconds = total_seconds % 60;
                    
                    response += "\n" + std::to_string(i + 1) + ". " + t.title;
                    response += "（";
                    if (hours > 0) {
                        response += std::to_string(hours) + "小时" + 
                                   std::to_string(minutes) + "分后触发";
                    } else if (minutes > 0) {
                        response += std::to_string(minutes) + "分" + 
                                   std::to_string(seconds) + "秒后触发";
                    } else {
                        response += std::to_string(seconds) + "秒后触发";
                    }
                    if (t.is_repeating) {
                        response += "，每天重复";
                    }
                    response += "）";
                }
                return response;
            });

        mcp_server.AddTool("motor.forward",
            "让小车前进。参数：speed_percent(速度百分比0-100), duration_ms(持续时间毫秒)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 0, 10000)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                HandleMotorActionForApplication(4, speed, duration, 2);
                return "小车前进中";
            });

        mcp_server.AddTool("motor.backward",
            "让小车后退。参数：speed_percent(速度百分比0-100), duration_ms(持续时间毫秒)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 0, 10000)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                HandleMotorActionForApplication(2, speed, duration, 2);
                return "小车后退中";
            });

        mcp_server.AddTool("motor.turn_left",
            "让小车左转。参数：speed_percent(速度百分比0-100), duration_ms(持续时间毫秒)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 0, 10000)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                HandleMotorActionForApplication(3, speed, duration, 2);
                return "小车左转中";
            });

        mcp_server.AddTool("motor.turn_right",
            "让小车右转。参数：speed_percent(速度百分比0-100), duration_ms(持续时间毫秒)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 0, 10000)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                HandleMotorActionForApplication(1, speed, duration, 2);
                return "小车右转中";
            });

        mcp_server.AddTool("motor.dance",
            "让小车跳舞。参数：speed_percent(速度百分比0-100)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 0, 100)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                auto board = static_cast<AhuhuBoard*>(&Board::GetInstance());
                board->MotorDance(speed);
                return "小车跳舞中";
            });

        // 视觉自动跟随：用户说"跟紧我/离我近一点/别走"时开启，小车自动朝画面中的移动目标追去
        mcp_server.AddTool("follow.start",
            "开启视觉自动跟随。小车会利用摄像头识别到的移动目标方位，自动转向并靠近目标（适合孩子说'离我近一点/跟紧我'时）。参数无",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                g_follow_active = true;
                SendFollowControl(true);   // 通知发送板启动跟随决策
                ESP_LOGI(TAG, "Follow START requested by voice");
                return "好的，我已开启跟随，会朝你靠近";
            });

        mcp_server.AddTool("follow.stop",
            "关闭视觉自动跟随。取消追踪，停止自动靠近。适合孩子说'停/够了/别跟了'时。参数无",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                g_follow_active = false;
                SendFollowControl(false);  // 通知发送板停止跟随决策
                HandleMotorActionForApplication(0, 0, 0, 2);
                ESP_LOGI(TAG, "Follow STOP requested by voice");
                return "好的，已停止跟随";
            });

        mcp_server.AddTool("self.network.get_ip",
            "获取当前WiFi IP地址信息，用于语音播报或状态查询",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto& wifi = WifiManager::GetInstance();
                std::string ip = wifi.GetIpAddress();
                if (ip.empty()) {
                    return std::string("当前未连接到WiFi网络，无法获取IP地址");
                }
                return std::string("当前IP地址是") + ip;
            });

        // ── 大灯功能（控制 GPIO48 板载 WS2812，同一个 LED 双用途） ──
        // 默认状态：BOOT 对话时红/绿/蓝（由 OnStateChanged 管理）
        // 语音控制：打开（白光）、变色、关闭（临时覆盖状态颜色）
        mcp_server.AddTool("self.lamp.get_state",
            "获取大灯状态",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                auto& app = Application::GetInstance();
                bool on = (app.GetDeviceState() != kDeviceStateIdle);
                return on ? "{\"power\": true}" : "{\"power\": false}";
            });

        mcp_server.AddTool("self.lamp.set_color",
            "设置大灯颜色，参数：red(0-255), green(0-255), blue(0-255)",
            PropertyList({
                Property("red", kPropertyTypeInteger, 0, 255),
                Property("green", kPropertyTypeInteger, 0, 255),
                Property("blue", kPropertyTypeInteger, 0, 255)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int r = properties["red"].value<int>();
                int g = properties["green"].value<int>();
                int b = properties["blue"].value<int>();
                g_lamp_mode_active = true;
                auto* led = static_cast<CircularStrip*>(GetLed());
                led->SetAllColor({
                    static_cast<uint8_t>(r),
                    static_cast<uint8_t>(g),
                    static_cast<uint8_t>(b)
                });
                return "已将大灯颜色设置为RGB(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + ")";
            });

        mcp_server.AddTool("self.lamp.random_color",
            "将大灯随机切换到7种预设颜色之一（红/橙/黄/绿/青/蓝/紫），用户说'变颜色'时调用此工具",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                static const struct { const char* name; uint8_t r, g, b; } kColors[] = {
                    {"红色",   255,   0,   0},
                    {"橙色",   255, 165,   0},
                    {"黄色",   255, 255,   0},
                    {"绿色",     0, 255,   0},
                    {"青色",     0, 255, 255},
                    {"蓝色",     0,   0, 255},
                    {"紫色",   255,   0, 255},
                };
                int idx = esp_random() % 7;
                g_lamp_mode_active = true;
                auto* led = static_cast<CircularStrip*>(GetLed());
                led->SetAllColor({kColors[idx].r, kColors[idx].g, kColors[idx].b});
                return std::string("已将大灯切换为") + kColors[idx].name;
            });

        mcp_server.AddTool("self.lamp.marquee",
            "跑马灯模式：大灯自动在7种颜色中循环切换，持续30秒后自动停止。用户说'跑马灯'时调用此工具",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                g_lamp_mode_active = true;

                // 启动后台任务执行跑马灯 LED
                xTaskCreate([](void*) {
                    static const struct { uint8_t r, g, b; } kMarqueeColors[] = {
                        {255,   0,   0},  // 红
                        {255, 165,   0},  // 橙
                        {255, 255,   0},  // 黄
                        {  0, 255,   0},  // 绿
                        {  0, 255, 255},  // 青
                        {  0,   0, 255},  // 蓝
                        {255,   0, 255},  // 紫
                    };
                    
                    auto* led = static_cast<CircularStrip*>(Board::GetInstance().GetLed());
                    const int color_count = sizeof(kMarqueeColors) / sizeof(kMarqueeColors[0]);
                    const int delay_per_color = 200;       // 每色停留200ms，跑马灯节奏
                    const int total_steps = 30000 / delay_per_color;  // 30秒总步数
                    
                    for (int i = 0; i < total_steps; i++) {
                        if (!g_lamp_mode_active.load()) break;
                        
                        int idx = i % color_count;
                        led->SetAllColor({kMarqueeColors[idx].r, kMarqueeColors[idx].g, kMarqueeColors[idx].b});
                        vTaskDelay(pdMS_TO_TICKS(delay_per_color));
                    }
                    
                    // 跑马灯结束后自动关灯
                    g_lamp_mode_active.store(false);
                    led->SetAllColor({0, 0, 0});
                    led->OnStateChanged();
                    ESP_LOGI(TAG, "Marquee finished");
                    vTaskDelete(nullptr);
                }, "lamp_marquee", 2048, nullptr, 1, nullptr);

                // 启动跑马灯音效（kuaile.ogg，持续播放直到停止或 LED 任务结束）
                xTaskCreate([](void*) {
                    while (g_lamp_mode_active.load()) {
                        Application::GetInstance().PlaySound(Lang::Sounds::OGG_KUAILE);
                        if (!g_lamp_mode_active.load()) break;
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                    vTaskDelete(nullptr);
                }, "marquee_sound", 4096, nullptr, 1, nullptr);
                
                return "已开启跑马灯模式，大灯将在7种颜色中循环30秒";
            });

        // ── 音乐播放功能 ──
        auto music = GetMusic();
        if (music) {
            mcp_server.AddTool("self.music.play_song",
                "播放指定的歌曲。当用户要求播放音乐时使用此工具，会自动搜索歌曲并开始流式播放。\n"
                "参数:\n"
                "  `song_name`: 要播放的歌曲名称（必需）。\n"
                "  `artist_name`: 要播放的歌曲艺术家名称（可选，默认为空字符串）。\n"
                "返回:\n"
                "  播放状态信息，不需确认，立刻播放歌曲。",
                PropertyList({
                    Property("song_name", kPropertyTypeString),
                    Property("artist_name", kPropertyTypeString, "")
                }),
                [music](const PropertyList& properties) -> ReturnValue {
                    auto song_name = properties["song_name"].value<std::string>();
                    auto artist_name = properties["artist_name"].value<std::string>();
                    
                    if (!music->Download(song_name, artist_name)) {
                        ESP_LOGW(TAG, "All music servers failed, checking SD card");
                        DIR* sd_dir = opendir(SDCARD_MOUNT_POINT);
                        if (sd_dir != nullptr) {
                            closedir(sd_dir);
                            ESP_LOGW(TAG, "SD card present, falling back to SD card 儿歌");
                            music->EnterSdCardFolderMode("儿歌");
                            return "{\"success\": true, \"message\": \"在线歌曲播放失败，已切换到SD卡儿歌随机播放\"}";
                        }
                        ESP_LOGW(TAG, "No SD card, returning to standby");
                        auto* lcd = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
                        if (lcd != nullptr) {
                            Application::GetInstance().GetAudioService().SetMusicPlaying(false);
                            Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
                            lcd->SetMusicMode(false);
                            lcd->HideMusicInfo();   // 隐藏音乐界面，避免回退后残留遮挡待机/聊天界面
                            lcd->Resume();
                            lcd->ShowStandbyContent();
                            lcd->ShowStatusBar();
                        }
                        return "{\"success\": false, \"message\": \"在线歌曲播放失败，请稍后重试\"}";
                    }
                    return "{\"success\": true, \"message\": \"♪ " + song_name + " 开始播放\"}";
                });

            mcp_server.AddTool("self.music.stop",
                "停止当前正在播放的音乐。当用户说'停止播放'、'停音乐'、'别放了'等时调用此工具。"
                "注意：持续播放模式下不接受此指令，必须先通过'你好小智'唤醒后才能停止。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    // 持续播放模式下拒绝停止，只能由"你好小智"唤醒词或 BOOT 键停止
                    if (music->IsContinuousMode()) {
                        return "{\"success\": false, \"message\": \"持续播放中，请说'你好小智'后停止播放\"}";
                    }
                    if (music->IsDownloading()) {
                        music->StopStreaming();
                        return "{\"success\": true, \"message\": \"音乐已停止播放\"}";
                    }
                    return "{\"success\": false, \"message\": \"当前没有正在播放的音乐\"}";
                });

            mcp_server.AddTool("self.radio.play",
                "播放网络电台。当用户说'播放电台'、'打开电台'、'听电台'、'听广播'、'播放广播'等时使用此工具。\n"
                "参数：station_name=电台名称（可选，不指定则播放默认电台）。\n"
                "支持的电台：中国之声、杭州交通91.8、浙江之声、河南音乐广播、河南交通广播、河南新闻广播、江苏音乐广播、湖北之声、福建音乐广播、河北音乐广播、北京音乐广播、上海动感101、深圳音乐广播、深圳交通广播、深圳新闻广播、深圳生活广播。\n"
                "返回：操作结果。",
                PropertyList({
                    Property("station_name", kPropertyTypeString, "北京音乐广播")
                }),
                [music](const PropertyList& properties) -> ReturnValue {
                    std::string station = properties["station_name"].value<std::string>();
                    if (music->StartRadio(station)) {
                        return "{\"success\": true, \"message\": \"正在播放电台: " + station + "\"}";
                    }
                    return "{\"success\": false, \"message\": \"电台播放失败，请检查网络连接\"}";
                });

            mcp_server.AddTool("self.music.enter_music_mode",
                "进入音乐模式（纯音乐播放模式）。当用户说'音乐模式'、'进入音乐模式'、'开始音乐模式'等时使用此工具。\n"
                "音乐模式下：自动连续播放随机网络歌曲，不响应任何语音唤醒和打断，只有按BOOT键才能退出。\n"
                "参数：无。\n"
                "返回：操作结果。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    music->EnterMusicMode();
                    return "{\"success\": true, \"message\": \"已进入音乐模式，将连续播放随机网络歌曲，按BOOT键退出\"}";
                });

            mcp_server.AddTool("self.music.enter_local_mode",
                "进入本地音乐模式。当用户说'本地模式'、'播放本地歌曲'、'播放本地音乐'等时使用此工具。\n"
                "本地模式下：自动连续播放板载本地存储的歌曲，不响应任何语音唤醒和打断，只有按BOOT键才能退出。\n"
                "参数：无。\n"
                "返回：操作结果。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    music->EnterLocalMode();
                    return "{\"success\": true, \"message\": \"已进入本地音乐模式，将连续播放本地歌曲，按BOOT键退出\"}";
                });

            mcp_server.AddTool("self.music.dance_cat",
                "播放月薪猫跳舞动画和音乐。当用户说'播放猫咪'、'小猫咪'等时调用此工具。\n"
                "进入月薪猫模式：全屏显示猫咪跳舞动画，循环播放月薪猫音乐，不响应语音唤醒，按BOOT键退出。\n"
                "参数：无。\n"
                "返回：操作结果。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    music->EnterCatMode();
                    return "{\"success\": true, \"message\": \"月薪猫来啦！按BOOT键退出\"}";
                });

            mcp_server.AddTool("self.music.enter_sd_mode",
                "进入SD卡音乐模式。当用户说'SD卡模式'、'播放SD卡音乐'、'播放内存卡音乐'等时使用此工具。\n"
                "SD卡模式下：自动连续播放SD卡中的歌曲，不响应任何语音唤醒和打断，只有按BOOT键才能退出。\n"
                "参数：无。\n"
                "返回：操作结果。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    music->EnterSdCardMode();
                    return "{\"success\": true, \"message\": \"已进入SD卡音乐模式，将连续播放SD卡歌曲，按BOOT键退出\"}";
                });

            mcp_server.AddTool("self.music.enter_sd_folder",
                "进入SD卡文件夹播放模式（随机播放）。当用户说'播放儿歌'、'播放戏曲'、'播放故事'、'播放相声'、'播放国学'、'播放歌曲'、'播放音乐'等时使用此工具。\n"
                "支持的文件夹：音乐歌曲、国学、儿歌、相声、戏曲、故事、轻音乐、多多儿歌。\n"
                "参数：folder=文件夹名称。\n"
                "返回：操作结果。",
                PropertyList({Property("folder", kPropertyTypeString)}),
                [music](const PropertyList& properties) -> ReturnValue {
                    std::string folder = properties["folder"].value<std::string>();
                    music->EnterSdCardFolderMode(folder);
                    return "{\"success\": true, \"message\": \"已进入\" + folder + \"文件夹，随机播放，按BOOT键退出\"}";
                });

            mcp_server.AddTool("self.music.enter_sd_menu",
                "进入SD卡目录选择菜单。当用户说'SD卡目录'、'显示SD卡文件夹'、'选择SD卡文件夹'等时使用此工具。\n"
                "在屏幕上显示SD卡文件夹列表，用户可以用音量+/-键选择，按BOOT键确认播放。\n"
                "参数：无。\n"
                "返回：操作结果。",
                PropertyList(),
                [this](const PropertyList& properties) -> ReturnValue {
                    EnterSdCardMenuMode();
                    return "{\"success\": true, \"message\": \"已进入SD卡目录选择菜单，音量+/-选择，BOOT键确认\"}";
                });

            mcp_server.AddTool("self.music.enter_children_mode",
                "进入儿歌模式（网络搜索）。当用户说'儿歌模式'、'进入儿歌模式'等时使用此工具。\n"
                "儿歌模式下：自动连续播放随机儿歌（网络搜索），不响应任何语音唤醒和打断，只有按BOOT键才能退出。\n"
                "参数：无。\n"
                "返回：操作结果。",
                PropertyList(),
                [music](const PropertyList& properties) -> ReturnValue {
                    music->EnterChildrenMode();
                    return "{\"success\": true, \"message\": \"已进入儿歌模式，将连续播放儿歌，按BOOT键退出\"}";
                });
        }

        mcp_server.AddTool("self.tv.play_live",
            "播放直播电视。当用户说'看电视'、'看直播'、'播放电视'、'看中央台'、'看红牛'、'播放直播源'等时使用此工具。\n"
            "参数：channel=频道名称（可选，默认cctv1）。\n"
            "支持的频道：cctv1~cctv15=中央台，cctv17=农业农村，河南卫视，redbull=红牛TV，cctv5plus=CCTV5+，\n"
            "时尚一台/时尚二台，法国新闻一/二/三台，Aleph News，印尼新闻，\n"
            "中央国际英文/纪录/俄语/法语/西语/阿语，1983春晚，1987春晚。\n"
            "返回：操作结果。",
            PropertyList({
                Property("channel", kPropertyTypeString, "cctv1")
            }),
            [](const PropertyList& properties) -> ReturnValue {
                std::string channel = properties["channel"].value<std::string>();
                static std::map<std::string, std::string> channel_map = {
                    {"cctv1", "cctv1"}, {"中央一台", "cctv1"}, {"中央1台", "cctv1"},
                    {"cctv2", "cctv2"}, {"中央二台", "cctv2"}, {"中央2台", "cctv2"},
                    {"cctv3", "cctv3"}, {"中央三台", "cctv3"}, {"中央3台", "cctv3"},
                    {"cctv4", "cctv4"}, {"中央四台", "cctv4"}, {"中央4台", "cctv4"},
                    {"cctv5", "cctv5"}, {"中央五台", "cctv5"}, {"中央5台", "cctv5"},
                    {"cctv5plus", "cctv5plus"}, {"cctv5+", "cctv5plus"}, {"体育赛事", "cctv5plus"},
                    {"cctv6", "cctv6"}, {"中央六台", "cctv6"}, {"中央6台", "cctv6"},
                    {"cctv7", "cctv7"}, {"中央七台", "cctv7"}, {"中央7台", "cctv7"},
                    {"cctv8", "cctv8"}, {"中央八台", "cctv8"}, {"中央8台", "cctv8"},
                    {"cctv9", "cctv9"}, {"中央九台", "cctv9"}, {"中央9台", "cctv9"},
                    {"cctv10", "cctv10"}, {"中央十台", "cctv10"}, {"中央10台", "cctv10"},
                    {"cctv11", "cctv11"}, {"中央十一台", "cctv11"}, {"中央11台", "cctv11"},
                    {"cctv12", "cctv12"}, {"中央十二台", "cctv12"}, {"中央12台", "cctv12"},
                    {"cctv13", "cctv13"}, {"中央十三台", "cctv13"}, {"中央13台", "cctv13"},
                    {"cctv14", "cctv14"}, {"中央十四台", "cctv14"}, {"中央14台", "cctv14"}, {"少儿", "cctv14"},
                    {"cctv15", "cctv15"}, {"中央十五台", "cctv15"}, {"中央15台", "cctv15"},
                    {"cctv17", "cctv17"}, {"中央十七台", "cctv17"}, {"中央17台", "cctv17"}, {"农业农村", "cctv17"},
                    {"河南卫视", "henan_tv"}, {"河南台", "henan_tv"}, {"河南电视", "henan_tv"}, {"henan", "henan_tv"},
                    {"redbull", "redbull"}, {"红牛", "redbull"}, {"红牛TV", "redbull"},
                    {"斗鱼电影3y", "douyu_movie_3y"}, {"douyu_movie_3y", "douyu_movie_3y"},
                    {"时尚一台", "fashion1"}, {"时尚一", "fashion1"}, {"时尚1台", "fashion1"}, {"法国时尚", "fashion1"},
                    {"时尚二台", "fashion2"}, {"时尚二", "fashion2"}, {"时尚2台", "fashion2"},
                    {"法国新闻一台", "france_news1"}, {"法国新闻1台", "france_news1"}, {"法国新闻一", "france_news1"},
                    {"法国新闻二台", "france_news2"}, {"法国新闻2台", "france_news2"}, {"法国24", "france_news2"}, {"france24", "france_news2"},
                    {"法国新闻三台", "france_news3"}, {"法国新闻3台", "france_news3"}, {"法国新闻三", "france_news3"},
                    {"aleph", "aleph_news"}, {"aleph news", "aleph_news"},
                    {"印尼新闻", "cna_indo"}, {"cna", "cna_indo"}, {"cna印尼", "cna_indo"},
                    {"中央国际", "cgtn_en"}, {"中央国际英文", "cgtn_en"}, {"CGTN英文", "cgtn_en"}, {"cgtn英文", "cgtn_en"}, {"cgtn英语", "cgtn_en"},
                    {"中央国际纪录", "cgtn_doc"}, {"中央国际记录", "cgtn_doc"}, {"CGTN纪录", "cgtn_doc"}, {"cgtn纪录", "cgtn_doc"}, {"cgtn记录", "cgtn_doc"},
                    {"中央国际俄语", "cgtn_ru"}, {"CGTN俄语", "cgtn_ru"}, {"cgtn俄语", "cgtn_ru"},
                    {"中央国际法语", "cgtn_fr"}, {"CGTN法语", "cgtn_fr"}, {"cgtn法语", "cgtn_fr"},
                    {"中央国际西语", "cgtn_es"}, {"中央国际西班牙语", "cgtn_es"}, {"CGTN西语", "cgtn_es"}, {"cgtn西语", "cgtn_es"}, {"CGTN西班牙语", "cgtn_es"}, {"cgtn西班牙语", "cgtn_es"},
                    {"中央国际阿语", "cgtn_ar"}, {"中央国际阿拉伯语", "cgtn_ar"}, {"CGTN阿语", "cgtn_ar"}, {"cgtn阿语", "cgtn_ar"}, {"CGTN阿拉伯语", "cgtn_ar"}, {"cgtn阿拉伯语", "cgtn_ar"},
                    {"1983年春晚", "chunwan_1983"}, {"1983春晚", "chunwan_1983"}, {"83春晚", "chunwan_1983"},
                    {"1987年春晚", "chunwan_1987"}, {"1987春晚", "chunwan_1987"}, {"87春晚", "chunwan_1987"},
                };
                auto it = channel_map.find(channel);
                std::string channel_key = (it != channel_map.end()) ? it->second : "cctv1";
                std::string server_ip = DiscoverControlServer();
                char ctrl_url[128];
                snprintf(ctrl_url, sizeof(ctrl_url), "http://%s:8090/start?channel=%s", server_ip.c_str(), channel_key.c_str());
                esp_http_client_config_t ctrl_cfg = {};
                ctrl_cfg.url = ctrl_url;
                ctrl_cfg.timeout_ms = 15000;
                esp_http_client_handle_t ctrl = esp_http_client_init(&ctrl_cfg);
                if (ctrl) {
                    esp_http_client_perform(ctrl);
                    esp_http_client_cleanup(ctrl);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                char url[64];
                snprintf(url, sizeof(url), "http://%s:8081/stream", server_ip.c_str());
                bool connected = false;
                for (int retry = 0; retry < 5; retry++) {
                    if (GetMjpegPlayer().Play(url, true, true)) {
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        if (GetMjpegPlayer().IsPlaying()) {
                            connected = true;
                            break;
                        }
                    }
                    ESP_LOGW(TAG, "MJPEG connect retry %d/5", retry + 1);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                if (!connected) {
                    char stop_url[64];
                    snprintf(stop_url, sizeof(stop_url), "http://%s:8090/stop", server_ip.c_str());
                    esp_http_client_config_t stop_cfg2 = {};
                    stop_cfg2.url = stop_url;
                    stop_cfg2.timeout_ms = 5000;
                    esp_http_client_handle_t stop_cli2 = esp_http_client_init(&stop_cfg2);
                    if (stop_cli2) {
                        esp_http_client_perform(stop_cli2);
                        esp_http_client_cleanup(stop_cli2);
                    }
                    return "{\"success\": false, \"message\": \"直播播放失败，请检查服务器是否启动\"}";
                }
                SetVideoQuietMode(true);
                return "{\"success\": true, \"message\": \"正在播放直播电视: " + channel + "\"}";
            });

        mcp_server.AddTool("self.tv.stop",
            "停止播放直播电视。当用户说'关电视'、'停止直播'、'关闭电视'、'不看了'等时使用此工具。\n"
            "参数：无。\n"
            "返回：操作结果。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                std::string server_ip = DiscoverControlServer();
                char stop_url[64];
                snprintf(stop_url, sizeof(stop_url), "http://%s:8090/stop", server_ip.c_str());
                esp_http_client_config_t stop_cfg = {};
                stop_cfg.url = stop_url;
                stop_cfg.timeout_ms = 5000;
                esp_http_client_handle_t stop_cli = esp_http_client_init(&stop_cfg);
                if (stop_cli) {
                    esp_http_client_perform(stop_cli);
                    esp_http_client_cleanup(stop_cli);
                }
                if (GetMjpegPlayer().IsPlaying()) {
                    GetMjpegPlayer().Stop();
                    SetVideoQuietMode(false);
                    return "{\"success\": true, \"message\": \"已停止播放直播电视\"}";
                }
                return "{\"success\": true, \"message\": \"当前没有在播放直播\"}";
            });

        mcp_server.AddTool("camera.set_preview",
            "打开或关闭摄像头预览。参数：enable(true=打开, false=关闭)。当用户说'打开摄像头'、'摄像头预览'、'关闭摄像头'、'停止预览'等时使用此工具。",
            PropertyList({
                Property("enable", kPropertyTypeBoolean, true)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                bool enable = properties["enable"].value<bool>();
#ifndef DISPLAY_TYPE_ST7735S
                if (enable) {
                    if (camera_preview_active_) {
                        return "摄像头预览已经在运行中";
                    }
                    StartCameraPreview();
                    if (!camera_) {
                        return "摄像头未检测到，请检查摄像头连接";
                    }
                    return "摄像头预览已开启";
                } else {
                    if (!camera_preview_active_) {
                        return "摄像头预览未在运行";
                    }
                    StopCameraPreview();
                    return "摄像头预览已关闭";
                }
#else
                if (enable) {
                    if (GetMjpegPlayer().IsPlaying()) {
                        return "{\"success\": true, \"message\": \"摄像头画面已在显示中\"}";
                    }
                    std::string cam_ip = DiscoverCamera();
                    if (cam_ip.empty()) {
                        return "{\"success\": false, \"message\": \"未找到摄像头，请确认摄像头已联网且与本机在同一局域网\"}";
                    }
                    char url[64];
                    snprintf(url, sizeof(url), "http://%s/camera", cam_ip.c_str());
                    bool ok = false;
                    for (int retry = 0; retry < 5; retry++) {
                        if (GetMjpegPlayer().Play(url, false, false)) {
                            vTaskDelay(pdMS_TO_TICKS(3000));
                            if (GetMjpegPlayer().IsPlaying()) {
                                ok = true;
                                break;
                            }
                        }
                        ESP_LOGW(TAG, "Camera MJPEG connect retry %d/5", retry + 1);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                    if (!ok) {
                        return "{\"success\": false, \"message\": \"连接摄像头画面失败，请检查摄像头是否在线\"}";
                    }
                    return "{\"success\": true, \"message\": \"正在显示摄像头实时画面: " + cam_ip + "\"}";
                } else {
                    if (GetMjpegPlayer().IsPlaying()) {
                        GetMjpegPlayer().Stop();
                        return "{\"success\": true, \"message\": \"已停止显示摄像头画面\"}";
                    }
                    return "{\"success\": true, \"message\": \"当前没有在显示摄像头画面\"}";
                }
#endif
            });

        mcp_server.AddTool("camera.view",
            "在显示屏上显示局域网内 AASXT 摄像头板的实时画面。当用户说'看摄像头'、'打开监控'、'显示监控画面'、'看监控'时使用此工具。参数：无。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                std::string cam_ip = DiscoverCamera();
                if (cam_ip.empty()) {
                    return "{\"success\": false, \"message\": \"未找到摄像头，请确认摄像头已联网且与本机在同一局域网\"}";
                }
                char url[64];
                snprintf(url, sizeof(url), "http://%s/camera", cam_ip.c_str());
                bool ok = false;
                for (int retry = 0; retry < 5; retry++) {
                    if (GetMjpegPlayer().Play(url, false, false)) {
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        if (GetMjpegPlayer().IsPlaying()) {
                            ok = true;
                            break;
                        }
                    }
                    ESP_LOGW(TAG, "Camera MJPEG connect retry %d/5", retry + 1);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                if (!ok) {
                    return "{\"success\": false, \"message\": \"连接摄像头画面失败，请检查摄像头是否在线\"}";
                }
                return "{\"success\": true, \"message\": \"已显示摄像头实时画面: " + cam_ip + "\"}";
            });

        mcp_server.AddTool("camera.stop",
            "停止显示摄像头画面。当用户说'关闭监控'、'关掉画面'、'不看摄像头'时使用此工具。参数：无。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                if (GetMjpegPlayer().IsPlaying()) {
                    GetMjpegPlayer().Stop();
                    return "{\"success\": true, \"message\": \"已停止显示摄像头画面\"}";
                }
                return "{\"success\": true, \"message\": \"当前没有在显示摄像头画面\"}";
            });

        mcp_server.AddTool("camera.capture",
            "拍摄一张照片并在屏幕上显示 5 秒后自动返回对话界面。当用户说'拍照'、'拍一张'、'拍个照'、'给我拍照'时使用此工具。参数：无。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                std::string cam_ip = DiscoverCamera();
                if (cam_ip.empty()) {
                    return "{\"success\": false, \"message\": \"未找到摄像头，请确认摄像头已联网且与本机在同一局域网\"}";
                }
                char url[64];
                snprintf(url, sizeof(url), "http://%s/capture", cam_ip.c_str());

                std::vector<uint8_t> jpeg_data;
                if (!HttpGetToMemory(url, jpeg_data)) {
                    return "{\"success\": false, \"message\": \"拍照失败，无法获取摄像头画面\"}";
                }

                uint8_t* rgb = nullptr;
                size_t rgb_len = 0, w = 0, h = 0, stride = 0;
                if (jpeg_to_image(jpeg_data.data(), jpeg_data.size(), &rgb, &rgb_len, &w, &h, &stride) != ESP_OK || rgb == nullptr) {
                    return "{\"success\": false, \"message\": \"照片解码失败\"}";
                }

                auto* lcd = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
                if (lcd == nullptr) {
                    heap_caps_free(rgb);
                    return "{\"success\": false, \"message\": \"显示屏不可用\"}";
                }
                lcd->SetPreviewImage(std::make_unique<LvglAllocatedImage>(rgb, rgb_len, (int)w, (int)h, (int)stride, LV_COLOR_FORMAT_RGB565));
                return "{\"success\": true, \"message\": \"拍照成功，照片显示 5 秒后自动关闭\"}";
            });

        mcp_server.AddTool("camera.describe",
            "拍摄一张照片并用视觉大模型理解画面内容。当用户问'照片里是什么'、'你看到了什么'、'前面有什么'、'描述一下画面'、'看看有什么'、'这是什么'时使用此工具。参数：无。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                std::string cam_ip = DiscoverCamera();
                if (cam_ip.empty()) {
                    return "{\"success\": false, \"message\": \"未找到摄像头，请确认摄像头已联网且与本机在同一局域网\"}";
                }
                char url[64];
                snprintf(url, sizeof(url), "http://%s/capture", cam_ip.c_str());

                std::vector<uint8_t> jpeg_data;
                if (!HttpGetToMemory(url, jpeg_data)) {
                    return "{\"success\": false, \"message\": \"拍照失败，无法获取摄像头画面\"}";
                }

                return AskZhipu(jpeg_data, "");
            });
    }

    void PerformMotorAction(int direction, int duration_ms = 300) {
        HandleMotorActionForApplication(direction, 100, duration_ms, 2);
    }

    void InitializeSdCard() {
        ESP_LOGI(TAG, "Initializing SD card (SPI)...");

        gpio_set_pull_mode(SDCARD_SPI_MISO, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(SDCARD_SPI_MOSI, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(SDCARD_SPI_CS, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(SDCARD_SPI_SCLK, GPIO_PULLUP_ONLY);

        vTaskDelay(pdMS_TO_TICKS(100));

        spi_bus_config_t bus_cfg = {
            .mosi_io_num = SDCARD_SPI_MOSI,
            .miso_io_num = SDCARD_SPI_MISO,
            .sclk_io_num = SDCARD_SPI_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };

        esp_err_t ret = spi_bus_initialize(SDCARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD card SPI bus init failed: %s", esp_err_to_name(ret));
            return;
        }

        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = SDCARD_SPI_CS;
        slot_config.host_id = SDCARD_SPI_HOST;

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 0,
            .disk_status_check_enable = true,
        };

        int freq_list[] = {100, SDMMC_FREQ_PROBING, 10000, SDMMC_FREQ_DEFAULT};
        sdmmc_card_t* card = nullptr;

        for (int attempt = 0; attempt < 4; attempt++) {
            sdmmc_host_t host = SDSPI_HOST_DEFAULT();
            host.slot = SDCARD_SPI_HOST;
            host.max_freq_khz = freq_list[attempt];

            ESP_LOGI(TAG, "SD card attempt %d at %d kHz", attempt + 1, freq_list[attempt]);

            ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
            if (ret == ESP_OK) {
                sdmmc_card_print_info(stdout, card);
                ESP_LOGI(TAG, "SD card mounted at %s", SDCARD_MOUNT_POINT);
                return;
            }

            ESP_LOGW(TAG, "SD card attempt %d failed: %s", attempt + 1, esp_err_to_name(ret));
            if (attempt < 3) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }

        ESP_LOGW(TAG, "SD card mount failed after 4 attempts");
        spi_bus_free(SDCARD_SPI_HOST);
    }

public:
    void OnIdle() {
        HandleMotorActionForApplication(0, 0, 0, 2);
    }

    // 非阻塞：仅设置电机方向（WEB 遥控按住持续走，direction=0 停止）
    void SetMotorDirection(int direction) {
        SetMotorDirectionGpio(direction);
    }

private:
    void MotorForward(int duration_ms = 500, uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(4, speed_percent, duration_ms, 2);
    }

    void MotorBackward(int duration_ms = 500, uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(2, speed_percent, duration_ms, 2);
    }

    void MotorTurnLeft(int duration_ms = 300, uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(3, speed_percent, duration_ms, 2);
    }

    void MotorTurnRight(int duration_ms = 300, uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(1, speed_percent, duration_ms, 2);
    }

    void MotorTurnLeftDuration(int duration_ms = 600, uint8_t speed_percent = 100) {
        MotorTurnLeft(duration_ms, speed_percent);
    }

    void MotorTurnRightDuration(int duration_ms = 600, uint8_t speed_percent = 100) {
        MotorTurnRight(duration_ms, speed_percent);
    }

    void MotorStop() {
        HandleMotorActionForApplication(0, 0, 0, 2);
    }

    void MotorSpinAround(uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(3, speed_percent, 2000, 2);
    }

    void MotorQuickForward(uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(4, speed_percent, 500, 2);
    }

    void MotorQuickBackward(uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(2, speed_percent, 500, 2);
    }

    void MotorWiggle(uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(1, speed_percent, 300, 2);
    }

public:
    void MotorDance(uint8_t speed_percent = 100) {
        ESP_LOGI(TAG, "Motor dance: playing music and dancing (speed: %d%%)", speed_percent);

        static std::atomic<bool> dance_running{false};
        static uint8_t dance_speed = 100;

        if (dance_running) {
            ESP_LOGW(TAG, "Motor dance already running, ignoring");
            return;
        }

        dance_speed = speed_percent;
        dance_running = true;

        xTaskCreate([](void* arg) {
            while (dance_running) {
                HandleMotorActionForApplication(4, dance_speed, 300, 2);
                vTaskDelay(pdMS_TO_TICKS(350));

                HandleMotorActionForApplication(3, dance_speed, 250, 2);
                vTaskDelay(pdMS_TO_TICKS(300));

                HandleMotorActionForApplication(1, dance_speed, 250, 2);
                vTaskDelay(pdMS_TO_TICKS(300));

                HandleMotorActionForApplication(2, dance_speed, 300, 2);
                vTaskDelay(pdMS_TO_TICKS(350));

                HandleMotorActionForApplication(4, dance_speed, 200, 2);
                vTaskDelay(pdMS_TO_TICKS(250));

                HandleMotorActionForApplication(3, dance_speed, 200, 2);
                vTaskDelay(pdMS_TO_TICKS(250));

                HandleMotorActionForApplication(1, dance_speed, 200, 2);
                vTaskDelay(pdMS_TO_TICKS(250));

                HandleMotorActionForApplication(4, dance_speed, 400, 2);
            }

            HandleMotorActionForApplication(0, 0, 0, 2);
            ESP_LOGI(TAG, "Motor dance: finished");
            vTaskDelete(nullptr);
        }, "motor_dance", 4096, nullptr, 5, nullptr);

        Application::GetInstance().PlaySound(Lang::Sounds::OGG_KUAILE);

        dance_running = false;
        ESP_LOGI(TAG, "Motor dance: song ended, stopping dance");
    }

    

void InitializeScreenOffTimer() {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto self = static_cast<AhuhuBoard*>(arg);
                auto& app = Application::GetInstance();
                if (!self->clock_mode_ && !app.CanEnterSleepMode()) {
                    self->clock_mode_ticks_ = 0;
                    return;
                }
                self->clock_mode_ticks_++;
                if (self->clock_mode_ticks_ >= kScreenOffTimeoutSec && !self->clock_mode_) {
                    ESP_LOGI(TAG, "Entering clock mode (%ds idle)", kScreenOffTimeoutSec);
                    self->clock_mode_ = true;
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "screen_off",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &screen_off_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(screen_off_timer_, 1000000));
        ESP_LOGI(TAG, "Clock mode timer started (%ds timeout)", kScreenOffTimeoutSec);
    }

    void WakeUp() override {
        ESP_LOGI(TAG, "WakeUp: exiting clock mode");
        clock_mode_ticks_ = 0;
        if (clock_mode_) {
            clock_mode_ = false;
            if (display_) {
                display_->HideClockContent();
                display_->HideQuotesContent();
            }
        }
    }

public:
    AhuhuBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        ESP_LOGI(TAG, "AhuhuBoard constructor starting...");
#ifdef CONFIG_WEATHER_API_TOKEN
        WeatherService::GetInstance()->Init(CONFIG_WEATHER_API_TOKEN, 33.569578, 114.075365);
#else
        WeatherService::GetInstance()->Init("3182d4fb9c5f1cf12b8d744303967b68", 33.569578, 114.075365);
#endif
        
        ESP_LOGI(TAG, "Weather service initialized, enabled=%d", WeatherService::GetInstance()->IsTokenValid());
        
        if (WeatherService::GetInstance()->IsTokenValid()) {
            xTaskCreatePinnedToCore(WeatherInitTask, "WeatherInit", 8192, this, 5, nullptr, 1);
        }
        InitializeButtons();
        InitializeTools();
        InitializeSdCard();
#ifdef DISPLAY_TYPE_ST7735S
        InitializeSpi();
        InitializeSt7735Display();
        // 开启背光
        if (auto backlight = GetBacklight()) {
            backlight->RestoreBrightness();
            ESP_LOGI(TAG, "Backlight enabled");
        }
        InitializeScreenOffTimer();
#else
        // 摄像头在用户双击音量+时按需初始化，避免启动时卡死
#endif
        
        // 只创建WebServer，不启动（等StartNetwork中WiFi初始化后再启动）
        web_server_ = new WebServer();
        // 注册电机配置回调（Ahuhu无电机，用默认值避免API 500错误）
        web_server_->SetMotorActionConfigCallback(
            []() -> WebServer::MotorActionConfig {
                return WebServer::MotorActionConfig();  // 全默认值
            },
            [](const WebServer::MotorActionConfig&) {
                // 无电机，忽略保存
            }
        );
        web_server_->SetMusicServerUrlCallback(
            []() -> std::string {
                Settings nvs("music", false);
                return nvs.GetString("server_url_0", "");
            },
            [](const std::string& url) {
                Settings nvs("music", true);
                nvs.SetString("server_url_0", url);
                ESP_LOGI(TAG, "Music server URL saved to NVS, reboot to take effect: %s", url.c_str());
            }
        );
        // 待机背景图上传：收到 PNG 后交由显示层持久化并刷新
        web_server_->SetStandbyBackgroundCallback([this](const uint8_t* data, size_t size) -> bool {
            auto* display = GetDisplay();
            if (display == nullptr) {
                return false;
            }
            display->SetStandbyBackgroundImage(data, size);
            return true;
        });
        // 小车行走控制：direction=0 停止，1=右转 2=后退 3=左转 4=前进（WEB 按住持续走/松手即停）
        web_server_->SetMotorMoveCallback([this](int direction, int duration_ms) {
            (void)duration_ms;
            SetMotorDirection(direction);
        });
        web_server_->SetCameraStreamCallback([this](uint8_t** jpeg_data, size_t* jpeg_len) -> bool {
#ifdef DISPLAY_TYPE_ST7735S
            return false;
#else
            static int call_count = 0;
            call_count++;
            if (!camera_) {
                ESP_LOGI(TAG, "CamCB: init camera...");
                InitializeCamera();
            }
            if (!camera_) { ESP_LOGW(TAG, "CamCB: no camera"); return false; }
            if (!camera_->Capture()) { ESP_LOGW(TAG, "CamCB: capture failed"); return false; }
            auto& frame = camera_->GetFrame();
            if (!frame.data || frame.len == 0) { ESP_LOGW(TAG, "CamCB: frame empty"); return false; }
            if (call_count <= 3) {
                ESP_LOGI(TAG, "CamCB: frame #%d fmt=%d %dx%d len=%zu", call_count, frame.format, frame.width, frame.height, frame.len);
            }
            bool ok = image_to_jpeg(frame.data, frame.len, frame.width, frame.height,
                                    frame.format, 70, jpeg_data, jpeg_len);
            if (!ok) { ESP_LOGW(TAG, "CamCB: jpeg encode failed, fmt=%d %dx%d len=%zu", frame.format, frame.width, frame.height, frame.len); }
            if (call_count <= 3) {
                ESP_LOGI(TAG, "CamCB: jpeg result=%d len=%zu", ok, ok ? *jpeg_len : 0);
            }
            return ok;
#endif
        });
        
        ESP_LOGI(TAG, "AhuhuBoard constructor done");
    }

    void StartNetwork() override {
        // 先初始化WiFi和LwIP协议栈
        WifiBoard::StartNetwork();
        // 等LwIP就绪后再启动HTTP服务
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (web_server_) {
            web_server_->Start();
            ESP_LOGI(TAG, "Web server started after WiFi init");
        }

        // CAMERA AUTO-TEST disabled: camera started manually via button/voice

        // RADIO AUTO-TEST: 启动后15秒自动播放中国之声测试HLS流
        #define RADIO_AUTO_TEST 0
        #if RADIO_AUTO_TEST
        esp_timer_handle_t radio_test_timer = nullptr;
        esp_timer_create_args_t radio_test_args = {};
        radio_test_args.callback = [](void* arg) {
            auto* board = static_cast<AhuhuBoard*>(arg);
            auto music = board->GetMusic();
            if (music) {
                ESP_LOGI(TAG, "=== RADIO AUTO-TEST: Playing 中国之声 ===");
                music->StartRadio("中国之声");
            }
        };
        radio_test_args.arg = this;
        radio_test_args.dispatch_method = ESP_TIMER_TASK;
        radio_test_args.name = "radio_test";
        ESP_ERROR_CHECK(esp_timer_create(&radio_test_args, &radio_test_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(radio_test_timer, 15000000));  // 15秒后触发
        ESP_LOGI(TAG, "RADIO AUTO-TEST: Will play 中国之声 in 15 seconds");
        #endif

        // VIDEO AUTO-TEST: 启动后10秒自动播放视频
        #define VIDEO_AUTO_TEST 0
        #if VIDEO_AUTO_TEST
        esp_timer_handle_t video_test_timer = nullptr;
        esp_timer_create_args_t video_test_args = {};
        video_test_args.callback = [](void* arg) {
            ESP_LOGI(TAG, "=== VIDEO AUTO-TEST: Playing test_orig.avi ===");
            static AviPlayer player;
            player.Play("http://192.168.1.66:8080/test_orig.avi");
        };
        video_test_args.arg = this;
        video_test_args.dispatch_method = ESP_TIMER_TASK;
        video_test_args.name = "video_test";
        ESP_ERROR_CHECK(esp_timer_create(&video_test_args, &video_test_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(video_test_timer, 10000000));  // 10秒后触发
        ESP_LOGI(TAG, "VIDEO AUTO-TEST: Will play test.avi in 10 seconds");
        #endif

        // MJPEG LIVE STREAMING: controlled by voice via MCP tool
        #define MJPEG_AUTO_TEST 0
        #if MJPEG_AUTO_TEST
        esp_timer_handle_t mjpeg_test_timer = nullptr;
        esp_timer_create_args_t mjpeg_test_args = {};
        mjpeg_test_args.callback = [](void* arg) {
            ESP_LOGI(TAG, "=== MJPEG AUTO-TEST: Streaming translated live TV ===");
            GetMjpegPlayer().Play("http://192.168.1.66:8083");
        };
        mjpeg_test_args.arg = this;
        mjpeg_test_args.dispatch_method = ESP_TIMER_TASK;
        mjpeg_test_args.name = "mjpeg_test";
        ESP_ERROR_CHECK(esp_timer_create(&mjpeg_test_args, &mjpeg_test_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(mjpeg_test_timer, 25000000));  // 25秒后触发，等板子稳定
        ESP_LOGI(TAG, "MJPEG AUTO-TEST: Will stream translated live TV in 25 seconds");
        #endif

        // 视觉跟随服务：接收 AASXT 上报并控制电机自动追随（语音 follow.start/stop 开关）
        StartFollowService();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Led* GetLed() override {
        static LampAwareCircularStrip led(BUILTIN_LED_GPIO, 1);
        return &led;
    }

    virtual Display* GetDisplay() override {
#ifdef DISPLAY_TYPE_ST7735S
        return display_;
#else
        if (!display_) {
            ESP_LOGI(TAG, "Initializing display on first GetDisplay() call...");
            InitializeDisplayI2c();
            InitializeSsd1306Display();
        }
        return display_;
#endif
    }

    ~AhuhuBoard() {
#ifndef DISPLAY_TYPE_ST7735S
        StopCameraPreview();
        if (camera_) {
            delete camera_;
            camera_ = nullptr;
        }
        g_camera_xclk_active = false;
#endif
        if (web_server_) {
            delete web_server_;
            web_server_ = nullptr;
        }
        
        if (display_ && display_ != &no_display_) {
            delete display_;
            display_ = nullptr;
        }
        
#ifndef DISPLAY_TYPE_ST7735S
        if (panel_io_) {
            esp_lcd_panel_io_del(panel_io_);
            panel_io_ = nullptr;
        }
        
        if (panel_) {
            esp_lcd_panel_del(panel_);
            panel_ = nullptr;
        }
        
        if (display_i2c_bus_) {
            i2c_del_master_bus(display_i2c_bus_);
            display_i2c_bus_ = nullptr;
        }
#endif
    }

#ifdef DISPLAY_TYPE_ST7735S
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
#endif

private:
    static NoDisplay no_display_;
};





// 共用：仅按方向设置电机 GPIO（direction=0 停止）
static void SetMotorDirectionGpio(int direction) {
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << MOTOR_LF_GPIO) | (1ULL << MOTOR_LB_GPIO) |
                           (1ULL << MOTOR_RF_GPIO) | (1ULL << MOTOR_RB_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    switch (direction) {
    case 0:
        gpio_set_level(MOTOR_LF_GPIO, 0);
        gpio_set_level(MOTOR_LB_GPIO, 0);
        gpio_set_level(MOTOR_RF_GPIO, 0);
        gpio_set_level(MOTOR_RB_GPIO, 0);
        break;
    case 1:
        gpio_set_level(MOTOR_LF_GPIO, 1);
        gpio_set_level(MOTOR_LB_GPIO, 0);
        gpio_set_level(MOTOR_RF_GPIO, 0);
        gpio_set_level(MOTOR_RB_GPIO, 1);
        break;
    case 2:
        gpio_set_level(MOTOR_LF_GPIO, 0);
        gpio_set_level(MOTOR_LB_GPIO, 1);
        gpio_set_level(MOTOR_RF_GPIO, 0);
        gpio_set_level(MOTOR_RB_GPIO, 1);
        break;
    case 3:
        gpio_set_level(MOTOR_LF_GPIO, 0);
        gpio_set_level(MOTOR_LB_GPIO, 1);
        gpio_set_level(MOTOR_RF_GPIO, 1);
        gpio_set_level(MOTOR_RB_GPIO, 0);
        break;
    case 4:
        gpio_set_level(MOTOR_LF_GPIO, 1);
        gpio_set_level(MOTOR_LB_GPIO, 0);
        gpio_set_level(MOTOR_RF_GPIO, 1);
        gpio_set_level(MOTOR_RB_GPIO, 0);
        break;
    default:
        gpio_set_level(MOTOR_LF_GPIO, 0);
        gpio_set_level(MOTOR_LB_GPIO, 0);
        gpio_set_level(MOTOR_RF_GPIO, 0);
        gpio_set_level(MOTOR_RB_GPIO, 0);
        break;
    }
}

extern "C" void HandleMotorActionForApplication(int direction, int speed, int duration_ms, int priority) {
    (void)priority;

    SetMotorDirectionGpio(direction);

    if (direction != 0 && duration_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        SetMotorDirectionGpio(0);
    }
}

extern "C" void HandleMotorActionForEmotion(const char* emotion) {
    // 空实现 - ahuhu板子不支持情绪动画电机动作
    ESP_LOGD("AhuhuBoard", "HandleMotorActionForEmotion called with: %s (not supported on this board)", 
             emotion ? emotion : "null");
}

extern "C" void HandleMotorActionForDance(uint8_t speed_percent) {
    auto board = static_cast<AhuhuBoard*>(&Board::GetInstance());
    board->MotorDance(speed_percent);
}

NoDisplay AhuhuBoard::no_display_;

DECLARE_BOARD(AhuhuBoard);