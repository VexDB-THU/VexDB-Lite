#!/usr/bin/env python3
"""Verify the exact single-image and single-text payloads used by the custom image UI."""

import base64
import json
import os
from pathlib import Path
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
RESOURCES = ROOT / "Resources"
ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/services/embeddings/multimodal-embedding/multimodal-embedding"
MODEL = "tongyi-embedding-vision-plus"
QUERY = "哪张图片里有一只羊和一辆汽车？"
EXPECTED = {
    "image-demo-1.png",
    "image-demo-sheep-car-2.png",
    "image-demo-sheep-car-3.png",
}


def embed(content, api_key):
    body = json.dumps({"model": MODEL, "input": {"contents": [content]}}).encode("utf-8")
    request = Request(ENDPOINT, data=body, method="POST", headers={
        "Authorization": "Bearer " + api_key,
        "Content-Type": "application/json",
    })
    with urlopen(request, timeout=120) as response:
        payload = json.loads(response.read().decode("utf-8"))
    return payload["output"]["embeddings"][0]["embedding"]


def squared_l2(left, right):
    return sum((a - b) ** 2 for a, b in zip(left, right))


def main():
    api_key = os.environ.get("DASHSCOPE_API_KEY", "").strip()
    if not api_key:
        raise SystemExit("DASHSCOPE_API_KEY is required")

    image_bytes = (RESOURCES / "image-demo-1.png").read_bytes()
    image_vector = embed({
        "image": "data:image/png;base64," + base64.b64encode(image_bytes).decode("ascii")
    }, api_key)
    query_vector = embed({"text": QUERY}, api_key)
    if len(image_vector) != 1152 or len(query_vector) != 1152:
        raise SystemExit("unexpected vector dimensions")

    payload = json.loads((RESOURCES / "image-demo-vectors.json").read_text(encoding="utf-8"))
    ranked = sorted(payload["images"],
                    key=lambda item: squared_l2(query_vector, item["embedding"]))
    actual = {item["resource"] for item in ranked[:3]}
    if actual != EXPECTED:
        raise SystemExit("single-text query top three mismatch: {}".format(sorted(actual)))

    print("single image vector: 1152 dimensions")
    print("single text vector: 1152 dimensions")
    print("single-text query top-3 group recall: 100%")


if __name__ == "__main__":
    main()
