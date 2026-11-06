# VexDB Lite iOS Demo

一个在 iPhone 上运行的 SwiftUI 向量检索演示。App 使用 SQLite C API 静态注册
VexDB Lite。首页包含默认文本、自定义文本、默认图片和自定义图片四个 Tab。

## 演示内容

- 默认示例来自《Embedding 文档切片.docx》，按结构和完整句子生成 13 个分片；
- 分片和 7 个预设问题使用 `text-embedding-v4` 预生成 1024 维向量；
- 默认页只显示文档前 100 字，点击后在弹窗中查看并展开全部分片；
- 点击预设问题后，直接用打包的查询向量执行本机 top-5 KNN；
- 搜索结果省略展示，点击后展开原文；
- 查询耗时、数据库大小和 `EXPLAIN QUERY PLAN`；
- 粘贴用户文本，按字符数和重叠长度自动分片；
- 生成向量前以可展开方式预览每个分片；
- 输入框有内容后可以收起为 100 字摘要，生成向量成功后自动收起；
- 通过 OpenAI-compatible Embedding API 批量生成向量；
- 用完整问题查询，向量索引和 top-5 KNN 均在手机本地运行。
- 默认图片页内置 `embedding_test/images/general` 的 20 张图片和 3 个文字问题，使用本机图片图索引检索；
- 自定义图片页一次最多选择 12 张图片，支持保存图片、追加索引和文字搜图；
- 图片搜索展示真实 L2 距离、查询耗时和 `EXPLAIN QUERY PLAN`。

## 使用自己的图片

1. 打开“自定义图片”Tab，设置支持图片与文字的多模态 Embedding 模型；
2. 从照片中一次选择最多 12 张图片；
3. 点击“生成并加入索引”，图片和向量保存在本机；
4. 输入图片内容描述，查看最接近的 5 张图片和真实执行计划。

默认图片与自定义图片共用 `vexdb.sqlite`，但分别创建
`bundled_media_vectors` 和 `user_media_vectors` 两张 `GRAPH_INDEX` 虚拟表。
两张索引可以独立重建，不会互相覆盖。

## 使用自己的文本

1. 打开“自定义文本”Tab，点击“Embedding 模型”；
2. 点击“保存并测试连接”；
3. 粘贴文本，设置每段长度和相邻段重叠长度，检查分片预览；
4. 点击“生成向量”；
5. 输入完整问题，查看最接近的 5 个原文片段和真实执行计划。

App 默认载入 `Resources/embedding-demo.txt`，内容来自《Embedding 文档切片.docx》的
“智能城市与人工智能应用综合白皮书”。页面提供自动驾驶、Bloom Filter、智慧农业、
隐私保护、北京 GDP、中英混合和向量数据库等标准测试问题。

默认使用 OpenAI 兼容接口：

- API 地址：`https://dashscope.aliyuncs.com/compatible-mode/v1`
- 模型：`text-embedding-v4`
- 默认向量维度：1024
- 每批最多 10 个文本分片

用户只需要在设置页填写自己的 API Key。Key 只写入 iOS Keychain。默认示例的
分片和预设问题向量已预生成并随 App 打包，API Key 不会写入源码或 App 资源。默认页
输入任意新问题时仍需要用户自己的 Key，因为新问题必须用同一个模型生成向量。

API 需要兼容 OpenAI 的 embeddings 请求格式：

```http
POST /v1/embeddings
Authorization: Bearer <API Key>
Content-Type: application/json

{"model":"text-embedding-v4","input":["第一段文本","第二段文本"]}
```

API 地址既可以填写到 `/v1`，也可以直接填写到 `/embeddings`。非本机地址必须使用
HTTPS。本机调试允许 `http://127.0.0.1` 和 `http://localhost`。

API Key 只保存在 iOS Keychain。自定义文本片段和查询问题会发送给用户配置的 API
生成向量；原文、向量、索引和相似度查询保存在手机本地。默认示例的预设问题不访问网络。

内置数据和用户数据使用两个独立数据库：

- `vexdb-example.sqlite`：默认文档的 13 个预生成向量；
- `vexdb-user.sqlite`：用户导入的文本和动态维度向量。

## 构建

要求：Xcode、CMake 和 Boost headers。脚本会先构建
`dist/sqlite/vexdb_lite.xcframework`，再生成 Xcode 工程并编译 simulator App。

```bash
bash examples/ios/VexDBLiteDemo/build_demo.sh
```

产物：

```text
examples/ios/VexDBLiteDemo/build-simulator/Debug-iphonesimulator/VexDB Lite.app
```

已有启动中的 simulator 时可以直接安装并启动：

```bash
bash examples/ios/VexDBLiteDemo/build_demo.sh run
```

20 张示例图已经是压缩良好的 JPEG，构建时直接打包，不再转成体积更大的 WebP。
Release 包体预算可以这样检查：

```bash
python3 examples/ios/VexDBLiteDemo/Tests/verify_app_bundle.py
```

用户文本链路可以连接 OpenAI-compatible 测试服务做自动冒烟：

```bash
/usr/bin/python3 examples/ios/VexDBLiteDemo/Tests/mock_embedding_server.py --port 18765
xcrun simctl launch booted org.vexdb.lite.demo \
  --user-smoke \
  --embedding-endpoint http://127.0.0.1:18765/v1 \
  --embedding-model mock-1536
```

自定义文本导入使用连续的 Float32 数据传入原生层，避免为每个向量元素创建
`NSNumber`。为控制手机峰值内存，单次导入最多接受 250 万个向量元素；超过后会提示
增加分片长度或减少导入文本，不会覆盖原有索引。

也可以打开生成的工程：

```bash
open examples/ios/VexDBLiteDemo/build-simulator/VexDBLiteDemo.xcodeproj
```

## 集成要点

iOS 不使用 `.load`。App 链接静态 XCFramework 和系统 `libsqlite3`，在每个连接打开后
调用：

```c
vexdb_sqlite_register(db);
```

Demo 最低系统为 iOS 17，避免当前 iOS NEON 编译基线与更老设备不一致。
