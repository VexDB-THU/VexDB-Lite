# VexDB Lite WASM 网页演示

这个示例把 SQLite、VexDB Lite 和共享图索引算法静态编译进 WebAssembly。查询在浏览器内执行，不是用 JavaScript 数组模拟相似度排序。页面沿用 iOS 演示的深色卡片风格，使用两层 Tab：第一层切换文本/图片检索，第二层切换默认/自定义数据。

## 快速运行

macOS 首次准备工具：

```bash
brew install emscripten cmake boost node webp
```

从项目根目录构建并测试：

```bash
bash build_sqlite.sh wasm
```

构建后可以直接双击打开：

```text
examples/wasm/dist/vexdb-lite-demo.html
```

这个单文件已经内嵌 WASM、样式、页面脚本、演示文档和向量，不需要启动任何服务。20 张示例图直接复用已经压缩好的 JPEG；WASM、文本和向量清单在单文件内使用 gzip，页面打开时由浏览器原生解压。

如果需要开发调试或查看多个原始文件，再启动本地静态服务器：

```bash
npm --prefix examples/wasm run serve
```

浏览器打开 <http://127.0.0.1:4173>。

也可以在本目录分别执行：

```bash
npm run build
npm test
npm run serve
```

## 测试验证了什么

`npm test` 会直接在 Node.js 中加载同一个 WASM 文件，并验证：

- 建立内存 SQLite `GRAPH_INDEX` 虚拟表；
- 导入演示文档的 13 个、1024 维向量；
- 执行 top-5 向量检索；
- 第一条结果命中预期文本片段；
- SQLite 执行计划包含 `VIRTUAL TABLE INDEX 1`，证明查询走了 VexDB 图索引。
- 用 L2 距离导入 20 张、1152 维的内置图片向量；
- 对内置图片的 3 条文字搜图问题执行 top-5 检索，并输出 recall@1 / recall@5；
- 同一个 WASM 和 SQLite 中的四张 `GRAPH_INDEX` 表可以独立重建，查询计划命中各自表；
- 生成的单文件 HTML 不引用外部脚本、样式、WASM 或演示数据；
- 单文件内 gzip 数据可以无损还原成原始 WASM、文本和向量清单。

网页提供四条完整交互：

- 默认文本示例：选择预设问题只填入输入框，点击发送后才运行 WASM 查询；预设问题使用内置向量，不需要 API Key。
- 自定义文本：设置 OpenAI-compatible Embedding 接口，输入或填入示例文本，检查分片，生成向量，再搜索自己的内容。
- 默认图片示例：展示 20 张普通场景内置图片；选择 3 个预设问题之一后点击发送，使用内置问题向量执行真实 L2 检索。
- 自定义图片：设置多模态 Embedding 接口，导入最多 20 张图片，生成独立的 L2 索引，再用完整问题搜索图片。

默认文本、默认图片、自定义文本和自定义图片共用一个 WASM 模块和一个内存 SQLite，分别使用 `bundled_text_vectors`、`user_text_vectors`、`bundled_media_vectors` 和 `user_media_vectors` 四张 `GRAPH_INDEX` 表。重建时只替换目标表，不会覆盖其他数据。自定义文本输入框和搜索结果可以收起或展开。API 地址和模型保存在浏览器本地，API Key 只保存在当前浏览器会话，不会写入单文件 HTML。Embedding API 必须允许浏览器跨域请求，远程地址必须使用 HTTPS。

## 构建说明

浏览器不能像桌面 SQLite 一样动态加载扩展，因此这里使用 `VEXDB_SQLITE_CORE` 静态注册方式，把扩展直接链接进 WASM。页面只创建一个 WASM 实例。按当前 64 MiB 初始线性内存配置，四条流程全部使用后不再需要四份 WASM 线性内存；索引数据仍按四张表实际占用空间。当前默认不启用 WASM pthread，这样普通静态服务器也能打开，不要求额外设置 COOP/COEP 响应头。浏览器内的索引构建使用 1 个线程；查询算法、存储格式和原生 SQLite 仍共用同一套实现。

Linux 或自带 Emscripten SDK 的环境可以设置 `VEXDB_BOOST_INC` 指向 Boost 头文件目录。构建产物位于 `examples/wasm/dist/`，其中 `vexdb-lite-demo.html` 可以离线直接打开；其余分离文件用于开发调试和部署到静态网站。单文件依赖现代浏览器的 `DecompressionStream`；版本过旧时页面会显示明确错误，不会静默失败。

浏览器联调自定义流程时，可以启动只用于测试的 8 维假 Embedding 服务：

```bash
node tests/mock-embedding-server.mjs 18766
```

然后在文本设置或图片设置中填写 `http://127.0.0.1:18766/v1`。文本模型可填写 `mock-8`，图片模型可填写 `mock-image-8`。

参考资料：

- [SQLite WASM 自定义扩展构建](https://www.sqlite.org/wasm/doc/tip/building.md)
- [Emscripten 模块化输出](https://emscripten.org/docs/compiling/Modularized-Output.html)
- [Emscripten pthread 限制](https://emscripten.org/docs/porting/pthreads.html)
