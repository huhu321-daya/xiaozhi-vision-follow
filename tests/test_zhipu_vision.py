# -*- coding: utf-8 -*-
"""测试智谱 GLM-4V 视觉接口：抓图 -> 调 glm-4v-flash -> 打印结果"""
import base64
import json
import os
import sys
import time

import requests

CAM_URL = "http://192.168.1.11/capture"
ZHIPU_URL = "https://open.bigmodel.cn/api/paas/v4/chat/completions"
API_KEY = os.getenv("ZHIPU_API_KEY", "YOUR_ZHIPU_API_KEY")  # export ZHIPU_API_KEY=xxx
MODEL = "glm-4v-flash"


def main():
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
    data_url = f"data:image/jpeg;base64,{b64}"

    body = {
        "model": MODEL,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": "描述图片里有什么，人在哪，有什么物体。"},
                    {"type": "image_url", "image_url": {"url": data_url}},
                ],
            }
        ],
    }
    headers = {
        "Authorization": f"Bearer {API_KEY}",
        "Content-Type": "application/json",
    }

    t0 = time.time()
    try:
        r = requests.post(ZHIPU_URL, json=body, headers=headers, timeout=60)
    except Exception as e:
        print(f"[模型] 请求失败: {e}")
        return 1
    dt = time.time() - t0

    print(f"[模型] HTTP {r.status_code}, 耗时 {dt:.1f}s")
    try:
        data = r.json()
    except Exception:
        print("[模型] 原始返回:")
        print(r.text[:2000])
        return 1

    if r.status_code == 200:
        content = data["choices"][0]["message"]["content"]
        print("[模型] 返回:")
        print(content)
    else:
        print("[模型] 错误:")
        print(json.dumps(data, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())