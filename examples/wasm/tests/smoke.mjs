import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import createVexDBWasm from "../dist/vexdb-wasm.js";

const demo = JSON.parse(await readFile(new URL("../dist/embedding-demo-vectors.json", import.meta.url)));
const module = await createVexDBWasm({
  locateFile: file => new URL(`../dist/${file}`, import.meta.url).pathname
});
const scope = "bundled_text";
const reset = module.cwrap("vexdb_wasm_reset_scope", "number", ["string", "number", "string"]);
const insert = module.cwrap("vexdb_wasm_insert_scope", "number", ["string", "number", "string", "string", "string"]);
const search = module.cwrap("vexdb_wasm_search_scope", "string", ["string", "string", "number"]);
const count = module.cwrap("vexdb_wasm_count_scope", "number", ["string"]);
const lastError = module.cwrap("vexdb_wasm_last_error", "string", []);

assert.equal(reset(scope, demo.dimensions, "cosine"), 0, lastError());
for (const chunk of demo.chunks) {
  assert.equal(insert(scope, chunk.id, JSON.stringify(chunk.embedding), `文本片段 ${chunk.id}`, chunk.text), 0,
    lastError());
}
assert.equal(count(scope), demo.chunks.length);

const result = JSON.parse(search(scope, JSON.stringify(demo.queries[0].embedding), 5));
assert.equal(result.ok, true);
assert.equal(result.rows.length, 5);
assert.equal(result.rows[0].rowid, 1);
assert.match(result.plan, /VIRTUAL TABLE INDEX 1/);
assert.match(result.plan, /bundled_text_vectors/);
console.log(`WASM SMOKE PASS: rows=${count(scope)} top1=${result.rows[0].rowid} plan=${result.plan}`);
