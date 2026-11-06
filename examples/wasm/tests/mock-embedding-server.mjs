import http from "node:http";

const port = Number(process.argv[2] ?? 18766);

function embeddingFor(input) {
  const text = String(input).toLowerCase();
  const vector = new Array(8).fill(0);
  const groups = [
    ["部署", "发布", "上线", "回滚", "deploy", "release", "rollback"],
    ["监控", "告警", "指标", "日志", "monitor", "alert", "metric"],
    ["备份", "恢复", "容灾", "backup", "restore", "recovery"],
    ["你好", "hello"]
  ];
  groups.forEach((words, index) => {
    vector[index] = words.reduce((score, word) => score + (text.includes(word) ? 1 : 0), 0);
  });
  if (vector.every(value => value === 0)) vector[4] = 1;
  vector[5] = Math.min(1, text.length / 500);
  vector[6] = text.length % 7 / 7;
  vector[7] = 0.25;
  const norm = Math.hypot(...vector) || 1;
  return vector.map(value => value / norm);
}

function imageEmbeddingFor(content, index) {
  const vector = new Array(8).fill(0);
  if (typeof content?.image === "string") {
    vector[Math.floor(index / 3) % 3] = 1;
  } else {
    const text = String(content?.text ?? "").toLowerCase();
    if (["羊", "汽车", "sheep", "car"].some(word => text.includes(word))) vector[0] = 1;
    else if (["input", "prompt", "llm", "output", "箭头"].some(word => text.includes(word))) vector[1] = 1;
    else if (["智能体", "记忆", "规划", "工具", "agent", "memory", "tool"].some(word => text.includes(word))) vector[2] = 1;
    else vector[0] = 1;
  }
  return vector;
}

function json(response, status, payload) {
  const body = JSON.stringify(payload);
  response.writeHead(status, {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Headers": "Content-Type, Authorization",
    "Access-Control-Allow-Methods": "POST, OPTIONS",
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": Buffer.byteLength(body)
  });
  response.end(body);
}

const server = http.createServer((request, response) => {
  if (request.method === "OPTIONS") {
    response.writeHead(204, {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Headers": "Content-Type, Authorization",
      "Access-Control-Allow-Methods": "POST, OPTIONS"
    });
    response.end();
    return;
  }
  const isTextRequest = request.url?.endsWith("/embeddings");
  const isImageRequest = request.url?.endsWith("/multimodal-embedding");
  if (request.method !== "POST" || (!isTextRequest && !isImageRequest)) {
    json(response, 404, { error: { message: "not found" } });
    return;
  }

  let body = "";
  request.setEncoding("utf8");
  request.on("data", chunk => { body += chunk; });
  request.on("end", () => {
    try {
      const payload = JSON.parse(body);
      if (isImageRequest) {
        const contents = payload?.input?.contents;
        if (!payload.model || !Array.isArray(contents) || contents.length === 0) {
          json(response, 400, { error: { message: "model and input.contents are required" } });
          return;
        }
        json(response, 200, {
          output: {
            embeddings: contents.map((content, index) => ({ index, embedding: imageEmbeddingFor(content, index) }))
          },
          usage: { input_tokens: contents.length }
        });
        return;
      }
      const inputs = Array.isArray(payload.input) ? payload.input : [payload.input];
      if (!payload.model || inputs.length === 0 || inputs.some(value => typeof value !== "string")) {
        json(response, 400, { error: { message: "model and string input are required" } });
        return;
      }
      json(response, 200, {
        object: "list",
        model: payload.model,
        data: inputs.map((input, index) => ({
          object: "embedding",
          index,
          embedding: embeddingFor(input)
        }))
      });
    } catch (error) {
      json(response, 400, { error: { message: error instanceof Error ? error.message : String(error) } });
    }
  });
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock embedding server: http://127.0.0.1:${port}/v1/{embeddings,multimodal-embedding}`);
});
