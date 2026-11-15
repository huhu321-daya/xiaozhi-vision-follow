# xiaozhi-vision-follow

小智 AI 视觉跟随系统（车尾背装小车），「电脑当大脑」三层架构：电脑端做检测与决策，ESP32 双板只负责推流与电机执行。

本仓库同时提供**可运行成品**与**完整源码**：发送板可独立编译，接收板以整目录覆盖回 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 主框架，电脑端纯 Python 即可跑。

## 架构

```
电脑端 pc_brain（YOLOv8n 人体检测 + 跟随决策）
      │ 拉流  http://<发送板IP>/stream
      ▼
发送板 sender_aas3cam（AAS3-CAM：MJPEG 推流 + 本地人体检测 + UDP 广播）
      │ UDP MOTOR 指令（255.255.255.255:8093）
      ▼
接收板 main/boards/ahuhu（ahuhu 板：电机执行 + 彩屏待机）
```

## 目录

| 路径 | 作用 | 能否独立编译 |
|---|---|---|
| `pc_brain/` | 电脑端大脑（Python + YOLOv8n，含 `yolov8n.onnx`） | ✅ |
| `sender_aas3cam/` | 发送板固件（ESP-IDF v5.5.4 / ESP32-S3 / OV3660 VGA） | ✅ |
| `main/boards/ahuhu/` | 接收板完整板级实现（15 文件：UDP 电机、OV7670、ST7735 屏、天气/新闻服务） | ⚠️ 依赖小智主框架 |
| `main/boards/AAS3-CAM/` | 发送板板级说明 | — |
| `tests/` | 视觉链路验证脚本（硅基流动 / 智谱 / 本地 Ollama） | ✅ |
| `docs/` | 三阶段方案演进、任务记录、彩屏待机要求 | — |
| `build_aas3cam.ps1`、`sdkconfig.aas3cam.defaults` | 发送节点编译脚本与配置基线 | — |

## 快速开始

### 1. 电脑端（pc_brain）

```bash
pip install -r pc_brain/requirements.txt
python pc_brain/phone_follow.py              # 跟随 + 画面
python pc_brain/phone_follow.py --no-show    # 只跟随
python pc_brain/phone_follow.py --check      # 拉一帧自检
```

运行前把 `phone_follow.py` 顶部 `STREAM_URL` / `SENDER_IP` 改成发送板实际 IP。

### 2. 发送板（sender_aas3cam）

```powershell
cd sender_aas3cam
.\build_node.ps1 build     # 编译
.\build_node.ps1 flash     # 烧录
.\build_node.ps1 monitor   # 监控
```

### 3. 接收板（main/boards/ahuhu）

不独立编译，需在小智主框架内使用：

```bash
git clone https://github.com/78/xiaozhi-esp32
cp -r main/boards/ahuhu /path/to/xiaozhi-esp32/main/boards/
```

`config.h` 内电机引脚：LF=12 LB=11 RF=14 RB=13。

## 决策逻辑

| 优先级 | 条件 | 动作 |
|---|---|---|
| 1 | 框宽占比 ≥0.80（≈0.5 米内）或贴边 | 前进（远离） |
| 2 | 框中心偏移 >0.12 | 转向对齐（比例转向） |
| 3 | 框宽占比 ≤0.40（≈1.5 米外） | 后退（靠近） |
| 4 | 其余 | 静止 |

电机指令 `MOTOR <dir> <speed> <dur>`，dir：1=右转 2=后退 3=左转 4=前进。

## 凭据配置

仓库**不含任何真实凭据**，全部为 `YOUR_*` 占位符。

C 侧（`sender_aas3cam/main/cam_config.h`、`main/boards/ahuhu/ahuhu_board.cc`）已用 `#ifndef` 包裹，编译宏优先：

```bash
idf.py -DCAM_WIFI_SSID=\"你的SSID\" -DCAM_WIFI_PASSWORD=\"密码\" -DCAM_VLM_KEY=\"sk-xxx\" build
```

Python 测试脚本读环境变量：

```bash
export VLM_API_KEY=sk-xxx        # tests/test_follow_vlm.py（硅基流动）
export ZHIPU_API_KEY=xxx         # tests/test_zhipu_vision.py（智谱）

python tests/test_zhipu_vision.py    # 抓图 → glm-4v-flash
python tests/test_follow_vlm.py      # Qwen3-VL 结构化判断延迟测试
python tests/test_ollama_vision.py   # 本地 Ollama
```

## 模型

`pc_brain/yolov8n.onnx`：Ultralytics YOLOv8n（COCO 预训练，只取 person 类）。重新导出：

```bash
pip install ultralytics && yolo export model=yolov8n.pt format=onnx
```

## 文档

- [视觉方案进度](docs/视觉方案进度.md)：三阶段演进（小脑控制 → 小脑+大脑协作 → 电脑端大脑）完整开发历程
- [发送板说明](sender_aas3cam/AAS3-CAM独立摄像头节点说明.md)
- [接收板说明](main/boards/ahuhu/README.md)
