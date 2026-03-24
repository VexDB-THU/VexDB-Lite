# VexDB-Lite Python 使用指南

## 安装

```bash
# 从 GitHub Release 下载 wheel 安装
pip install vexdb_lite-1.5.0-cp312-cp312-macosx_11_0_arm64.whl

# 或（即将发布到 PyPI）
# pip install vexdb-lite
```

VEX 扩展已内置，无需手动 LOAD。`import duckdb` 即可使用全部向量搜索功能。

---

## 快速开始

```python
import duckdb

con = duckdb.connect()

# 创建向量表
con.execute("""
    CREATE TABLE documents (
        id INTEGER,
        title VARCHAR,
        embedding FLOAT[384]
    )
""")

# 插入数据
con.execute("""
    INSERT INTO documents VALUES
        (1, '机器学习入门', [0.1, 0.2, 0.3]::FLOAT[3]),
        (2, '深度学习实战', [0.4, 0.5, 0.6]::FLOAT[3]),
        (3, '自然语言处理', [0.7, 0.8, 0.9]::FLOAT[3])
""")

# 创建 HNSW 索引
con.execute("CREATE INDEX idx ON documents USING GRAPH_INDEX(embedding)")

# 语义搜索
results = con.execute("""
    SELECT id, title
    FROM documents
    ORDER BY cosine_distance(embedding, [0.15, 0.25, 0.35]::FLOAT[3])
    LIMIT 10
""").fetchall()
```

---

## 批量导入（PyArrow 零拷贝）

```python
import numpy as np
import pyarrow as pa

# 生成 10 万条 128 维向量
data = np.random.randn(100000, 128).astype(np.float32)

ids = pa.array(range(len(data)), type=pa.int32())
vecs = pa.array([row.tolist() for row in data], type=pa.list_(pa.float32()))
table = pa.table({"id": ids, "v": vecs})

con.execute("CREATE TABLE vectors (id INTEGER, v FLOAT[128])")
con.execute("INSERT INTO vectors SELECT * FROM table")
# 0.3 秒完成（34,868 vec/s）
```

---

## 创建索引

### GRAPH_INDEX（HNSW 图索引）

```python
# L2 距离（默认）
con.execute("CREATE INDEX idx ON vectors USING GRAPH_INDEX(v)")

# Cosine 距离（自动归一化）
con.execute("CREATE INDEX idx ON vectors USING GRAPH_INDEX(v) WITH (metric='cosine')")

# 内积
con.execute("CREATE INDEX idx ON vectors USING GRAPH_INDEX(v) WITH (metric='ip')")

# 自定义参数
con.execute("""
    CREATE INDEX idx ON vectors USING GRAPH_INDEX(v)
    WITH (metric='cosine', m=32, ef_construction=200)
""")

# PQ 量化（省内存 4-8 倍）
con.execute("""
    CREATE INDEX idx ON vectors USING GRAPH_INDEX(v)
    WITH (metric='l2', quantizer='pq', pq_m=16)
""")

# 向量去重
con.execute("""
    CREATE INDEX idx ON vectors USING GRAPH_INDEX(v)
    WITH (max_dedup=8)
""")
```

### HYBRID_INDEX（混合索引）

```python
con.execute("""
    CREATE TABLE products (id INT, category VARCHAR, vec FLOAT[128])
""")
con.execute("CREATE INDEX idx ON products USING HYBRID_INDEX(vec, category)")

# 在某个分类内做向量搜索
results = con.execute("""
    SELECT id FROM products
    WHERE category = '数码'
    ORDER BY cosine_distance(vec, [...]::FLOAT[128])
    LIMIT 10
""").fetchall()
```

---

## 距离函数

| 函数 | 操作符 | 说明 | 索引 metric |
|------|--------|------|-------------|
| `l2_distance(a, b)` | `<->` | 欧氏距离 | `'l2'`（默认） |
| `cosine_distance(a, b)` | `<=>` | 余弦距离 | `'cosine'` |
| `inner_product(a, b)` | — | 内积 | `'ip'` |

```python
con.execute("SELECT l2_distance([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3])")       # 5.196
con.execute("SELECT cosine_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3])")   # 1.0
con.execute("SELECT inner_product([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3])")     # 32.0
```

---

## 运行时参数

```python
con.execute("SET vex_ef_search = 100")             # 搜索精度（默认 40，越高越准）
con.execute("SET vex_brute_force_threshold = 64")   # 小数据暴力搜索阈值
```

---

## 持久化

```python
con = duckdb.connect("vectors.db")
con.execute("CREATE TABLE t (id INT, v FLOAT[128])")
con.execute("CREATE INDEX idx ON t USING GRAPH_INDEX(v)")
# ... 插入数据 ...
con.execute("CHECKPOINT")  # 强制写盘
con.close()

# 下次打开，索引自动恢复
con = duckdb.connect("vectors.db")
results = con.execute("SELECT id FROM t ORDER BY l2_distance(v, [...]) LIMIT 10").fetchall()
```

---

## 索引参数参考

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `m` | 16 | 每个节点的最大连接数 |
| `ef_construction` | 64 | 构建时搜索扩展因子 |
| `metric` | `'l2'` | 距离度量: `'l2'`, `'cosine'`, `'ip'` |
| `quantizer` | 无 | 量化器: `'pq'` |
| `pq_m` | 自动 | PQ 子量化器数量 |
| `max_dedup` | 8 | 单节点最大去重行数（1=禁用） |

## 性能参考（10K 向量, 128 维）

| 指标 | HNSW | PQ |
|------|------|-----|
| 插入速度 | 34,868 vec/s | — |
| 索引构建 | 8.98s | 6.71s |
| Recall@10 | 91% | 69% |
| QPS | 505 | 16 |
| 平均延迟 | 2.0ms | — |
