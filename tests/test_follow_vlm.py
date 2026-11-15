# -*- coding: utf-8 -*-
"""测试硅基流动 Qwen3-VL 做小车跟随结构化判断的延迟与准确率。

返回 {"has":0/1, "zone":"L/M/R", "dist":1/2/3}
has=1 有人；zone=人相对画面中心的水平位置；dist=1远 2中 3近
"""
import base64
import json
import os
import sys
import time

import requests

URL = "https://api.siliconflow.cn/v1/chat/completions"
API_KEY = os.getenv("VLM_API_KEY", "YOUR_VLM_API_KEY")  # export VLM_API_KEY=sk-xxx
MODEL = "Qwen/Qwen3-VL-8B-Instruct"

PROMPT = (
    "这是小车摄像头画面。只回答一个JSON，不要任何其他文字，格式严格为："
    '{"has":0,"zone":"M","dist":2}。'
    "has=1表示画面里有人，has=0表示没人；"
    "zone表示人相对画面中心的水平位置，L左、M中、R右；"
    "dist=1表示人远、2表示中等距离、3表示很近。"
)


def once(img_b64):
    body = {
        "model": MODEL,
        "max_tokens": 50,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": PROMPT},
                    {"type": "image_url",
                     "image_url": {"url": f"data:image/jpeg;base64,{img_b64}"}},
                ],
            }
        ],
    }
    headers = {"Authorization": f"Bearer {API_KEY}",
               "Content-Type": "application/json"}
    t0 = time.time()
    r = requests.post(URL, json=body, headers=headers, timeout=60)
    dt = time.time() - t0
    if r.status_code != 200:
        return dt, f"HTTP {r.status_code}", r.text[:200]
    data = r.json()
    content = data["choices"][0]["message"]["content"]
    return dt, content, None


def main():
    img_path = sys.argv[1] if len(sys.argv) > 1 else "test_cam.jpg"
    with open(img_path, "rb") as f:
        img = f.read()
    print(f"[图] {img_path} = {len(img)} bytes")
    b64 = base64.b64encode(img).decode()

    # 先测一句简要描述（对比：你改的 50 字方案），再测结构化跟随
    print("\n=== 1) 简要描述（对照你改的 50 字方案） ===")
    t = time.time()
    body = {
        "model": MODEL, "max_tokens": 200,
        "messages": [{"role": "user", "content": [
            {"type": "text", "text": "请用一句中文简要描述这张照片的内容，控制在50个字以内"},
            {"type": "image_url",
             "image_url": {"url": f"data:image/jpeg;base64,{b64}"}}]}]}
    headers = {"Authorization": f"Bearer {API_KEY}",
               "Content-Type": "application/json"}
    r = requests.post(URL, json=body, headers=headers, timeout=60)
    print(f"  耗时 {time.time()-t:.1f}s")
    if r.status_code == 200:
        print(f"  返回: {r.json()['choices'][0]['message']['content']}")
    else:
        print(f"  HTTP {r.status_code} {r.text[:200]}")

    print("\n=== 2) 结构化跟随判断（连续 5 次测延迟） ===")
    times = []
    for i in range(5):
        dt, content, err = once(b64)
        times.append(dt)
        if err:
            print(f"  第{i+1}次: {dt:.1f}s  错误: {content} {err}")
        else:
            print(f"  第{i+1}次: {dt:.1f}s  返回: {content.strip()}")
    times.sort()
    print(f"\n延迟: 最快 {times[0]:.1f}s  中位 {times[len(times)//2]:.1f}s  "
          f"最慢 {times[-1]:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())