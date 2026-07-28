import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import createVexDBWasm from "../dist/vexdb-wasm.js";

const demo = JSON.parse(await readFile(new URL("../dist/image-demo-vectors.json", import.meta.url)));
const module = await createVexDBWasm({
  locateFile: file => new URL(`../dist/${file}`, import.meta.url).pathname
});
const scope = "bundled_media";
const reset = module.cwrap("vexdb_wasm_reset_scope", "number", ["string", "number", "string"]);
const insert = module.cwrap("vexdb_wasm_insert_scope", "number", ["string", "number", "string", "string", "string"]);
const search = module.cwrap("vexdb_wasm_search_scope", "string", ["string", "string", "number"]);
const lastError = module.cwrap("vexdb_wasm_last_error", "string", []);

assert.equal(reset(scope, demo.dimensions, "l2"), 0, lastError());
for (const [index, item] of demo.images.entries()) {
  assert.equal(insert(scope, index + 1, JSON.stringify(item.embedding), item.resource, "内置图片"), 0,
    lastError());
}

const resources = new Set(demo.images.map(item => item.resource));
let top1Hits = 0;
let top5Hits = 0;
for (const [index, query] of demo.queries.entries()) {
  assert.ok(resources.has(query.gold_resource), `query ${query.query_id ?? index + 1} refers to a missing image`);
  const result = JSON.parse(search(scope, JSON.stringify(query.embedding), 5));
  assert.equal(result.ok, true);
  assert.equal(result.rows.length, Math.min(5, demo.images.length));
  if (result.rows[0].title === query.gold_resource) top1Hits += 1;
  if (result.rows.some(row => row.title === query.gold_resource)) top5Hits += 1;
  assert.match(result.plan, /VIRTUAL TABLE INDEX 1/);
  assert.match(result.plan, /bundled_media_vectors/);
}

console.log(`WASM IMAGE PASS: images=${demo.images.length} queries=${demo.queries.length} dimensions=${demo.dimensions} metric=l2 topk=5 recall@1=${(top1Hits / demo.queries.length).toFixed(3)} recall@5=${(top5Hits / demo.queries.length).toFixed(3)}`);
