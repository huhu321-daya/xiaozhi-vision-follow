# AAS3-CAM 摄像头监控节点（无屏幕）

AAS3-CAM 是一个**独立的摄像头监控板（无屏幕）**，作为 xiaozhi 工程里的一个独立板型
构建。它脱离 xiaozhi 的 `Application` 大框架，参考 EYE（`esp32-eye-main/firmware/cam`）
的干净架构实现：**自包含、只需摄像头 + WiFi + Web 视频流 + 与 AHUHU 小车握手/跟随**。

## 硬件

- **主控**: ESP32-S3
- **摄像头**: OV3560（DVP 24PIN FPC，VGA 640×480，JPEG 片上压缩），驱动沿用 AASXT
  现有 esp32-camera（自动识别型号），**驱动的引脚/代码与 AASXT 完全一致**
- **屏幕**: 无
- **音频**: 无（音频引脚被摄像头占用，全部 GPIO_NUM_NC 禁用）
- **SD 卡**: 硬件存在，但本节点不实现录像（按需保留宏）
- **补光 LED**: WS2812（GPIO48），本节点暂不启用

## 功能清单（只保留这 6 项）

1. **连接 WiFi 通信**
   - 采用与 EYE 相同的方式：上电直连指定 WiFi（SSID/密码写在 `config.h`）或复用
     xiaozhi 已配网（NVS）的凭据，二者之一。
2. **Web 端看视频流**
   - 用 `esp_http_server` 自建 HTTP 服务（端口 80），完全不用 xiaozhi 的 WebServer 类：
     - `GET /stream` — MJPEG 实时流（`multipart/x-mixed-replace`），浏览器直接看
     - `GET /capture` — 抓拍最新一帧 JPEG
     - `GET /healthz` — 状态 JSON（uptime / RSSI / 堆内存 / 摄像头帧率等）
3. **与 AHUHU 握手**
   - UDP `8092` 监听：收到 AHUHU 广播的 `XIAOZHI_CAM_DISCOVER` 后，回显本机 IP，
     让小车能找到摄像头板。
4. **传递数据（视觉跟随）**
   - UDP `8093` 广播：对画面做 32×18 亮度网格的**帧间差分**，计算动目标的
     存在/左右分区(zone L/C/R)/距离等级，按 EYE 风格每 ~200ms 发一条
     `FOLLOW <has> <zone> <dist>`，AHUHU 据此驱动小车跟随。
5. **屏幕画面显示**
   - 本板**无屏幕**，此功能不适用（删除）。如需调试画面走 `/stream`。
6. **删除无关功能**
   - 已删除：xiaozhi Application/MQTT/配网 UI/音频/ST7735 屏显/OLED/录像/运动检测/
     WS2812 补光等。

## 摄像头模型（参考 EYE）

- 独立采集任务：`esp_camera_fb_get()` 持续抓最新 JPEG 帧 → 拷贝到 **PSRAM 共享槽**
  （`s_latest`），`camCopyLatest()` 供 HTTP handler 在**短临界区**内拷贝最新帧，发送
  不持任何摄像头锁 → HTTP 线程与采集线程互不阻塞。
- 看门狗：`fb_get` 连续 N 次失败 → deinit + re-init 摄像头；多次失败仍不行 → 重启。
- 摄像头在 **core 1**（PSRAM 友好），网络任务在 core 0，避免互相拖慢。

## 目录结构（独立，全部放这里）

```
main/boards/AAS3-CAM/
  config.h            # 硬件引脚（复用 AASXT 的 OV3560 引脚表）+ 无屏/音频禁用
  aas3_cam_board.cc   # 板核心实现（可拆分为 camera/http_server/wifi 模块）
  README.md           # 本文件（功能说明 / 参考）
```

## 与 AASXT / AHUHU 的关系

- **AAS3-CAM**：纯监控板固件，烧录到 COM4。
- **AHUHU**：小车/主控，烧录到 COM3，通过 UDP 8092/8093 与 AAS3-CAM 握手并接收跟随数据。
- 两者走**完全独立的构建通道**（不同 SDKCONFIG / build 目录），互不影响。

## 调试方法

- 浏览器打开 `http://<摄像头板IP>/stream` 查看实时画面。
- `GET /healthz` 查看状态；跟随数据可抓 UDP 8093 报文验证。