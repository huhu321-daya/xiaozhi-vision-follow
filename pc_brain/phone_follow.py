#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
电脑当手机：拉 ESP32 发送板摄像头流 + YOLOv8 人体检测 + UDP 控制小车跟随。
用法：
  python phone_follow.py            # 正常跟随（带画面显示）
  python phone_follow.py --no-show  # 不显示画面
  python phone_follow.py --check    # 拉一帧检测后退出（自检）
"""
import cv2
import numpy as np
import socket
import threading
import time
import os
import sys
import urllib.request

# ── 网络配置 ──
STREAM_URL  = "http://192.168.1.11/stream"   # 发送板视频流
SENDER_IP   = "192.168.1.11"                  # 发送板（FOLLOW 开关发这里）
SENDER_PORT = 8094                            # 发送板 hello 监听端口
MOTOR_ADDR  = ("255.255.255.255", 8093)       # MOTOR 广播到接收板
BOUNDARY    = b"aas3camframe"

# ── 检测 / 决策参数 ──
CONF_TH  = 0.25   # 人体置信度阈值
NEAR_W   = 0.80   # 框宽占比 ≥ 此值（约0.5米内）→ 前进（远离）
FAR_W    = 0.40   # 框宽占比 ≤ 此值（约1.5米外）→ 后退（靠近）
TURN_ON  = 0.12   # 框中心偏移占比 > 此值 → 转向对齐（约8°以上才转）
FOV_DEG  = 66.0   # 摄像头水平视场角（度），用于偏移占比→角度换算
TURN_MS_PER_DEG = 9.0  # 速度70下每度约需9ms，按偏移量比例转向

# 电机编码：1=右转 2=后退 3=左转 4=前进
MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "yolov8n.onnx")


def send_udp(addr, msg):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.sendto(msg.encode(), addr)
    s.close()


def send_motor(dir, speed, dur):
    send_udp(MOTOR_ADDR, "MOTOR %d %d %d\n" % (dir, speed, dur))


# 拉流线程：持续读 MJPEG，只保留最新一帧（丢弃旧帧，避免延迟）
_latest = None
_lock = threading.Lock()


def stream_reader():
    global _latest
    marker = b"--" + BOUNDARY
    while True:
        try:
            resp = urllib.request.urlopen(STREAM_URL, timeout=15)
            data = b""
            while True:
                chunk = resp.read(4096)
                if not chunk:
                    break
                data += chunk
                while True:
                    i = data.find(marker)
                    if i < 0:
                        data = data[-len(marker) - 8:]
                        break
                    j = data.find(marker, i + len(marker))
                    if j < 0:
                        data = data[i:]
                        break
                    blob = data[i + len(marker):j]
                    data = data[j:]
                    he = blob.find(b"\r\n\r\n")
                    if he < 0:
                        continue
                    jpeg = blob[he + 4:].rstrip(b"\r\n")
                    if len(jpeg) < 200:
                        continue
                    img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
                    if img is not None:
                        with _lock:
                            _latest = img
        except Exception as e:
            print("stream error: %s, 重连中..." % e)
            time.sleep(2)


def get_frame():
    with _lock:
        return _latest.copy() if _latest is not None else None


def detect_person(net, img):
    """返回最大人框的 (w_ratio, offset_ratio, touch)，无人返回 None"""
    h, w = img.shape[:2]
    r = min(640 / w, 640 / h)
    nw, nh = int(w * r), int(h * r)
    resized = cv2.resize(img, (nw, nh))
    canvas = np.full((640, 640, 3), 114, np.uint8)
    canvas[:nh, :nw] = resized
    padx, pady = (640 - nw) // 2, (640 - nh) // 2

    blob = cv2.dnn.blobFromImage(canvas, 1 / 255.0, (640, 640), swapRB=True, crop=False)
    net.setInput(blob)
    out = net.forward()
    if out.shape[1] == 8400:            # (1,8400,84) -> (1,84,8400)
        out = out.transpose(0, 2, 1)
    preds = out[0].T                    # (8400, 84)

    boxes = preds[:, :4]                # cx, cy, w, h
    scores = preds[:, 4:]
    cls = scores.argmax(1)
    conf = np.where(cls == 0, scores.max(1), 0.0)  # 只留 person 类

    idx = np.argmax(conf)
    if conf[idx] < CONF_TH:
        return None
    cx, cy, bw, bh = boxes[idx]
    x1 = (cx - bw / 2 - padx) / r
    x2 = (cx + bw / 2 - padx) / r
    w_ratio = (x2 - x1) / w
    offset_ratio = ((x1 + x2) / 2 - w / 2) / w
    touch = (x1 <= 2 or x2 >= w - 2) and w_ratio > 0.5  # 框贴画面边缘 → 人太近身体超出画面
    return w_ratio, offset_ratio, touch


def main():
    show = "--no-show" not in sys.argv
    check = "--check" in sys.argv
    calib = "--calib" in sys.argv

    net = cv2.dnn.readNetFromONNX(MODEL_PATH)
    print("model loaded: %s" % os.path.basename(MODEL_PATH))

    t = threading.Thread(target=stream_reader, daemon=True)
    t.start()

    # 自检：拉一帧检测后退出
    if check:
        for _ in range(50):
            img = get_frame()
            if img is not None:
                break
            time.sleep(0.1)
        if img is None:
            print("拉流失败，检查 %s" % STREAM_URL)
            return
        res = detect_person(net, img)
        print("check: img=%dx%d person=%s" % (img.shape[1], img.shape[0], res))
        return

    # 关闭发送板自身的决策，避免命令冲突
    send_udp((SENDER_IP, SENDER_PORT), "FOLLOW_OFF")
    print("已发 FOLLOW_OFF，本机接管决策")
    time.sleep(0.3)

    if calib:
        send_motor(0, 0, 0)  # 标定模式：锁死车，只测距离

    last_dir = -1
    last_res = None
    last_person_t = 0.0
    next_act_t = 0.0   # 下次可发指令的时间点：接收板每条指令会阻塞 dur 毫秒，必须等它执行完再发
    try:
        while True:
            img = get_frame()
            if img is None:
                time.sleep(0.02)
                continue

            res = detect_person(net, img)
            if res is not None:
                last_res = res
                last_person_t = time.time()
            elif last_res is not None and time.time() - last_person_t < 0.6:
                res = last_res  # 短暂漏检沿用上一帧
            if res is None:
                dir, speed, dur = 0, 0, 0
                print("无人")
            else:
                w_ratio, off, touch = res
                if w_ratio >= NEAR_W or touch:
                    dir = 4; speed, dur = 60, 250   # 太近/贴边 → 前进（远离）
                elif abs(off) > TURN_ON:
                    # 比例转向：偏得多转得多（偏得少转得少），避免固定时长过冲来回摆
                    dur = int(abs(off) * FOV_DEG * TURN_MS_PER_DEG)
                    if dur < 30: dur = 30
                    elif dur > 200: dur = 200
                    dir = 1 if off > 0 else 3; speed = 70  # 人偏右→右转，偏左→左转
                elif w_ratio <= FAR_W:
                    dir = 2; speed, dur = 90, 250   # 远 → 后退（靠近）
                else:
                    dir = 0; speed, dur = 0, 0      # 中 → 停
                print("person w=%.2f off=%.2f touch=%d -> dir=%d ms=%d" % (w_ratio, off, touch, dir, dur))

            if not calib and time.time() >= next_act_t:
                if dir != 0:
                    send_motor(dir, speed, dur)
                    next_act_t = time.time() + dur / 1000.0 + 0.30  # 等指令执行完 + 新画面反馈
                    last_dir = dir
                elif last_dir != 0:
                    send_motor(0, 0, 0)   # 归零停车
                    last_dir = 0

            if show:
                disp = img.copy()
                if res:
                    h, w = img.shape[:2]
                    cv2.putText(disp, "dir=%d w=%.2f" % (dir, res[0]), (10, 30),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)
                cv2.imshow("phone follow", disp)
                if cv2.waitKey(1) & 0xFF == 27:
                    break
    except KeyboardInterrupt:
        pass
    finally:
        send_motor(0, 0, 0)
        send_udp((SENDER_IP, SENDER_PORT), "FOLLOW_ON")
        print("已恢复 FOLLOW_ON，退出")
        if show:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    main()