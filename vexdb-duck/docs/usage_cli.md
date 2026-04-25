# VexDB-Lite CLI 使用指南

## 安装

从 [GitHub Release](https://github.com/VexDB-Beijing/VexDB-Lite/releases) 下载对应平台的 CLI：

```bash
# macOS
curl -LO https://github.com/VexDB-Beijing/VexDB-Lite/releases/download/v0.0.1/vexdb-lite-cli-macos
chmod +x vexdb-lite-cli-macos
mv vexdb-lite-cli-macos /usr/local/bin/vexdb-lite
```

所有向量搜索功能已内置，无需额外配置。

---

## 快速开始

### 交互模式

```bash
vexdb-lite
```

进入 SQL 交互终端：

```sql
D CREATE TABLE docs (id INT, title VARCHAR, vec FLOAT[384]);
D CREATE INDEX idx ON docs USING GRAPH_INDEX(vec) WITH (metric='cosine');
D INSERT INTO docs VALUES (1, '机器学习入门', [0.1, 0.2, ...]::FLOAT[384]);
D SELECT title FROM docs ORDER BY cosine_distance(vec, [0.3, ...]::FLOAT[384]) LIMIT 10;
```

### 持久化数据库

```bash
# 打开（或创建）文件数据库
vexdb-lite vectors.db
```

数据和索引持久化到 `vectors.db`，下次打开自动恢复。

### 单行命令

```bash
# -c 执行单条 SQL
vexdb-lite -c "SELECT l2_distance([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3])"

# 对文件数据库执行
vexdb-lite vectors.db -c "SELECT COUNT(*) FROM docs"
```

### 管道模式

```bash
# 从 stdin 读取 SQL
echo "SELECT cosine_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3]);" | vexdb-lite

# 从文件执行
vexdb-lite vectors.db < init.sql

# 结合其他工具
cat queries.sql | vexdb-lite vectors.db > results.txt
```

---

## 常用操作

### 建表 + 建索引

```bash
vexdb-lite vectors.db -c "
CREATE TABLE products (
    id INTEGER,
    name VARCHAR,
    category VARCHAR,
    embedding FLOAT[128]
);

CREATE INDEX idx ON products USING GRAPH_INDEX(embedding)
    WITH (metric='cosine', m=16, ef_construction=200);

CREATE INDEX hidx ON products USING HYBRID_INDEX(embedding, category);
"
```

### 导入数据

```bash
# 从 CSV 导入
vexdb-lite vectors.db -c "
CREATE TABLE raw AS SELECT * FROM read_csv('data.csv');
"

# 从 Parquet 导入
vexdb-lite vectors.db -c "
CREATE TABLE raw AS SELECT * FROM read_parquet('embeddings.parquet');
"

# 从 JSON 导入
vexdb-lite vectors.db -c "
CREATE TABLE raw AS SELECT * FROM read_json('data.json');
"
```

### 搜索

```bash
# 语义搜索
vexdb-lite vectors.db -c "
SELECT id, name, cosine_distance(embedding, [0.1, 0.2, ...]::FLOAT[128]) AS dist
FROM products
ORDER BY dist
LIMIT 10;
"

# 混合过滤
vexdb-lite vectors.db -c "
SELECT id, name
FROM products
WHERE category = '数码'
ORDER BY cosine_distance(embedding, [0.1, 0.2, ...]::FLOAT[128])
LIMIT 10;
"
```

### 导出结果

```bash
# 导出为 CSV
vexdb-lite vectors.db -c "
COPY (SELECT id, name FROM products LIMIT 100) TO 'output.csv' (HEADER);
"

# 导出为 Parquet
vexdb-lite vectors.db -c "
COPY (SELECT * FROM products) TO 'products.parquet' (FORMAT PARQUET);
"

# JSON 输出模式
vexdb-lite vectors.db -json -c "SELECT id, name FROM products LIMIT 5"
```

---

## 运行时配置

```bash
vexdb-lite vectors.db -c "
SET vex_ef_search = 100;            -- 搜索精度（越高越准）
SET vex_brute_force_threshold = 64; -- 小数据暴力搜索阈值
"
```

---

## 诊断

```bash
# 查看索引状态
vexdb-lite vectors.db -c "SELECT * FROM vex_index_info()"

# 查看数据库大小
vexdb-lite vectors.db -c "
SELECT table_name, estimated_size, column_count
FROM duckdb_tables()
"

# 查看执行计划（确认走了索引）
vexdb-lite vectors.db -c "
EXPLAIN SELECT id FROM products
ORDER BY cosine_distance(embedding, [0.1, ...]::FLOAT[128])
LIMIT 10
"
```

---

## 脚本化

```bash
#!/bin/bash
# batch_search.sh - 批量向量搜索

DB="vectors.db"
QUERY_VEC="[0.1, 0.2, 0.3]"

vexdb-lite "$DB" -c "
SET vex_ef_search = 200;
SELECT id, name,
       cosine_distance(embedding, ${QUERY_VEC}::FLOAT[3]) AS score
FROM products
ORDER BY score
LIMIT 20;
"
```

---

## 输出格式

```bash
vexdb-lite -box      # 表格（默认）
vexdb-lite -csv      # CSV
vexdb-lite -json     # JSON
vexdb-lite -line     # 每行一个字段
vexdb-lite -markdown # Markdown 表格
```
