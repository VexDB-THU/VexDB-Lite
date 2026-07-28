import assert from "node:assert/strict";
import createVexDBWasm from "../dist/vexdb-wasm.js";

const module = await createVexDBWasm({
  locateFile: file => new URL(`../dist/${file}`, import.meta.url).pathname
});
const reset = module.cwrap("vexdb_wasm_reset_scope", "number", ["string", "number", "string"]);
const insert = module.cwrap("vexdb_wasm_insert_scope", "number", ["string", "number", "string", "string", "string"]);
const search = module.cwrap("vexdb_wasm_search_scope", "string", ["string", "string", "number"]);
const count = module.cwrap("vexdb_wasm_count_scope", "number", ["string"]);
const lastError = module.cwrap("vexdb_wasm_last_error", "string", []);

const scopes = [
  ["bundled_text", "bundled_text_vectors", "cosine", [[1, 0, 0, 0], [0.9, 0.1, 0, 0]]],
  ["user_text", "user_text_vectors", "cosine", [[0, 1, 0, 0]]],
  ["bundled_media", "bundled_media_vectors", "l2", [[0, 0, 1, 0], [0, 0, 0.9, 0.1], [0, 0, 0, 1]]],
  ["user_media", "user_media_vectors", "l2", [[0.5, 0.5, 0, 0]]]
];

for (const [scope, , metric, vectors] of scopes) {
  assert.equal(reset(scope, 4, metric), 0, `${scope}: ${lastError()}`);
  for (const [index, vector] of vectors.entries()) {
    assert.equal(insert(scope, index + 1, JSON.stringify(vector), `${scope}-${index + 1}`, "test"), 0,
      `${scope}: ${lastError()}`);
  }
}

assert.deepEqual(scopes.map(([scope]) => count(scope)), [2, 1, 3, 1]);

assert.equal(reset("user_media", 4, "l2"), 0, lastError());
for (const [index, vector] of [[0.4, 0.6, 0, 0], [0.3, 0.7, 0, 0]].entries()) {
  assert.equal(insert("user_media", index + 1, JSON.stringify(vector), `rebuilt-${index + 1}`, "test"), 0,
    lastError());
}

assert.deepEqual(scopes.map(([scope]) => count(scope)), [2, 1, 3, 2]);

for (const [scope, table, , vectors] of scopes) {
  const result = JSON.parse(search(scope, JSON.stringify(vectors[0]), 1));
  assert.equal(result.ok, true);
  assert.equal(result.rows.length, 1);
  assert.match(result.plan, new RegExp(`${table} VIRTUAL TABLE INDEX 1`));
}

assert.equal(reset("unknown", 4, "l2"), 21);
assert.match(lastError(), /未知的向量索引类型/);

console.log("WASM SCOPE ISOLATION PASS: one module, one SQLite, four GRAPH_INDEX tables");
