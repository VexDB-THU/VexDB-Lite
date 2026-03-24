# VexDB-Lite Rust 使用指南

## 安装

```toml
# Cargo.toml
[dependencies]
vexdb-lite = { path = "path/to/vexdb-lite-rs" }
```

VEX 已内置，无需手动加载扩展。

## 快速开始

```rust
use vexdb_lite::Connection;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let conn = Connection::open_in_memory()?;

    // 创建向量表
    conn.execute_batch("
        CREATE TABLE docs (id INTEGER, vec FLOAT[384]);
        CREATE INDEX idx ON docs USING GRAPH_INDEX(vec) WITH (metric='cosine');
    ")?;

    // 插入向量
    conn.execute("INSERT INTO docs VALUES (1, [0.1, 0.2, ...]::FLOAT[384])", [])?;

    // 语义搜索
    let id: i32 = conn.query_row(
        "SELECT id FROM docs ORDER BY cosine_distance(vec, [0.3, ...]::FLOAT[384]) LIMIT 1",
        [], |row| row.get(0)
    )?;

    Ok(())
}
```

## 距离函数

```rust
// L2 距离
let d: f64 = conn.query_row(
    "SELECT l2_distance([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3])", [], |r| r.get(0))?;

// Cosine 距离
let d: f64 = conn.query_row(
    "SELECT cosine_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3])", [], |r| r.get(0))?;

// 内积
let d: f64 = conn.query_row(
    "SELECT inner_product([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3])", [], |r| r.get(0))?;
```

## 索引类型

```rust
// GRAPH_INDEX
conn.execute_batch("CREATE INDEX idx ON t USING GRAPH_INDEX(v) WITH (metric='cosine')")?;

// HYBRID_INDEX（向量 + 标量过滤）
conn.execute_batch("CREATE INDEX idx ON t USING HYBRID_INDEX(v, category)")?;

// PQ 量化
conn.execute_batch("CREATE INDEX idx ON t USING GRAPH_INDEX(v) WITH (quantizer='pq', pq_m=16)")?;
```

## 持久化

```rust
// 文件数据库
let conn = Connection::open("vectors.db")?;
conn.execute_batch("
    CREATE TABLE t (id INT, v FLOAT[128]);
    CREATE INDEX idx ON t USING GRAPH_INDEX(v);
")?;
// ... 插入数据 ...
conn.execute("CHECKPOINT", [])?;
drop(conn);

// 重新打开，索引自动恢复
let conn = Connection::open("vectors.db")?;
```

## 运行时配置

```rust
conn.execute("SET vex_ef_search = 100", [])?;
conn.execute("SET vex_brute_force_threshold = 64", [])?;
```

## 构建要求

- Rust 1.70+
- CMake 3.14+
- macOS: `MACOSX_DEPLOYMENT_TARGET=11.0`
