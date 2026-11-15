# xiaozhi-vision-follow

小智 AI 视觉跟随系统（车尾背装小车），采用「电脑当大脑」三层架构：电脑端做检测与决策，ESP32 双板只负责推流与电机执行。

## 架构

```
电脑端 pc_brain（YOLOv8n 人体检测 + 决策）
      │ 拉流  http://192.168.1.11/stream
      ▼
发送板 sender_aas3cam（AAS3-CAM：MJPEG 推流 + UDP 转发）
      │ UDP 广播 MOTOR 指令（255.255.255.255:8093）
      ▼
接收板 receiver_ahuhu（ahuhu 板：电机执行）
```

## 目录结构

| 目录 | 作用 | 独立编译 |
|---|---|---|
| pc_brain/ | 电脑端大脑（Python + YOLOv8n） | ✅ |
| sender_aas3cam/ | 发送板固件（ESP-IDF 独立项目） | ✅ |
| receiver_ahuhu/ | 接收板固件（依赖小智主框架，作 patch） | ⚠️ |
| docs/ | 进度文档 | — |

## 快速开始

### 1. 电脑端（pc_brain）

依赖 Python 3.9+：

```bash
pip install -r pc_brain/requirements.txt
python pc_brain/phone_follow.py              # 跟随 + 画面
python pc_brain/phone_follow.py --no-show    # 只跟随
python pc_brain/phone_follow.py --check      # 拉一帧自检
```

运行前把 `phone_follow.py` 顶部 `STREAM_URL` / `SENDER_IP` 改成发送板实际 IP。

### 2. 发送板（sender_aas3cam）

ESP-IDF v5.5.4 + ESP32-S3，AAS3-CAM（OV3660，VGA 640×480）。

```powershell
cd sender_aas3cam
.\build_node.ps1 build     # 编译
.\build_node.ps1 flash     # 烧录
.\build_node.ps1 monitor   # 监控
```

### 3. 接收板（receiver_ahuhu）

接收端依赖小智主框架 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，不独立编译。
把 `receiver_ahuhu/` 下三个文件放到主仓库 `main/boards/ahuhu/` 编译即可：
- `ahuhu_board.cc/.h`：UDP 电机指令执行
- `config.h`：电机引脚（LF=12 LB=11 RF=14 RB=13）

## 决策逻辑

| 优先级 | 条件 | 动作 |
|---|---|---|
| 1 | 框宽占比 ≥0.80（≈0.5 米内）或贴边 | 前进（远离） |
| 2 | 框中心偏移 >0.12 | 转向对齐（比例转向） |
| 3 | 框宽占比 ≤0.40（≈1.5 米外） | 后退（靠近） |
| 4 | 其余 | 静止 |

电机指令 `MOTOR <dir> <speed> <dur>`，dir：1=右转 2=后退 3=左转 4=前进。

## 模型

`pc_brain/yolov8n.onnx`：Ultralytics YOLOv8n（COCO 预训练，只取 person 类）。
重新导出：

```bash
pip install ultralytics
yolo export model=yolov8n.pt format=onnx
```

## 凭据配置

仓库**不含任何真实凭据**：WiFi SSID/密码、VLM API Key 均为 `YOUR_*` 占位符。编译前二选一：

- 本地修改 `sender_aas3cam/main/cam_config.h`、`receiver_ahuhu/ahuhu_board.cc`（改动不要提交）
- 或用编译宏注入（优先级更高）：

```bash
idf.py -DCAM_WIFI_SSID=\"你的SSID\" -DCAM_WIFI_PASSWORD=\"你的密码\" -DCAM_VLM_KEY=\"sk-xxx\" build
```

## 文档

- [视觉方案进度](docs/视觉方案进度.md)：完整开发历程
- [发送板说明](sender_aas3cam/AAS3-CAM独立摄像头节点说明.md)
