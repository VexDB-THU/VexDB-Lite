#!/usr/bin/env python3
import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer


def vector_for(text, dimensions):
    text = text.lower()
    values = [0.05] * 8
    groups = [
        (("部署", "发布", "回滚", "生产环境"), 0),
        (("监控", "告警", "请求量", "耗时"), 1),
        (("备份", "恢复", "增量", "演练"), 2),
    ]
    matched = False
    for words, index in groups:
        if any(word in text for word in words):
            values[index] = 1.0
            matched = True
    if not matched:
        values[3] = 1.0
    return values + [0.0] * max(0, dimensions - len(values))


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if not self.path.endswith("/embeddings"):
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length))
            inputs = body.get("input", [])
            dimensions = 1536 if body.get("model") == "mock-1536" else 8
            if isinstance(inputs, str):
                inputs = [inputs]
            data = [
                {"object": "embedding", "index": index,
                 "embedding": vector_for(text, dimensions)}
                for index, text in enumerate(inputs)
            ]
            payload = json.dumps({"object": "list", "data": data}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except Exception as error:
            payload = json.dumps({"error": {"message": str(error)}}).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    def log_message(self, format, *args):
        print(format % args, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18765)
    args = parser.parse_args()
    server = HTTPServer(("127.0.0.1", args.port), Handler)
    print(f"mock embedding server: http://127.0.0.1:{args.port}/v1/embeddings", flush=True)
    server.serve_forever()
