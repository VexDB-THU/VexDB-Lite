#!/usr/bin/env python3
"""Generate bundled image and preset-query vectors from the multimodal API."""

import base64
import argparse
import json
import mimetypes
import os
from pathlib import Path
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
RESOURCES = ROOT / "Resources"
OUTPUT = RESOURCES / "image-demo-vectors.json"
ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/services/embeddings/multimodal-embedding/multimodal-embedding"
MODEL = "tongyi-embedding-vision-plus"

IMAGES = [
    ("image-test-img01.jpg", "仰视视角的现代高层建筑，立面有密集的水平线条", ["建筑", "高楼", "现代", "仰视"]),
    ("image-test-img02.jpg", "夕阳下横跨海湾的红色悬索桥", ["桥梁", "红色", "悬索桥", "海湾"]),
    ("image-test-img03.jpg", "一个孩子捧着《HOLY BIBLE》专心阅读", ["人物", "孩子", "书", "阅读"]),
    ("image-test-img04.jpg", "木地板上摆放着斧头、锤子和手套等旧工具", ["工具", "木地板", "斧头", "锤子"]),
    ("image-test-img05.jpg", "金色田野里的传统农耕场景，有马匹、农机和干草垛", ["田野", "农业", "马", "农机", "干草"]),
    ("image-test-img06.jpg", "夕阳照亮海面，一只海鸥在码头附近低飞", ["海面", "夕阳", "海鸥", "码头"]),
    ("image-test-img07.jpg", "逆光下一个背对镜头站在草地上的人", ["人物", "背影", "草地", "逆光"]),
    ("image-test-img08.jpg", "高举火炬的自由女神像黑白照片", ["自由女神像", "黑白", "火炬", "雕像"]),
    ("image-test-img09.jpg", "叶片饱满、呈莲座状排列的绿色多肉植物", ["多肉", "植物", "绿色", "莲座"]),
    ("image-test-img10.jpg", "带有纹理、结疤和鞋印的棕色木地板", ["木板", "木地板", "纹理", "棕色"]),
    ("image-test-img11.jpg", "雪山和海浪前的一艘红白色小渔船", ["渔船", "海浪", "雪山", "红白"]),
    ("image-test-img12.jpg", "蓝天中一片纤细弯曲的白云", ["蓝天", "白云", "天空", "云朵"]),
    ("image-test-img13.jpg", "棕色卷发、红唇女性的面部肖像特写", ["女性", "肖像", "卷发", "红唇"]),
    ("image-test-img14.jpg", "云雾笼罩的山谷和远处的针叶林", ["山谷", "云雾", "森林", "风景"]),
    ("image-test-img15.jpg", "岩石山地上方的蓝色星轨夜空", ["星空", "星轨", "夜空", "岩石"]),
    ("image-test-img16.jpg", "停在水面上的橙色起重工程船", ["工程船", "起重机", "水面", "港口"]),
    ("image-test-img17.jpg", "桌面上打开的一台银灰色笔记本电脑", ["笔记本电脑", "桌面", "银灰色", "办公"]),
    ("image-test-img18.jpg", "夕阳照亮云海和远处的群山", ["夕阳", "云海", "群山", "风景"]),
    ("image-test-img19.jpg", "穿过绿色山谷的铁路轨道", ["铁路", "铁轨", "山谷", "绿色"]),
    ("image-test-img20.jpg", "雾气中的金色山坡和几棵树", ["山坡", "雾气", "树木", "风景"]),
]

QUERIES = [
    ("img03_q1", "正在阅读圣经的孩子", "image-test-img03.jpg"),
    ("img08_q1", "高举火炬的自由女神像", "image-test-img08.jpg"),
    ("img19_q1", "穿过绿色山谷的铁路", "image-test-img19.jpg"),
]


def image_data_url(path):
    mime = mimetypes.guess_type(path.name)[0] or "image/png"
    return "data:{};base64,{}".format(mime, base64.b64encode(path.read_bytes()).decode("ascii"))


def request_embeddings(api_key, contents):
    body = json.dumps({"model": MODEL, "input": {"contents": contents}}).encode("utf-8")
    request = Request(ENDPOINT, data=body, method="POST", headers={
        "Authorization": "Bearer " + api_key,
        "Content-Type": "application/json",
    })
    with urlopen(request, timeout=120) as response:
        payload = json.loads(response.read().decode("utf-8"))
    embeddings = sorted(payload["output"]["embeddings"], key=lambda item: item["index"])
    if len(embeddings) != len(contents):
        raise SystemExit("Embedding count mismatch")
    return [item["embedding"] for item in embeddings]


def load_dataset(dataset_root, resource_prefix, subset=None, captions_file=None, queries_file=None):
    captions_path = captions_file or (dataset_root / "image_captions.json")
    captions = json.loads(captions_path.read_text(encoding="utf-8"))
    if subset:
        captions = [item for item in captions if item.get("subset") == subset]
        query_path = queries_file or (dataset_root / f"queries_{subset}.jsonl")
    else:
        query_path = queries_file or (dataset_root / "queries.jsonl")
    queries = [json.loads(line) for line in query_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    image_by_file = {}
    for item in captions:
        source_path = Path(item["path"])
        if not source_path.is_absolute(): source_path = dataset_root / source_path
        if not source_path.is_file():
            raise SystemExit(f"Dataset image not found: {source_path}")
        resource = f"{resource_prefix}{item['file']}"
        image_by_file[item["file"]] = resource
        item["resource"] = resource
        item["source_path"] = str(source_path)
    for query in queries:
        query["gold_resource"] = image_by_file.get(query.get("gold_image"), query.get("gold_image"))
    return captions, queries


def load_curated_general(dataset_root, resource_prefix):
    image_items = []
    resource_map = {}
    for resource, caption, tags in IMAGES:
        prefix = "image-test-"
        filename = resource[len(prefix):] if resource.startswith(prefix) else resource
        source_path = dataset_root / "images" / "general" / filename
        if not source_path.is_file():
            raise SystemExit(f"Dataset image not found: {source_path}")
        bundled_resource = f"{resource_prefix}{filename}"
        resource_map[resource] = bundled_resource
        image_items.append({
            "resource": bundled_resource,
            "caption": caption,
            "tags": tags,
            "subset": "general",
            "source_path": str(source_path),
        })
    query_items = [
        {
            "query_id": query_id,
            "text": text,
            "gold_resource": resource_map[gold_resource],
            "query_type": "图片搜索",
            "difficulty": "easy",
            "subset": "general",
        }
        for query_id, text, gold_resource in QUERIES
    ]
    return image_items, query_items


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-root", type=Path, help="embedding_test 数据集根目录")
    parser.add_argument("--output", type=Path, default=OUTPUT, help="向量 JSON 输出路径")
    parser.add_argument("--resource-prefix", default="image-test-", help="资源文件名前缀")
    parser.add_argument("--subset", choices=["general", "animals"], help="只使用数据集中的一个子集")
    parser.add_argument("--captions-file", type=Path, help="覆盖图片描述清单")
    parser.add_argument("--queries-file", type=Path, help="覆盖问题 JSONL 文件")
    return parser.parse_args()


def query_record(item, embedding):
    record = {key: value for key, value in item.items() if key != "source_path"}
    record["text"] = record.pop("query", record.get("text", ""))
    record["embedding"] = embedding
    return record


def image_record(item, embedding):
    record = {
        key: value
        for key, value in item.items()
        if key in {"resource", "caption", "tags", "subset"}
    }
    record["embedding"] = embedding
    return record


def main():
    args = parse_args()
    api_key = os.environ.get("DASHSCOPE_API_KEY", "").strip()
    if not api_key:
        raise SystemExit("DASHSCOPE_API_KEY is required")

    if args.dataset_root:
        if args.subset == "general" and not args.captions_file and not args.queries_file:
            image_items, query_items = load_curated_general(args.dataset_root, args.resource_prefix)
        else:
            image_items, query_items = load_dataset(args.dataset_root, args.resource_prefix, args.subset, args.captions_file, args.queries_file)
        image_contents = [{"image": image_data_url(Path(item["source_path"]))} for item in image_items]
        query_contents = [{"text": item.get("query", item.get("text", ""))} for item in query_items]
        contents = image_contents + query_contents
    else:
        image_items = [
            {"resource": resource, "caption": caption, "tags": tags, "subset": "general"}
            for resource, caption, tags in IMAGES
        ]
        query_items = [
            {
                "query_id": query_id,
                "text": text,
                "gold_resource": gold_resource,
                "query_type": "图片搜索",
                "difficulty": "easy",
                "subset": "general",
            }
            for query_id, text, gold_resource in QUERIES
        ]
        contents = [{"image": image_data_url(RESOURCES / item["resource"])} for item in image_items]
        contents.extend({"text": item["text"]} for item in query_items)

    # Keep each request small enough for the multimodal endpoint and preserve order.
    embeddings = []
    for start in range(0, len(contents), 8):
        embeddings.extend(request_embeddings(api_key, contents[start:start + 8]))
    dimensions = len(embeddings[0])
    if not dimensions or any(len(item) != dimensions for item in embeddings):
        raise SystemExit("Embedding dimensions mismatch")

    output = {
        "model": MODEL,
        "dimensions": dimensions,
        "images": [
            image_record(item, embeddings[index])
            for index, item in enumerate(image_items)
        ],
        "queries": [
            query_record(item, embeddings[len(image_items) + index])
            for index, item in enumerate(query_items)
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    print("generated {} images, {} queries, {} dimensions".format(len(image_items), len(query_items), dimensions))


if __name__ == "__main__":
    main()
