# VexDB Python 使用指南

## 环境准备

### 安装 DuckDB Python 包

```bash
pip install duckdb
```

### 加载 VexDB 扩展

```python
import duckdb

conn = duckdb.connect()
conn.execute("LOAD 'path/to/vex.duckdb_extension'")
```

> 将 `path/to/vex.duckdb_extension` 替换为实际的扩展文件路径，例如 `build/release/extension/vex/vex.duckdb_extension`

---

## 基础用法

### 1. 创建表并插入向量数据

```python
import duckdb

conn = duckdb.connect()
conn.execute("LOAD 'path/to/vex.duckdb_extension'")

# 创建表
conn.execute("""
    CREATE TABLE documents (
        id INTEGER,
        title VARCHAR,
        embedding FLOATVECTOR(384)
    )
""")

# 插入数据
conn.execute("""
    INSERT INTO documents VALUES
        (1, '机器学习入门', [0.1, 0.2, 0.3]::FLOATVECTOR(3)),
        (2, '深度学习实战', [0.4, 0.5, 0.6]::FLOATVECTOR(3)),
        (3, '自然语言处理', [0.7, 0.8, 0.9]::FLOATVECTOR(3))
""")
```

### 2. 批量插入（使用 Python 列表）

```python
import random

# 生成随机向量数据
data = []
for i in range(10000):
    vec = [random.gauss(0, 1) for _ in range(128)]
    data.append((i, vec))

# 批量插入
conn.executemany(
    "INSERT INTO vectors VALUES (?, ?::FLOATVECTOR(128))",
    data
)
```

### 3. 使用 pandas DataFrame 批量插入

```python
import pandas as pd
import numpy as np

# 生成数据
n = 10000
dim = 128
ids = list(range(n))
embeddings = np.random.randn(n, dim).astype(np.float32)

# 转为 list of lists (DuckDB 需要)
vecs = [row.tolist() for row in embeddings]

df = pd.DataFrame({'id': ids, 'vec': vecs})

conn.execute("CREATE TABLE vectors (id INTEGER, vec FLOATVECTOR(128))")
conn.execute("""
    INSERT INTO vectors
    SELECT id, vec::FLOATVECTOR(128) FROM df
""")
```

---

## 创建索引

### GraphIndex（HNSW 索引）

```python
# 基本创建
conn.execute("CREATE INDEX idx ON vectors USING GRAPH_INDEX (vec)")

# 指定参数
conn.execute("""
    CREATE INDEX idx ON vectors USING GRAPH_INDEX (vec)
    WITH (m=32, ef_construction=128, metric='l2')
""")

# 使用余弦距离
conn.execute("""
    CREATE INDEX idx_cos ON vectors USING GRAPH_INDEX (vec)
    WITH (metric='cosine')
""")

# 使用内积
conn.execute("""
    CREATE INDEX idx_ip ON vectors USING GRAPH_INDEX (vec)
    WITH (metric='ip')
""")

# 启用 PQ 量化（降低内存占用）
conn.execute("""
    CREATE INDEX idx_pq ON vectors USING GRAPH_INDEX (vec)
    WITH (quantizer='pq', pq_m=16)
""")

# 启用向量去重
conn.execute("""
    CREATE INDEX idx_dedup ON vectors USING GRAPH_INDEX (vec)
    WITH (max_dedup=8)
""")
```

### HybridIndex（分区过滤索引）

```python
conn.execute("""
    CREATE TABLE products (
        id INTEGER,
        embedding FLOATVECTOR(128),
        category VARCHAR
    )
""")

# 创建混合索引（向量列 + 标量过滤列）
conn.execute("""
    CREATE INDEX idx_hybrid ON products USING HYBRID_INDEX (embedding, category)
""")
```

---

## 向量搜索

### Top-K 最近邻搜索

```python
# L2 距离搜索
results = conn.execute("""
    SELECT id, l2_distance(vec, [0.1, 0.2, 0.3]::FLOATVECTOR(3)) AS dist
    FROM documents
    ORDER BY dist
    LIMIT 10
""").fetchall()

for row in results:
    print(f"id={row[0]}, distance={row[1]:.4f}")
```

### 使用操作符语法

```python
# <-> L2 距离
results = conn.execute("""
    SELECT id FROM documents
    ORDER BY vec <-> [0.1, 0.2, 0.3]::FLOATVECTOR(3)
    LIMIT 10
""").fetchall()

# <=> 余弦距离
results = conn.execute("""
    SELECT id FROM documents
    ORDER BY vec <=> [0.1, 0.2, 0.3]::FLOATVECTOR(3)
    LIMIT 10
""").fetchall()
```

### 带过滤的向量搜索（HybridIndex）

```python
# 在指定分区内搜索（自动走索引）
results = conn.execute("""
    SELECT id, l2_distance(embedding, ?::FLOATVECTOR(128)) AS dist
    FROM products
    WHERE category = 'electronics'
    ORDER BY dist
    LIMIT 10
""", [query_vector]).fetchall()
```

### 返回 DataFrame

```python
df = conn.execute("""
    SELECT id, title, l2_distance(embedding, ?::FLOATVECTOR(384)) AS distance
    FROM documents
    ORDER BY distance
    LIMIT 10
""", [query_vec]).fetchdf()

print(df)
```

---

## 运行时参数调整

```python
# 调整搜索扩展因子（更高 = 更好的召回率，更慢）
conn.execute("SET vex_ef_search = 200")

# 调整暴力搜索阈值（节点数低于此值使用暴力搜索）
conn.execute("SET vex_brute_force_threshold = 128")

# 查看当前设置
ef = conn.execute("SELECT current_setting('vex_ef_search')").fetchone()[0]
print(f"ef_search = {ef}")

# 恢复默认值
conn.execute("RESET vex_ef_search")
conn.execute("RESET vex_brute_force_threshold")
```

---

## 索引诊断

```python
# 查看索引信息
info = conn.execute("SELECT * FROM vex_index_info()").fetchdf()
print(info)

# 查看索引列表
indexes = conn.execute("SELECT * FROM duckdb_indexes()").fetchdf()
print(indexes)
```

---

## 数据持久化

```python
# 使用持久化数据库
conn = duckdb.connect('my_vectors.db')
conn.execute("LOAD 'path/to/vex.duckdb_extension'")

# 创建表和索引
conn.execute("CREATE TABLE vectors (id INTEGER, vec FLOATVECTOR(128))")
conn.execute("CREATE INDEX idx ON vectors USING GRAPH_INDEX (vec)")

# 插入数据 ...

# 显式 checkpoint（可选，关闭连接时自动执行）
conn.execute("CHECKPOINT")

# 关闭连接
conn.close()

# 重新打开 — 索引自动恢复
conn = duckdb.connect('my_vectors.db')
conn.execute("LOAD 'path/to/vex.duckdb_extension'")
results = conn.execute("""
    SELECT id FROM vectors
    ORDER BY l2_distance(vec, ?::FLOATVECTOR(128))
    LIMIT 10
""", [query_vec]).fetchall()
```

---

## 完整示例：语义搜索

```python
import duckdb
import numpy as np

def main():
    conn = duckdb.connect('semantic_search.db')
    conn.execute("LOAD 'path/to/vex.duckdb_extension'")

    # 建表
    conn.execute("""
        CREATE TABLE IF NOT EXISTS articles (
            id INTEGER PRIMARY KEY,
            title VARCHAR,
            content VARCHAR,
            embedding FLOATVECTOR(384)
        )
    """)

    # 建索引
    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_articles
        ON articles USING GRAPH_INDEX (embedding)
        WITH (metric='cosine', m=32, ef_construction=128)
    """)

    # 调高搜索精度
    conn.execute("SET vex_ef_search = 100")

    # 模拟插入文章（实际中使用 embedding 模型生成向量）
    for i in range(1000):
        vec = np.random.randn(384).astype(np.float32).tolist()
        conn.execute(
            "INSERT INTO articles VALUES (?, ?, ?, ?::FLOATVECTOR(384))",
            [i, f"Article {i}", f"Content of article {i}", vec]
        )

    # 搜索
    query_vec = np.random.randn(384).astype(np.float32).tolist()
    results = conn.execute("""
        SELECT id, title, cosine_distance(embedding, ?::FLOATVECTOR(384)) AS dist
        FROM articles
        ORDER BY dist
        LIMIT 5
    """, [query_vec]).fetchdf()

    print(results)
    conn.close()

if __name__ == '__main__':
    main()
```

---

## 距离函数参考

| 函数 | 操作符 | 说明 | 索引 metric |
|------|--------|------|-------------|
| `l2_distance(a, b)` | `<->` | 欧氏距离 | `'l2'`（默认） |
| `cosine_distance(a, b)` | `<=>` | 余弦距离 | `'cosine'` |
| `inner_product(a, b)` | - | 内积（DESC排序） | `'ip'` |

## 索引参数参考

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `m` | 16 | 每个节点的最大连接数 |
| `ef_construction` | 64 | 构建时搜索扩展因子 |
| `metric` | `'l2'` | 距离度量: `'l2'`, `'cosine'`, `'ip'` |
| `quantizer` | 无 | 量化器: `'pq'` |
| `pq_m` | 自动 | PQ 子量化器数量 |
| `max_dedup` | 8 | 单节点最大去重行数（1=禁用） |

## 运行时参数参考

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `vex_ef_search` | 40 | 搜索扩展因子 |
| `vex_brute_force_threshold` | 64 | 暴力搜索切换阈值 |
