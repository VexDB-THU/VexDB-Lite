# VexDB-Lite Node.js 使用指南

## 安装

```bash
npm install vexdb-lite
```

VEX 已内置，无需手动加载扩展。

## 快速开始

```javascript
const duckdb = require('vexdb-lite');
const db = new duckdb.Database(':memory:');

// 创建向量表 + 索引
db.run("CREATE TABLE docs (id INT, vec FLOAT[384])");
db.run("CREATE INDEX idx ON docs USING GRAPH_INDEX(vec) WITH (metric='cosine')");

// 插入向量
db.run("INSERT INTO docs VALUES (1, [0.1, 0.2, ...]::FLOAT[384])");

// 语义搜索
db.all(`
    SELECT id FROM docs
    ORDER BY cosine_distance(vec, [0.3, 0.4, ...]::FLOAT[384])
    LIMIT 10
`, (err, rows) => {
    console.log(rows);
});
```

## Promise 风格

```javascript
const duckdb = require('vexdb-lite');

function query(db, sql) {
    return new Promise((resolve, reject) => {
        db.all(sql, (err, rows) => err ? reject(err) : resolve(rows));
    });
}

function exec(db, sql) {
    return new Promise((resolve, reject) => {
        db.run(sql, (err) => err ? reject(err) : resolve());
    });
}

async function main() {
    const db = new duckdb.Database(':memory:');

    await exec(db, "CREATE TABLE docs (id INT, vec FLOAT[384])");
    await exec(db, "CREATE INDEX idx ON docs USING GRAPH_INDEX(vec)");
    await exec(db, "INSERT INTO docs VALUES (1, [0.1, 0.2, ...]::FLOAT[384])");

    const results = await query(db, `
        SELECT id FROM docs
        ORDER BY l2_distance(vec, [0.3, ...]::FLOAT[384])
        LIMIT 10
    `);
    console.log(results);

    db.close();
}

main().catch(console.error);
```

## 索引类型

```javascript
// GRAPH_INDEX
await exec(db, "CREATE INDEX idx ON t USING GRAPH_INDEX(v) WITH (metric='cosine')");

// HYBRID_INDEX（向量 + 标量过滤）
await exec(db, "CREATE INDEX idx ON t USING HYBRID_INDEX(v, category)");

// PQ 量化
await exec(db, "CREATE INDEX idx ON t USING GRAPH_INDEX(v) WITH (quantizer='pq', pq_m=16)");
```

## 距离函数

```javascript
// L2 距离
await query(db, "SELECT l2_distance([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3]) AS d");

// Cosine 距离
await query(db, "SELECT cosine_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3]) AS d");

// 内积
await query(db, "SELECT inner_product([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3]) AS d");
```

## 持久化

```javascript
// 文件数据库
const db = new duckdb.Database('/path/to/vectors.db');
// ... 创建表、索引、插入数据 ...
await exec(db, "CHECKPOINT");
db.close();

// 重新打开，索引自动恢复
const db2 = new duckdb.Database('/path/to/vectors.db');
```

## 运行时配置

```javascript
await exec(db, "SET vex_ef_search = 100");
await exec(db, "SET vex_brute_force_threshold = 64");
```

## 要求

- Node.js 18+
