# VexDB-Lite WebAssembly 使用指南

## 获取 WASM 库

从 [GitHub Release](https://github.com/VexDB-Beijing/VexDB-Lite/releases) 下载：

- `libvexdb-wasm.a` — WASM 静态库

## 编译为 WASM 模块

需要 [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)：

```bash
# 安装 Emscripten
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
```

### 编译你的应用

```bash
emcc -O2 -pthread \
    your_app.c \
    libvexdb-wasm.a \
    -I include/ \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=268435456 \
    -sPTHREAD_POOL_SIZE=4 \
    -sENVIRONMENT=web,worker \
    -o vexdb_app.js
```

### 关键编译参数

| 参数 | 说明 |
|------|------|
| `-pthread` | 启用多线程（DuckDB 需要） |
| `-sALLOW_MEMORY_GROWTH=1` | 允许内存动态增长 |
| `-sINITIAL_MEMORY=268435456` | 初始内存 256MB |
| `-sPTHREAD_POOL_SIZE=4` | 线程池大小 |
| `-sENVIRONMENT=web,worker` | 浏览器环境 |

## JavaScript 集成示例

### Node.js

```javascript
import Module from './vexdb_app.mjs';

const db = await Module();
// db 现在包含编译后的 WASM 实例
// 通过 C API 调用 DuckDB 函数
```

### 浏览器

```html
<script src="vexdb_app.js"></script>
<script>
Module().then(function(db) {
    // 使用 WASM 模块
    console.log('VexDB-Lite WASM ready');
});
</script>
```

> **注意**: 浏览器环境需要设置 COOP/COEP 头部以启用 SharedArrayBuffer（pthread 依赖）：
> ```
> Cross-Origin-Opener-Policy: same-origin
> Cross-Origin-Embedder-Policy: require-corp
> ```

## C API 使用

WASM 模块暴露标准 DuckDB C API：

```c
#include "duckdb.h"

int main() {
    duckdb_database db;
    duckdb_connection con;
    duckdb_result result;

    // 打开内存数据库
    duckdb_open(NULL, &db);
    duckdb_connect(db, &con);

    // 创建向量表
    duckdb_query(con, "CREATE TABLE docs (id INT, vec FLOAT[384])", &result);
    duckdb_destroy_result(&result);

    // 插入向量
    duckdb_query(con, "INSERT INTO docs VALUES (1, [0.1, 0.2, ...]::FLOAT[384])", &result);
    duckdb_destroy_result(&result);

    // 创建 HNSW 索引
    duckdb_query(con, "CREATE INDEX idx ON docs USING GRAPH_INDEX(vec)", &result);
    duckdb_destroy_result(&result);

    // 语义搜索
    duckdb_query(con,
        "SELECT id FROM docs ORDER BY l2_distance(vec, [...]::FLOAT[384]) LIMIT 10",
        &result);
    // 处理结果...
    duckdb_destroy_result(&result);

    duckdb_disconnect(&con);
    duckdb_close(&db);
    return 0;
}
```

## 应用场景

### 浏览器端 RAG

在浏览器中运行向量搜索，无需服务器：

```javascript
// 1. 用户上传文档
// 2. 浏览器端 Embedding（使用 ONNX Runtime Web 或 Transformers.js）
// 3. 存入 VexDB-Lite WASM
// 4. 本地语义搜索，完全离线
```

### PWA 离线搜索

Progressive Web App 中实现离线向量搜索，数据不出浏览器。

## 已知限制

1. **持久化**：WASM 环境下文件系统是虚拟的，数据不会自动持久化到磁盘。需使用 IndexedDB 或 Emscripten 的 IDBFS 手动保存。
2. **内存**：浏览器内存受限，建议数据量控制在 10 万向量以内。
3. **线程**：需要 SharedArrayBuffer 支持（现代浏览器均支持，但需设置 COOP/COEP 头部）。
4. **SIMD**：WASM SIMD128 自动启用，提供向量距离计算加速。

## 包体积

WASM 模块约 **8-10MB**（经 wasm-opt 优化后），gzip 压缩后约 **3-4MB**。
