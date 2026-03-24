# VexDB-Lite iOS 使用指南

## 获取静态库

从 [GitHub Release](https://github.com/VexDB-Beijing/VexDB-Lite/releases) 下载：

- **真机**: `libvexdb-ios-arm64.tar.gz`
- **模拟器 (Apple Silicon)**: `libvexdb-ios-sim-arm64.tar.gz`

```bash
tar xzf libvexdb-ios-arm64.tar.gz
# 产出:
#   libvexdb.a     # 静态库（~11MB 链接后）
#   include/       # 头文件（duckdb.h, duckdb.hpp）
```

## Xcode 项目集成

### 1. 添加库文件

将 `libvexdb.a` 和 `include/` 拖入 Xcode 项目，或在 Build Settings 中配置：

```
Header Search Paths: $(PROJECT_DIR)/vexdb/include
Library Search Paths: $(PROJECT_DIR)/vexdb
Other Linker Flags: -lvexdb -lc++ -framework Foundation -framework Security
```

### 2. Swift 桥接

创建 `VexDB-Bridging-Header.h`：

```c
#import "duckdb.h"
```

### 3. Swift 封装示例

```swift
import Foundation

class VexDB {
    private var db: duckdb_database?
    private var con: duckdb_connection?

    init(path: String? = nil) {
        if let path = path {
            duckdb_open(path, &db)
        } else {
            duckdb_open(nil, &db)  // 内存数据库
        }
        duckdb_connect(db, &con)
    }

    func execute(_ sql: String) {
        var result = duckdb_result()
        duckdb_query(con, sql, &result)
        duckdb_destroy_result(&result)
    }

    func query(_ sql: String) -> [[String: Any]] {
        var result = duckdb_result()
        duckdb_query(con, sql, &result)

        var rows: [[String: Any]] = []
        let colCount = duckdb_column_count(&result)
        let rowCount = duckdb_row_count(&result)

        for r in 0..<rowCount {
            var row: [String: Any] = [:]
            for c in 0..<colCount {
                let name = String(cString: duckdb_column_name(&result, c))
                row[name] = duckdb_value_double(&result, c, r)
            }
            rows.append(row)
        }

        duckdb_destroy_result(&result)
        return rows
    }

    deinit {
        duckdb_disconnect(&con)
        duckdb_close(&db)
    }
}
```

## 使用示例

```swift
// 创建数据库和向量表
let db = VexDB()
db.execute("CREATE TABLE photos (id INTEGER, embedding FLOAT[512])")

// 插入向量
db.execute("INSERT INTO photos VALUES (1, [0.1, 0.2, ...]::FLOAT[512])")

// 创建 HNSW 索引
db.execute("""
    CREATE INDEX idx ON photos USING GRAPH_INDEX(embedding)
    WITH (metric='cosine', m=16, ef_construction=200)
""")

// 语义搜索
let results = db.query("""
    SELECT id FROM photos
    ORDER BY cosine_distance(embedding, [0.3, 0.4, ...]::FLOAT[512])
    LIMIT 10
""")

// 混合索引（向量 + 标签过滤）
db.execute("CREATE TABLE items (id INT, category VARCHAR, vec FLOAT[128])")
db.execute("CREATE INDEX hidx ON items USING HYBRID_INDEX(vec, category)")
let filtered = db.query("""
    SELECT id FROM items
    WHERE category = '数码'
    ORDER BY cosine_distance(vec, [...]::FLOAT[128])
    LIMIT 10
""")
```

## 持久化

```swift
// 使用文件路径创建持久化数据库
let docPath = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
let dbPath = docPath.appendingPathComponent("vectors.db").path
let db = VexDB(path: dbPath)

// 数据自动持久化到文件
db.execute("CREATE TABLE t (id INT, v FLOAT[3])")
db.execute("INSERT INTO t VALUES (1, [1,0,0]::FLOAT[3])")
db.execute("CREATE INDEX idx ON t USING GRAPH_INDEX(v)")
db.execute("CHECKPOINT")  // 强制写盘

// 下次启动直接打开，索引自动恢复
```

## 运行时配置

```swift
db.execute("SET vex_ef_search = 100")      // 搜索精度（越高越准，越慢）
db.execute("SET vex_brute_force_threshold = 64")  // 小数据暴力搜索阈值
```

## 包体积

链接到 app 后约 **11MB**（经 dead code strip）。静态库本身较大（~139MB）是因为包含完整的 .o 文件，链接器只会取用实际引用的符号。

## 最低版本

- iOS 15.0+
- Xcode 14+
- Swift 5.7+
