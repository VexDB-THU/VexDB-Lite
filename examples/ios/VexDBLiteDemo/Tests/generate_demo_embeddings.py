#!/usr/bin/env python3
"""Generate the bundled embeddings used by the default iOS demo."""

import json
import os
import pathlib
import re
import sys
from typing import List, Optional, Tuple
import urllib.error
import urllib.request


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Resources" / "embedding-demo.txt"
OUTPUT = ROOT / "Resources" / "embedding-demo-vectors.json"
MODEL = "text-embedding-v4"
ENDPOINT = "https://dashscope.aliyuncs.com/compatible-mode/v1/embeddings"
CHUNK_LENGTH = 500
OVERLAP_LENGTH = 0
QUERIES = [
    "自动驾驶汽车如何与道路设施通信？",
    "如何用 Bloom Filter 解决缓存穿透？",
    "智慧农业如何使用无人机和遥感技术？",
    "个人信息保护需要哪些安全技术？",
    "北京的 GDP 是多少？",
    "Smart City 如何预测 traffic congestion？",
    "什么是向量数据库的近似最近邻检索？",
]


TOP_HEADING = re.compile(r"^([一二三四五六七八九十百千万]+)、")
SUB_HEADING = re.compile(r"^(\d+)(?:\.\d+)+\s")


def heading_key(line: str) -> Optional[str]:
    match = TOP_HEADING.match(line) or SUB_HEADING.match(line)
    return match.group(1) if match else None


def split_sentences(text: str) -> List[str]:
    sentences: List[str] = []
    start = 0
    for index, char in enumerate(text):
        if char in "。！？!?" or char == "\n":
            value = text[start : index + 1].strip()
            if value:
                sentences.append(value)
            start = index + 1
    tail = text[start:].strip()
    if tail:
        sentences.append(tail)
    return sentences


def overlap_tail(text: str, limit: int) -> str:
    if limit <= 0:
        return ""
    sentences = split_sentences(text)
    tail = sentences[-1] if sentences else ""
    return tail if len(tail) <= limit else ""


def split_text(source: str) -> List[str]:
    """Keep headings and complete sentences together, with a safe fallback."""
    lines = [line.strip() for line in source.strip().splitlines() if line.strip()]
    groups: List[Tuple[str, List[str]]] = []
    current_key = "intro"
    current: List[str] = []
    for line in lines:
        match = TOP_HEADING.match(line)
        if match is not None:
            if current:
                groups.append((current_key, current))
            current_key = match.group(1)
            current = [line]
        else:
            current.append(line)
    if current:
        groups.append((current_key, current))
    if len(groups) > 1 and groups[0][0] == "intro":
        groups[1] = (groups[1][0], groups[0][1] + groups[1][1])
        groups = groups[1:]

    sections: List[Tuple[str, str]] = []
    for key, group in groups:
        block: List[str] = []
        for line in group:
            if block and SUB_HEADING.match(line):
                sections.append((key, "\n".join(block).strip()))
                block = []
            block.append(line)
        if block:
            sections.append((key, "\n".join(block).strip()))

    chunks: List[str] = []
    pending = ""
    pending_key = ""

    def flush() -> None:
        nonlocal pending
        if pending.strip():
            chunks.append(pending.strip())
        pending = ""

    for key, section in sections:
        pieces = split_sentences(section) if len(section) > CHUNK_LENGTH else [section]
        for piece in pieces:
            if len(piece) > CHUNK_LENGTH:
                flush()
                start = 0
                while start < len(piece):
                    end = min(start + CHUNK_LENGTH, len(piece))
                    if end < len(piece):
                        candidate = piece.rfind(" ", start + CHUNK_LENGTH // 2, end)
                        if candidate > start:
                            end = candidate
                    chunks.append(piece[start:end].strip())
                    start = end
                pending_key = ""
            elif pending and pending_key == key and len(pending) + 1 + len(piece) <= CHUNK_LENGTH:
                pending += "\n" + piece
            else:
                carry = overlap_tail(pending, OVERLAP_LENGTH) if pending_key == key else ""
                flush()
                pending = (carry + "\n" + piece).strip() if carry and len(carry) + 1 + len(piece) <= CHUNK_LENGTH else piece
                pending_key = key
    flush()
    return chunks


def embed(inputs: List[str], api_key: str) -> List[List[float]]:
    payload = json.dumps({"model": MODEL, "input": inputs}, ensure_ascii=False).encode()
    request = urllib.request.Request(
        ENDPOINT,
        data=payload,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            body = json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")[:500]
        raise RuntimeError(f"Embedding API returned HTTP {error.code}: {detail}") from error
    return [item["embedding"] for item in sorted(body["data"], key=lambda item: item["index"])]


def embed_batched(inputs: List[str], api_key: str) -> List[List[float]]:
    vectors: List[List[float]] = []
    for start in range(0, len(inputs), 10):
        batch = inputs[start : start + 10]
        vectors.extend(embed(batch, api_key))
        print(f"embedded {min(start + 10, len(inputs))}/{len(inputs)}", flush=True)
    return vectors


def compact(vector: List[float]) -> List[float]:
    # Eight decimal places retain far more precision than the ANN ranking needs
    # while keeping the bundled JSON small enough for a demo App.
    return [round(value, 8) for value in vector]


def main() -> int:
    api_key = os.environ.get("DASHSCOPE_API_KEY", "")
    if not api_key:
        print("DASHSCOPE_API_KEY is required", file=sys.stderr)
        return 2
    chunks = split_text(SOURCE.read_text(encoding="utf-8"))
    chunk_vectors = embed_batched(chunks, api_key)
    query_vectors = embed_batched(QUERIES, api_key)
    dimensions = len(chunk_vectors[0])
    if any(len(vector) != dimensions for vector in chunk_vectors + query_vectors):
        raise RuntimeError("Embedding dimensions are inconsistent")
    output = {
        "model": MODEL,
        "dimensions": dimensions,
        "chunkLength": CHUNK_LENGTH,
        "overlapLength": OVERLAP_LENGTH,
        "chunks": [
            {"id": index + 1, "text": text, "embedding": compact(vector)}
            for index, (text, vector) in enumerate(zip(chunks, chunk_vectors))
        ],
        "queries": [
            {"text": text, "embedding": compact(vector)}
            for text, vector in zip(QUERIES, query_vectors)
        ],
    }
    OUTPUT.write_text(json.dumps(output, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print(f"wrote {OUTPUT}: {len(chunks)} chunks, {dimensions} dimensions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
