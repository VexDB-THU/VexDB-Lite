# VexDB-Lite Android 使用指南

## 获取静态库

从 [GitHub Release](https://github.com/VexDB-Beijing/VexDB-Lite/releases) 下载：

- `libvexdb-android-arm64-v8a.tar.gz`

```bash
tar xzf libvexdb-android-arm64-v8a.tar.gz
# 产出: libvexdb.a
```

头文件从 iOS 包中获取（通用），或从源码 `duckdb/src/include/duckdb.h` 复制。

## Android 项目集成

### 1. 项目结构

```
app/
  src/main/
    cpp/
      CMakeLists.txt
      vexdb_jni.cpp
    jniLibs/
      arm64-v8a/
        (JNI .so 会自动生成)
  libs/
    arm64-v8a/
      libvexdb.a
    include/
      duckdb.h
```

### 2. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.18)
project(vexdb_jni)

# VexDB 静态库
add_library(vexdb STATIC IMPORTED)
set_target_properties(vexdb PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/../libs/${ANDROID_ABI}/libvexdb.a
)

# JNI 桥接
add_library(vexdb_jni SHARED vexdb_jni.cpp)
target_include_directories(vexdb_jni PRIVATE ${CMAKE_SOURCE_DIR}/../libs/include)
target_link_libraries(vexdb_jni
    vexdb
    log
    m
)
```

### 3. JNI 桥接 (vexdb_jni.cpp)

```cpp
#include <jni.h>
#include "duckdb.h"
#include <string>

extern "C" {

static duckdb_database db = nullptr;
static duckdb_connection con = nullptr;

JNIEXPORT void JNICALL
Java_com_vexdb_VexDB_open(JNIEnv *env, jobject, jstring path) {
    if (path == nullptr) {
        duckdb_open(nullptr, &db);
    } else {
        const char *p = env->GetStringUTFChars(path, nullptr);
        duckdb_open(p, &db);
        env->ReleaseStringUTFChars(path, p);
    }
    duckdb_connect(db, &con);
}

JNIEXPORT void JNICALL
Java_com_vexdb_VexDB_execute(JNIEnv *env, jobject, jstring sql) {
    const char *s = env->GetStringUTFChars(sql, nullptr);
    duckdb_result result;
    duckdb_query(con, s, &result);
    duckdb_destroy_result(&result);
    env->ReleaseStringUTFChars(sql, s);
}

JNIEXPORT jstring JNICALL
Java_com_vexdb_VexDB_queryScalar(JNIEnv *env, jobject, jstring sql) {
    const char *s = env->GetStringUTFChars(sql, nullptr);
    duckdb_result result;
    duckdb_query(con, s, &result);
    env->ReleaseStringUTFChars(sql, s);

    char *val = duckdb_value_varchar(&result, 0, 0);
    jstring ret = env->NewStringUTF(val ? val : "");
    duckdb_free(val);
    duckdb_destroy_result(&result);
    return ret;
}

JNIEXPORT void JNICALL
Java_com_vexdb_VexDB_close(JNIEnv *, jobject) {
    if (con) { duckdb_disconnect(&con); con = nullptr; }
    if (db) { duckdb_close(&db); db = nullptr; }
}

} // extern "C"
```

### 4. Kotlin 封装

```kotlin
package com.vexdb

class VexDB {
    init { System.loadLibrary("vexdb_jni") }

    external fun open(path: String?)
    external fun execute(sql: String)
    external fun queryScalar(sql: String): String
    external fun close()
}
```

### 5. build.gradle

```groovy
android {
    defaultConfig {
        externalNativeBuild {
            cmake {
                abiFilters 'arm64-v8a'
            }
        }
    }
    externalNativeBuild {
        cmake {
            path "src/main/cpp/CMakeLists.txt"
        }
    }
}
```

## 使用示例

```kotlin
val db = VexDB()

// 内存数据库
db.open(null)

// 或持久化到 app 内部存储
// db.open(context.filesDir.resolve("vectors.db").absolutePath)

// 创建向量表和索引
db.execute("CREATE TABLE photos (id INTEGER, embedding FLOAT[512])")
db.execute("""
    CREATE INDEX idx ON photos USING GRAPH_INDEX(embedding)
    WITH (metric='cosine', m=16, ef_construction=200)
""")

// 插入向量（通常从 ML 模型获取）
db.execute("INSERT INTO photos VALUES (1, [0.1, 0.2, ...]::FLOAT[512])")

// 语义搜索
val result = db.queryScalar("""
    SELECT id FROM photos
    ORDER BY cosine_distance(embedding, [0.3, 0.4, ...]::FLOAT[512])
    LIMIT 10
""")

// 混合过滤搜索
db.execute("CREATE TABLE items (id INT, category VARCHAR, vec FLOAT[128])")
db.execute("CREATE INDEX hidx ON items USING HYBRID_INDEX(vec, category)")
val filtered = db.queryScalar("""
    SELECT id FROM items WHERE category = '数码'
    ORDER BY cosine_distance(vec, [...]::FLOAT[128]) LIMIT 10
""")

db.close()
```

## 使用 JDBC 方式（替代 JNI）

如果不需要 JNI 级别的控制，可以直接使用 `vexdb-lite-1.5.0.jar`：

```kotlin
// build.gradle
dependencies {
    implementation files('libs/vexdb-lite-1.5.0.jar')
}

// Kotlin
Class.forName("org.duckdb.DuckDBDriver")
val con = DriverManager.getConnection("jdbc:duckdb:")
con.createStatement().execute("CREATE TABLE t (id INT, v FLOAT[3])")
con.createStatement().execute("CREATE INDEX idx ON t USING GRAPH_INDEX(v)")
```

## 运行时配置

```kotlin
db.execute("SET vex_ef_search = 100")
db.execute("SET vex_brute_force_threshold = 64")
```

## 要求

- Android API 24+ (Android 7.0)
- NDK 27+
- arm64-v8a（主流 Android 设备）
