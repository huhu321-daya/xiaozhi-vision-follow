# -*- coding: utf-8 -*-
"""测试本地 Ollama 视觉模型：从摄像头板抓图 -> 调 qwen2.5vl -> 打印理解结果"""
import base64
import json
import sys
import time

import requests

CAM_URL = "http://192.168.1.11/capture"
OLLAMA_URL = "http://127.0.0.1:11434/api/chat"
MODEL = "moondream"


def main():
    # 1. 抓图：优先用命令行传入的本地图片，否则从摄像头板抓
    img_path = sys.argv[1] if len(sys.argv) > 1 else None
    if img_path:
        with open(img_path, "rb") as f:
            img = f.read()
        print(f"[图] 本地文件 {img_path} = {len(img)} bytes")
    else:
        try:
            img = requests.get(CAM_URL, timeout=8).content
            print(f"[图] 摄像头板抓图 = {len(img)} bytes")
        except Exception as e:
            print(f"[图] 抓图失败: {e}")
            return 1

    b64 = base64.b64encode(img).decode()

    # 2. 调 Ollama 视觉问答
    prompt = ("Describe in detail everything you can see in this image. "
              "List all objects, people, colors and their positions.")
    body = {
        "model": MODEL,
        "messages": [{"role": "user", "content": prompt, "images": [b64]}],
        "stream": False,
        "options": {"num_predict": 256},
    }
    t0 = time.time()
    try:
        r = requests.post(OLLAMA_URL, json=body, timeout=600)
    except Exception as e:
        print(f"[模型] 请求失败: {e}")
        return 1
    dt = time.time() - t0

    print(f"[模型] HTTP {r.status_code}, 耗时 {dt:.1f}s")
    try:
        data = r.json()
        print("[模型] 返回:")
        print(json.dumps(data.get("message", {}).get("content", ""),
                         ensure_ascii=False, indent=2))
    except Exception:
        print("[模型] 原始返回:")
        print(r.text[:2000])
    return 0


if __name__ == "__main__":
    sys.exit(main())