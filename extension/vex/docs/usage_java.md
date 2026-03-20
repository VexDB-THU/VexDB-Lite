# VexDB Java 使用指南

## 环境准备

### Maven 依赖

```xml
<dependency>
    <groupId>org.duckdb</groupId>
    <artifactId>duckdb_jdbc</artifactId>
    <version>1.2.1</version>
</dependency>
```

### Gradle 依赖

```groovy
implementation 'org.duckdb:duckdb_jdbc:1.2.1'
```

> 版本号请根据实际编译的 DuckDB 版本调整

---

## 基础用法

### 1. 连接并加载扩展

```java
import java.sql.*;
import org.duckdb.DuckDBConnection;

public class VexDBExample {
    public static void main(String[] args) throws Exception {
        // 内存数据库
        Connection conn = DriverManager.getConnection("jdbc:duckdb:");

        // 或持久化数据库
        // Connection conn = DriverManager.getConnection("jdbc:duckdb:my_vectors.db");

        Statement stmt = conn.createStatement();

        // 加载 VexDB 扩展
        stmt.execute("LOAD 'path/to/vex.duckdb_extension'");
    }
}
```

### 2. 创建表并插入数据

```java
Statement stmt = conn.createStatement();

// 创建表
stmt.execute("""
    CREATE TABLE documents (
        id INTEGER,
        title VARCHAR,
        embedding FLOATVECTOR(128)
    )
""");

// 插入数据
stmt.execute("""
    INSERT INTO documents VALUES
        (1, '机器学习入门', [0.1, 0.2, 0.3, 0.4]::FLOATVECTOR(4)),
        (2, '深度学习实战', [0.5, 0.6, 0.7, 0.8]::FLOATVECTOR(4)),
        (3, '自然语言处理', [0.9, 1.0, 1.1, 1.2]::FLOATVECTOR(4))
""");
```

### 3. 批量插入（PreparedStatement）

```java
conn.setAutoCommit(false);

PreparedStatement pstmt = conn.prepareStatement(
    "INSERT INTO documents VALUES (?, ?, ?::FLOATVECTOR(128))"
);

Random random = new Random(42);
for (int i = 0; i < 10000; i++) {
    float[] vec = new float[128];
    for (int j = 0; j < 128; j++) {
        vec[j] = (float) random.nextGaussian();
    }

    pstmt.setInt(1, i);
    pstmt.setString(2, "Document " + i);
    // DuckDB JDBC 接受数组的字符串表示
    pstmt.setString(3, arrayToString(vec));
    pstmt.addBatch();

    if (i % 1000 == 0) {
        pstmt.executeBatch();
    }
}
pstmt.executeBatch();
conn.commit();
conn.setAutoCommit(true);

// 辅助方法：将 float[] 转为 DuckDB LIST 字符串
static String arrayToString(float[] vec) {
    StringBuilder sb = new StringBuilder("[");
    for (int i = 0; i < vec.length; i++) {
        if (i > 0) sb.append(", ");
        sb.append(vec[i]);
    }
    sb.append("]");
    return sb.toString();
}
```

---

## 创建索引

### GraphIndex（HNSW 索引）

```java
// 基本创建
stmt.execute("CREATE INDEX idx ON documents USING GRAPH_INDEX (embedding)");

// 指定参数
stmt.execute("""
    CREATE INDEX idx ON documents USING GRAPH_INDEX (embedding)
    WITH (m=32, ef_construction=128, metric='l2')
""");

// 余弦距离
stmt.execute("""
    CREATE INDEX idx_cos ON documents USING GRAPH_INDEX (embedding)
    WITH (metric='cosine')
""");

// 内积
stmt.execute("""
    CREATE INDEX idx_ip ON documents USING GRAPH_INDEX (embedding)
    WITH (metric='ip')
""");

// PQ 量化
stmt.execute("""
    CREATE INDEX idx_pq ON documents USING GRAPH_INDEX (embedding)
    WITH (quantizer='pq', pq_m=16)
""");

// 向量去重
stmt.execute("""
    CREATE INDEX idx_dedup ON documents USING GRAPH_INDEX (embedding)
    WITH (max_dedup=8)
""");
```

### HybridIndex（分区过滤索引）

```java
stmt.execute("""
    CREATE TABLE products (
        id INTEGER,
        embedding FLOATVECTOR(128),
        category VARCHAR
    )
""");

// 创建混合索引（向量列 + 标量过滤列）
stmt.execute("""
    CREATE INDEX idx_hybrid ON products USING HYBRID_INDEX (embedding, category)
""");
```

---

## 向量搜索

### Top-K 最近邻搜索

```java
// 构造查询向量
float[] queryVec = {0.1f, 0.2f, 0.3f, 0.4f};
String vecLiteral = arrayToString(queryVec);

ResultSet rs = stmt.executeQuery(String.format("""
    SELECT id, title, l2_distance(embedding, %s::FLOATVECTOR(4)) AS dist
    FROM documents
    ORDER BY dist
    LIMIT 10
""", vecLiteral));

while (rs.next()) {
    int id = rs.getInt("id");
    String title = rs.getString("title");
    double dist = rs.getDouble("dist");
    System.out.printf("id=%d, title=%s, distance=%.4f%n", id, title, dist);
}
rs.close();
```

### 使用 PreparedStatement 查询

```java
// 使用参数化查询
PreparedStatement query = conn.prepareStatement("""
    SELECT id, title, l2_distance(embedding, ?::FLOATVECTOR(128)) AS dist
    FROM documents
    ORDER BY dist
    LIMIT ?
""");

query.setString(1, arrayToString(queryVec));
query.setInt(2, 10);

ResultSet rs = query.executeQuery();
while (rs.next()) {
    System.out.printf("id=%d, dist=%.4f%n", rs.getInt("id"), rs.getDouble("dist"));
}
```

### 使用操作符语法

```java
// <-> L2 距离
ResultSet rs = stmt.executeQuery(String.format("""
    SELECT id FROM documents
    ORDER BY embedding <-> %s::FLOATVECTOR(4)
    LIMIT 10
""", vecLiteral));

// <=> 余弦距离
ResultSet rs2 = stmt.executeQuery(String.format("""
    SELECT id FROM documents
    ORDER BY embedding <=> %s::FLOATVECTOR(4)
    LIMIT 10
""", vecLiteral));
```

### 带过滤的搜索（HybridIndex）

```java
PreparedStatement filteredQuery = conn.prepareStatement("""
    SELECT id, l2_distance(embedding, ?::FLOATVECTOR(128)) AS dist
    FROM products
    WHERE category = ?
    ORDER BY dist
    LIMIT 10
""");

filteredQuery.setString(1, arrayToString(queryVec));
filteredQuery.setString(2, "electronics");

ResultSet rs = filteredQuery.executeQuery();
while (rs.next()) {
    System.out.printf("id=%d, dist=%.4f%n", rs.getInt("id"), rs.getDouble("dist"));
}
```

---

## 运行时参数调整

```java
// 调整搜索扩展因子
stmt.execute("SET vex_ef_search = 200");

// 调整暴力搜索阈值
stmt.execute("SET vex_brute_force_threshold = 128");

// 查看当前设置
ResultSet rs = stmt.executeQuery("SELECT current_setting('vex_ef_search')");
if (rs.next()) {
    System.out.println("ef_search = " + rs.getString(1));
}

// 恢复默认值
stmt.execute("RESET vex_ef_search");
stmt.execute("RESET vex_brute_force_threshold");
```

---

## 索引诊断

```java
ResultSet rs = stmt.executeQuery("SELECT * FROM vex_index_info()");
while (rs.next()) {
    System.out.printf("index=%s, type=%s, table=%s, nodes=%d, dimension=%d%n",
        rs.getString("index_name"),
        rs.getString("index_type"),
        rs.getString("table_name"),
        rs.getLong("node_count"),
        rs.getInt("dimension"));
}
```

---

## 数据持久化

```java
// 创建持久化数据库
Connection conn = DriverManager.getConnection("jdbc:duckdb:vectors.db");
Statement stmt = conn.createStatement();
stmt.execute("LOAD 'path/to/vex.duckdb_extension'");

// 创建表和索引
stmt.execute("CREATE TABLE vectors (id INTEGER, vec FLOATVECTOR(128))");
stmt.execute("CREATE INDEX idx ON vectors USING GRAPH_INDEX (vec)");

// 插入数据 ...

// 显式 checkpoint（可选）
stmt.execute("CHECKPOINT");

// 关闭连接
conn.close();

// 重新打开 — 索引自动恢复
Connection conn2 = DriverManager.getConnection("jdbc:duckdb:vectors.db");
Statement stmt2 = conn2.createStatement();
stmt2.execute("LOAD 'path/to/vex.duckdb_extension'");

ResultSet rs = stmt2.executeQuery("""
    SELECT id FROM vectors
    ORDER BY l2_distance(vec, [0.0, 0.0, 0.0]::FLOATVECTOR(3))
    LIMIT 10
""");
```

---

## 完整示例：Spring Boot 集成

### 配置类

```java
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import javax.sql.DataSource;
import org.springframework.jdbc.datasource.DriverManagerDataSource;

@Configuration
public class DuckDBConfig {

    @Bean
    public DataSource dataSource() {
        DriverManagerDataSource ds = new DriverManagerDataSource();
        ds.setDriverClassName("org.duckdb.DuckDBDriver");
        ds.setUrl("jdbc:duckdb:app_vectors.db");
        return ds;
    }
}
```

### Repository

```java
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;
import java.util.List;
import java.util.Map;

@Repository
public class VectorSearchRepository {

    private final JdbcTemplate jdbc;

    public VectorSearchRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
        // 初始化扩展
        jdbc.execute("LOAD 'path/to/vex.duckdb_extension'");
    }

    public void createTable() {
        jdbc.execute("""
            CREATE TABLE IF NOT EXISTS embeddings (
                id INTEGER PRIMARY KEY,
                content VARCHAR,
                vec FLOATVECTOR(384),
                category VARCHAR
            )
        """);
        jdbc.execute("""
            CREATE INDEX IF NOT EXISTS idx_emb
            ON embeddings USING HYBRID_INDEX (vec, category)
            WITH (metric='cosine')
        """);
    }

    public void insert(int id, String content, float[] vec, String category) {
        jdbc.update(
            "INSERT INTO embeddings VALUES (?, ?, ?::FLOATVECTOR(384), ?)",
            id, content, arrayToString(vec), category
        );
    }

    public List<Map<String, Object>> search(float[] queryVec, String category, int limit) {
        return jdbc.queryForList("""
            SELECT id, content, cosine_distance(vec, ?::FLOATVECTOR(384)) AS distance
            FROM embeddings
            WHERE category = ?
            ORDER BY distance
            LIMIT ?
        """, arrayToString(queryVec), category, limit);
    }

    public void setEfSearch(int ef) {
        jdbc.execute("SET vex_ef_search = " + ef);
    }

    private static String arrayToString(float[] vec) {
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i < vec.length; i++) {
            if (i > 0) sb.append(", ");
            sb.append(vec[i]);
        }
        return sb.append("]").toString();
    }
}
```

### 使用

```java
@Service
public class SearchService {

    private final VectorSearchRepository repo;

    public SearchService(VectorSearchRepository repo) {
        this.repo = repo;
        repo.createTable();
        repo.setEfSearch(100);
    }

    public List<Map<String, Object>> semanticSearch(
            float[] queryEmbedding, String category, int topK) {
        return repo.search(queryEmbedding, category, topK);
    }
}
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
