# VexDB-Lite Java 使用指南

## 安装

从 [GitHub Release](https://github.com/VexDB-Beijing/VexDB-Lite/releases) 下载 `vexdb-lite-1.5.0.jar`。

```groovy
// Gradle - 本地 JAR
dependencies {
    implementation files('libs/vexdb-lite-1.5.0.jar')
}
```

VEX 扩展已内置，无需手动加载。

---

## 快速开始

```java
import java.sql.*;

public class VexDemo {
    public static void main(String[] args) throws Exception {
        Class.forName("org.duckdb.DuckDBDriver");
        Connection con = DriverManager.getConnection("jdbc:duckdb:");
        Statement stmt = con.createStatement();

        // 创建向量表
        stmt.execute("CREATE TABLE documents (id INTEGER, title VARCHAR, embedding FLOAT[384])");

        // 插入数据
        stmt.execute("""
            INSERT INTO documents VALUES
            (1, '机器学习入门', [0.1, 0.2, 0.3]::FLOAT[3]),
            (2, '深度学习实战', [0.4, 0.5, 0.6]::FLOAT[3])
        """);

        // 创建 HNSW 索引
        stmt.execute("CREATE INDEX idx ON documents USING GRAPH_INDEX(embedding)");

        // 语义搜索
        ResultSet rs = stmt.executeQuery("""
            SELECT id, title
            FROM documents
            ORDER BY cosine_distance(embedding, [0.15, 0.25, 0.35]::FLOAT[3])
            LIMIT 10
        """);

        while (rs.next()) {
            System.out.printf("id=%d, title=%s%n", rs.getInt(1), rs.getString(2));
        }

        con.close();
    }
}
```

---

## 索引类型

### GRAPH_INDEX

```java
// L2 距离（默认）
stmt.execute("CREATE INDEX idx ON t USING GRAPH_INDEX(v)");

// Cosine 距离
stmt.execute("CREATE INDEX idx ON t USING GRAPH_INDEX(v) WITH (metric='cosine')");

// PQ 量化
stmt.execute("CREATE INDEX idx ON t USING GRAPH_INDEX(v) WITH (quantizer='pq', pq_m=16)");
```

### HYBRID_INDEX

```java
stmt.execute("CREATE TABLE products (id INT, category VARCHAR, vec FLOAT[128])");
stmt.execute("CREATE INDEX idx ON products USING HYBRID_INDEX(vec, category)");

// 分类内向量搜索
ResultSet rs = stmt.executeQuery("""
    SELECT id FROM products
    WHERE category = '数码'
    ORDER BY cosine_distance(vec, [...]::FLOAT[128])
    LIMIT 10
""");
```

---

## 持久化

```java
// 使用文件路径
Connection con = DriverManager.getConnection("jdbc:duckdb:/path/to/vectors.db");
// ... 创建表、索引、插入数据 ...
con.createStatement().execute("CHECKPOINT");
con.close();

// 下次打开，索引自动恢复
Connection con2 = DriverManager.getConnection("jdbc:duckdb:/path/to/vectors.db");
```

---

## 距离函数

```java
// L2 距离
rs = stmt.executeQuery("SELECT l2_distance([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3])");

// Cosine 距离
rs = stmt.executeQuery("SELECT cosine_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3])");

// 内积
rs = stmt.executeQuery("SELECT inner_product([1,2,3]::FLOAT[3], [4,5,6]::FLOAT[3])");
```

## 运行时配置

```java
stmt.execute("SET vex_ef_search = 100");
stmt.execute("SET vex_brute_force_threshold = 64");
```
