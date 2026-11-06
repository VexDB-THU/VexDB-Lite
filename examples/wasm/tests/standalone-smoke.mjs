import assert from "node:assert/strict";
import { readFile, stat } from "node:fs/promises";
import { gunzipSync } from "node:zlib";

const file = new URL("../dist/vexdb-lite-demo.html", import.meta.url);
const [html, info] = await Promise.all([readFile(file, "utf8"), stat(file)]);

assert.ok(info.size < 2_400_000, `单文件 HTML 超出 2.40 MB 体积预算：${info.size} 字节`);
assert.match(html, /globalThis\.__VEXDB_STANDALONE__/);
assert.match(html, /"imageDemo":/);
assert.match(html, /data:image\/jpeg;base64/);
assert.doesNotMatch(html, /data:image\/webp;base64/);
assert.doesNotMatch(html, /data:image\/png;base64/);
assert.doesNotMatch(html, /src="\.\/app\.js"/);
assert.doesNotMatch(html, /href="\.\/styles\.css"/);

const embeddedMatch = html.match(/globalThis\.__VEXDB_STANDALONE__ = (\{[^\n]+\});/);
assert.ok(embeddedMatch, "没有找到单文件资源清单");
const embedded = JSON.parse(embeddedMatch[1]);
assert.equal(embedded.compression, "gzip");
for (const [key, source] of [
  ["wasm", "vexdb-wasm.wasm"],
  ["demo", "embedding-demo-vectors.json"],
  ["document", "embedding-demo.txt"],
  ["imageDemo", "image-demo-vectors.json"]
]) {
  const [actual, expected] = await Promise.all([
    Promise.resolve(gunzipSync(Buffer.from(embedded[key], "base64"))),
    readFile(new URL(`../dist/${source}`, import.meta.url))
  ]);
  assert.deepEqual(actual, expected, `${source} 单文件压缩往返后内容变化`);
}

console.log(`STANDALONE HTML PASS: ${(info.size / 1024 / 1024).toFixed(2)} MB, no external assets`);
