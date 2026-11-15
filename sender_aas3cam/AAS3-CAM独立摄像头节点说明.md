# AAS3-CAM 独立摄像头节点

一个**不依托小智框架**的 ESP32-S3 摄像头节点固件。独立 ESP-IDF 工程，代码精简（只有 5 个源文件），通过 WiFi 提供 MJPEG 视频流、单帧抓拍、目标跟随检测，并通过 UDP 对外广播检测结果，供其他设备（如机器人主控）使用。

- 芯片：ESP32-S3（QFN56，8MB PSRAM，16MB Flash）
- 摄像头：OV3660（DVP 8-bit，QVGA 320×240）
- 传感器：检测到 OV3660（PID=0x3660）
- 工程目录：`aas3cam_node/`（与主工程 `main/` 完全解耦）

---

## 一、功能特性

| 功能 | 说明 |
|------|------|
| WiFi 直连 | 2.4G STA 模式，自动扫描并锁定 BSSID+信道，解决双频同名路由认证失败 |
| MJPEG 视频流 | `/stream`、`/camera`，multipart/x-mixed-replace 实时流 |
| 单帧抓拍 | `/capture`，返回一张 JPEG |
| 健康检查 | `/healthz`，返回 JSON 运行状态 |
| 目标跟随检测 | 对画面做运动检测，判断目标左右位置与距离等级 |
| UDP 设备发现 | 8092 端口，响应 `XIAOZHI_CAM_DISCOVER` 返回本机 IP |
| UDP 跟随广播 | 8093 端口，每 200ms 广播 `FOLLOW <has> <zone> <dist>` |

---

## 二、工作逻辑

### 2.1 整体架构

```
                    ┌─────────────────────────────────────────┐
                    │              ESP32-S3                    │
                    │                                          │
 OV3660  ──DVP──>  │  cam_task(采集, 优先级6, core1)            │
                    │     │ JPEG 帧拷贝到 PSRAM 共享槽           │
                    │     ▼                                    │
                    │  ┌─────────────┐                         │
                    │  │ 共享帧槽 s_slot │ (互斥锁保护, 最新帧)   │
                    │  └──────┬──────┘                         │
                    │         │                                │
                    │  ┌──────┴──────────┬─────────────┐       │
                    │  ▼                 ▼             ▼        │
                    │ stream_task     follow_task   capture     │
                    │ (流,优先级5)     (跟随,优先级1)  (抓拍)      │
                    │   │                 │                      │
                    └──┼─────────────────┼──────────────────────┘
                       ▼                 ▼
                  HTTP /stream     UDP 8093 广播
                  HTTP /capture     FOLLOW has zone dist
```

### 2.2 线程划分（FreeRTOS）

| 任务 | 优先级 | 核心 | 职责 |
|------|--------|------|------|
| `cam`（采集） | 6 | 1 | 持续取帧，JPEG 拷贝到 PSRAM 共享槽，15fps 固定间隔 |
| `follow`（跟随） | 1 | 1 | 每 1s 解码一帧，运动检测，更新目标位置/距离 |
| `stream`（流） | 5 | 1 | 每路客户端一个任务，从共享槽取最新帧发送 |
| `disc`（发现） | 5 | 任意 | UDP 8092 监听，响应发现请求 |
| `follow`（UDP广播） | 3 | 任意 | 每 200ms 广播跟随结果 |

**设计要点**：采集任务不做解码，只负责搬运最新帧，保证摄像头 DMA 不被打断；JPEG 解码单独放在低优先级跟随线程，不占用采集核心，避免帧堆积与延迟。

### 2.3 采集与帧管理

1. `cam_task` 循环调用 `esp_camera_fb_get()` 取帧（`CAMERA_GRAB_LATEST` 模式，总是拿最新帧）。
2. 拿到 JPEG 帧后，在互斥锁保护下 `memcpy` 到 PSRAM 共享槽 `s_slot`，更新序号 `s_seq` 与时间戳。
3. 归还帧缓冲，`vTaskDelay(1000/15)` 固定 15fps，降低 WiFi/PSRAM 总线竞争。
4. 若连续 30 次取帧失败，自动 `esp_camera_deinit()` 后重新初始化（最多 3 次，超过则 `esp_restart()`）。

### 2.4 目标跟随检测

1. `follow_task` 每 1000ms 从共享槽取一帧 JPEG。
2. 用 `esp_jpeg_decode` 以 **1/4 缩放**解码为 RGB888（320×240 → 80×60），输出缓冲放在**内部 SRAM**（降低 PSRAM 带宽占用）。
3. 将画面划分为 **32×18 网格**，逐格计算亮度，与上一帧差分，亮度变化 >18 判定为运动。
4. 运动格数 `active >= 10` 判定有目标，否则 `has=0`。
5. 目标水平位置：按运动格加权平均 x 坐标，落入左 1/3 → `L`，右 1/3 → `R`，否则 `C`。
6. 距离等级：`active >= 90` → 3（近），`>= 45` → 2（中），否则 1（远）。

### 2.5 HTTP 视频流

- 流使用 **multipart/x-mixed-replace**，每帧一个 `--boundary` 分界。
- 最多同时 **2 路**流，新客户端进来会顶掉最旧的连接，避免僵尸 socket 占满。
- 流任务用**原始 `send()`** 直接写 socket（绕过 httpd 全局锁），并设置 `TCP_NODELAY`（关 Nagle）、`SO_LINGER`（快速释放）、发送超时 10s。
- 若 6s 无新帧，流任务自动退出，避免僵尸任务挂死。

### 2.6 WiFi 连接

1. 先 `esp_wifi_scan_start` 扫描指定 SSID。
2. 找到 2.4G（信道 1~14）的 AP 后，锁定其 BSSID + 信道，避免双频同名路由认证失败。
3. 连接成功后 `esp_wifi_set_ps(WIFI_PS_NONE)` **关闭 WiFi 省电**，消除批量帧延迟。
4. 掉线自动重连（`WIFI_EVENT_STA_DISCONNECTED` 事件里延迟 2s 后重连）。

---

## 三、目录结构

```
aas3cam_node/
├── CMakeLists.txt              # 顶层工程定义（组件 + esp32-camera 依赖）
├── sdkconfig.defaults          # ESP32-S3 / PSRAM Octal / 16MB Flash / 关 BT+TLS
├── partitions.csv              # 16MB 分区表（factory 4MB）
├── README.md                   # 本文档
├── main/
│   ├── CMakeLists.txt          # 5 个源文件 + 组件依赖
│   ├── main.c                  # 入口：初始化摄像头→WiFi→HTTP→UDP
│   ├── cam_config.h            # 引脚 / WiFi / 参数配置（改这里）
│   ├── cam_camera.h            # 摄像头接口声明
│   ├── cam_camera.c            # 采集 + 跟随检测（核心）
│   ├── cam_http.h              # HTTP 接口声明
│   ├── cam_http.c              # HTTP 流/抓拍/健康检查
│   ├── cam_net.h               # 网络接口声明
│   └── cam_net.c               # WiFi + UDP 发现/广播
└── managed_components/
    └── espressif__esp32-camera # esp32-camera 驱动（OV3660）
```

---

## 四、环境准备

需要 ESP-IDF v5.5.4，安装在 `D:\S3\v5.5.4\esp-idf`，工具链在 `C:\Espressif`。

每次编译前，在 PowerShell 设置环境变量：

```powershell
$env:IDF_PATH = "D:\S3\v5.5.4\esp-idf"
$env:PATH = "C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.9;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;" + $env:PATH
```

---

## 五、编译 / 烧录 / 监视

所有命令在 **`aas3cam_node` 目录**下执行（先 `cd d:\xiaozhi-esp32-main\aas3cam_node`）。

### 方式一：直接 idf.py（推荐）

```powershell
# 编译
& "C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe" "D:\S3\v5.5.4\esp-idf\tools\idf.py" build

# 烧录（COM4 按实际串口改）
& "C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe" "D:\S3\v5.5.4\esp-idf\tools\idf.py" -p COM4 flash

# 监视（看日志）
& "C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe" "D:\S3\v5.5.4\esp-idf\tools\idf.py" -p COM4 monitor
```

### 方式二：带进度脚本

根目录有 `build_progress.ps1`（编译显示 `[N/M]` 文件数，烧录显示百分比进度条）。在 `aas3cam_node` 目录下调用：

```powershell
..\build_progress.ps1 build        # 编译
..\build_progress.ps1 flash        # 烧录（默认 COM3，可用 -Port COM4 覆盖）
..\build_progress.ps1 monitor      # 监视
..\build_progress.ps1 all          # 编译 + 烧录
```

> 烧录前若报 `Could not open COM4`（端口被占），先关掉旧的监视器进程再烧。

---

## 六、配置说明

所有可调配置集中在 `main/cam_config.h`：

```c
#define CAM_WIFI_SSID       "YOUR_WIFI_SSID"      // WiFi 名称
#define CAM_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"  // WiFi 密码

// 摄像头引脚（AAS3-CAM 板）
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

#define CAM_XCLK_HZ         10000000     // 摄像头时钟（10MHz，降低 EMI 干扰 WiFi）
#define CAM_CAPTURE_FPS     15           // 采集帧率
#define CAM_FRAME_CAP       (128 * 1024) // 单帧 JPEG 最大字节数
#define CAM_FOLLOW_MS       1000         // 跟随检测间隔

#define CAM_UDP_DISCOVER    8092         // 发现端口
#define CAM_UDP_FOLLOW      8093         // 跟随广播端口
#define CAM_DISCOVER_TAG    "XIAOZHI_CAM_DISCOVER"
```

---

## 七、对外接口

### 7.1 HTTP（设备 IP，如 `http://192.168.1.11`）

| 路径 | 方法 | 返回 |
|------|------|------|
| `/stream` | GET | MJPEG 流（multipart/x-mixed-replace） |
| `/camera` | GET | 同 `/stream`（别名） |
| `/capture` | GET | 单张 JPEG |
| `/healthz` | GET | JSON 状态 |

`/healthz` 返回示例：

```json
{"uptime_s":26,"frame_age_ms":34,"frames":343,"follow":[0,"C",0],"heap":8235440,"psram":8076496}
```

- `frame_age_ms`：最新帧年龄（ms）
- `follow`：`[有无目标, 位置(L/C/R), 距离等级(0~3)]`

### 7.2 UDP 发现（8092）

向局域网广播发送字符串 `XIAOZHI_CAM_DISCOVER`，设备收到后回送其 IP 地址（点分十进制文本）。

### 7.3 UDP 跟随广播（8093）

设备每 200ms 向 `255.255.255.255:8093` 广播一行：

```
FOLLOW <has> <zone> <dist>
```

- `has`：1 有目标 / 0 无目标
- `zone`：`L` 左 / `C` 中 / `R` 右
- `dist`：距离等级 0~3（越大越近）

---

## 八、关键优化记录（延迟/断流修复）

本板子曾出现视频流卡顿、延迟高、`httpd_sock_err: error in send : 11` 断流问题，最终定位根因并修复：

| 问题 | 根因 | 修复 |
|------|------|------|
| 延迟高（ping 293ms） | XCLK 20MHz 的 EMI 干扰 2.4G WiFi 空口 | XCLK 降到 10MHz（PCLK 10→5MHz） |
| 空口抖动（最长 1144ms） | 采集无间隔、DMA 持续争抢总线 | 采集 15fps 固定间隔 |
| 断流（send:11 = EAGAIN） | 采集线程被解码阻塞、发送超时过短 | 解码剥离到独立线程、超时调到 10s |
| 批量延迟 | WiFi 省电模式 | `WIFI_PS_NONE` 关闭休眠 |
| 小包延迟 | Nagle 算法合并小包 | `TCP_NODELAY` 关闭 Nagle |
| 帧堆积 | 全尺寸 JPEG 解码占满 PSRAM 带宽 | `esp_jpeg` 1/4 缩放 + 输出移内部 SRAM |

**最终实测**：ping 平均 11ms（修复前 293ms）、0 丢包、无断流，流速率 108KB/s 稳定，帧率约 13fps，帧年龄 34ms。

---

## 九、验证命令

设备启动并连上 WiFi 后（日志出现 `ip=192.168.1.11`）：

```powershell
# 健康检查
curl.exe -s http://192.168.1.11/healthz

# 抓拍一张（存为 jpg）
curl.exe -s -o cap.jpg http://192.168.1.11/capture

# 拉流 5 秒统计速率
curl.exe -s --max-time 5 -o NUL -w "speed=%{speed_download}B/s" http://192.168.1.11/stream

# 网络延迟
ping -n 20 192.168.1.11
```

---

## 十、实测记录

- 测试日期：**2026-11-12**
- 设备固件编译时间：Nov 11 2026 20:10:24
- 测试时设备上电约 26 秒（`uptime_s=26`）

### 10.1 GET /healthz

```powershell
curl.exe -s http://192.168.1.11/healthz
```

返回：

```json
{"uptime_s":26,"frame_age_ms":34,"frames":343,"follow":[0,"C",0],"heap":8235440,"psram":8076496}
```

### 10.2 GET /capture

```powershell
curl.exe -s -o NUL -w "capture HTTP %{http_code} size=%{size_download}" http://192.168.1.11/capture
```

返回：

```
capture HTTP 200 size=7818
```

（返回一张 7818 字节的 JPEG，HTTP 200）

### 10.3 GET /stream（拉流 5 秒）

```powershell
curl.exe -s --max-time 5 -o NUL -w "stream HTTP %{http_code} recv=%{size_download} bytes in %{time_total}s speed=%{speed_download}B/s" http://192.168.1.11/stream
```

返回：

```
stream HTTP 200 recv=544014 bytes in 5.006359s speed=108664B/s
```

（5 秒持续收到 544KB，速率约 108KB/s，无断流）

### 10.4 网络延迟（ping）

| 样本数 | 最短 | 最长 | 平均 | 丢包 |
|--------|------|------|------|------|
| 15 包 | 1ms | 51ms | 13ms | 0% |
| 40 包 | 2ms | 44ms | 11ms | 0% |