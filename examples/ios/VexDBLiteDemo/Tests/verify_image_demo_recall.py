#!/usr/bin/env python3
"""Verify that each bundled image query ranks its expected general image first."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = ROOT / "Resources" / "image-demo-vectors.json"

def squared_l2(left, right):
    return sum((a - b) ** 2 for a, b in zip(left, right))


def main():
    payload = json.loads(PAYLOAD.read_text(encoding="utf-8"))
    images = payload["images"]
    queries = payload["queries"]
    dimensions = payload["dimensions"]

    assert len(images) == 20, "expected 20 bundled images"
    assert len(queries) == 3, "expected 3 bundled queries"
    assert dimensions == 1152, "unexpected embedding dimensions"
    assert all(len(image["embedding"]) == dimensions for image in images)
    assert all(len(query["embedding"]) == dimensions for query in queries)

    for query_index, query in enumerate(queries, 1):
        ranked = sorted(
            images,
            key=lambda image: squared_l2(query["embedding"], image["embedding"]),
        )
        expected = query["gold_resource"]
        actual = ranked[0]["resource"]
        assert actual == expected, "query {} top one mismatch: expected {}, got {}".format(
            query_index, expected, actual
        )
        print("query {} top 5: {}".format(
            query_index, ", ".join(image["resource"] for image in ranked[:5])
        ))

    print("verified 20 images, 3 queries, recall@1 100%")


if __name__ == "__main__":
    main()
