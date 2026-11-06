# Spec 测试框架

更新日期：2026-10-26。业务用例写在 YAML 中，渲染器生成各数据库的测试格式。
数量与渲染成功都不等于 SQL 测试通过，执行结果以实际 runner 和 CI 为准。

## 目录与数量

| 目录 | YAML 数量 | 用途 |
|---|---:|---|
| `shared/` | 26 | 跨引擎用例 |
| `duckdb/` | 88 | DuckDB 专属 |
| `pg/` | 74 | PostgreSQL 专属，含 VexFS |
| `sqlite/` | 30 | SQLite 专属，含 VexFS |
| `opengauss/` | 1 | openGauss 专属 |
| **合计** | **219** | 不含 `_lib/dialects.yaml` 字典 |

`_lib/render.py` 中的 `ENGINE_DIRS` 是引擎路由的来源：

```python
ENGINE_DIRS = {
    "duckdb":    ["shared", "duckdb"],
    "pg":        ["shared", "pg"],
    "opengauss": ["shared", "pg", "opengauss"],
    "sqlite":    ["shared", "sqlite"],
    "python":    ["shared", "duckdb"],
}
```

当前按目标发现的用例数是 DuckDB 114、PG 100、openGauss 101、SQLite 56、Python 114。
shared 用例会复用，`skip.<engine>` 会在渲染时跳过，因此不能相加作为独立用例总数。
历史文档里的 105/105、26/36 是当时记录，不代表当前分支通过率。

## 准备和渲染

```bash
python3 -m pip install -r tests/spec/requirements.txt
python3 tests/spec/_lib/render.py --engine all --out build/spec
python3 tests/spec/_lib/render.py --engine sqlite --out build/spec
```

| 目标 | 产物 | 执行器 |
|---|---|---|
| DuckDB | `build/spec/duckdb/<name>.test` | DuckDB unittest |
| PG / openGauss | `build/spec/<engine>/sql/`、`expected/` | PG runner / 对应宿主 |
| SQLite | `build/spec/sqlite/sql/`、`expected/` | Python sqlite3 + 动态扩展 |
| Python | `build/spec/python/test_<name>.py` | Python / DuckDB |

只修改 YAML，不手工维护生成物。当前 PR CI 运行全部目标的渲染；openGauss 和 Python
产物生成不表示 CI 已执行这两类测试。`render.py --check` 当前没有实现内容比对，不能当成一致性检查。

## 运行测试

### DuckDB

```bash
bash tests/spec/_lib/docker/run_duckdb.sh test
bash tests/spec/_lib/docker/run_duckdb.sh test '*graph_index_ann*'
```

这个 runner 不使用 Docker。它先渲染并部署到
`vexdb_duckdb/test/sql/vex/spec_run/`，再构建 unittest 并默认运行 `[spec_run]`。
源码与二进制默认位于 `build/duck/v1.5.2/`；设置 `DUCKDB_VERSION` 后会使用对应版本目录。
不要在干净检出中直接运行旧 `test/sql/vex/*` 路径而省略部署步骤。

### PostgreSQL

```bash
python3 tests/spec/_lib/render.py --engine pg --out build/spec
BUILDX_EXTRA_ARGS='--build-arg PG_GIT_TAG=REL_19_STABLE' \
  bash tests/spec/_lib/docker/run_pg.sh build
bash tests/spec/_lib/docker/run_pg.sh test
bash tests/spec/_lib/docker/run_pg.sh test 'pg__vexfs_*'
bash tests/spec/_lib/docker/run_pg.sh down
```

首次需要构建镜像。runner 会启动容器，结果位于 `build/spec/pg/results/`。
Dockerfile 的历史默认是 `master`；CI 显式传入 `REL_19_STABLE`，防止测试宿主意外换代。

### SQLite

```bash
bash build_sqlite.sh test
# 已构建时，只运行 spec
bash tests/spec/_lib/docker/run_sqlite.sh
# 显式绑定动态扩展
VEXDB_SQLITE_EXTENSION=/absolute/path/to/vexdb_lite.so \
  bash tests/spec/_lib/docker/run_sqlite.sh
```

SQLite runner 不使用 Docker，需要 Python 带 SQLite 动态扩展加载能力。
`build_sqlite.sh test` 还运行向量、VexFS、平台接口和 CLI 冒烟；平台接口的编译测试不等于真实挂载验收。

## 写一个用例

```yaml
name: graph_index_basic_demo            # 全局唯一
tags: [core, index]                     # 子集筛选
description: HNSW 图索引基础 - 建索引 + KNN 查询
skip:
  opengauss: "待修：openGauss 侧已知问题"  # 精确豁免 (shared 用例的少量例外用)

steps:
  - statement: CREATE TABLE t (id INT, v ${VECTOR(3)});
  - statement: INSERT INTO t VALUES (1, ${VEC_LITERAL([1,0,0], 3)});
  - statement: CREATE INDEX idx ON t USING ${VEX_INDEX} (v${OPS_L2_COL})${IDX_OPTS_L2};
  - query: SELECT id FROM t ORDER BY ${L2(v, ${VEC_LITERAL([1,0,0], 3)})} LIMIT 1
    expect: [[1]]
  - query: SELECT count(*) FROM ${SYS_INDEXES} WHERE ${SYS_INDEXES_NAME}='idx'
    expect: [[1]]
  - statement: DROP TABLE t
```

### 字典变量（在 `_lib/dialects.yaml` 维护）

| 变量 | DuckDB | PG / openGauss |
|---|---|---|
| `${VECTOR(N)}` | `FLOAT[N]` | `floatvector(N)` |
| `${VEC_LITERAL([1,0,0], 3)}` | `[1,0,0]::FLOAT[3]` | `'[1,0,0]'::floatvector` |
| `${VEX_INDEX}` | `GRAPH_INDEX` | `vexdb_graph` |
| `${OPS_L2_COL}` 列后位置 | (空) | ` floatvector_l2_ops` |
| `${IDX_OPTS_COSINE}` 索引末尾 | ` WITH (metric='cosine')` | (空) |
| `${IDX_WITH_COSINE(p1=v1)}` | ` WITH (metric='cosine', p1=v1)` | ` WITH (p1=v1)` |
| `${L2(a,b)}` | `l2_distance(a,b)` | `(a <-> b)` 中缀 |
| `${COSINE(a,b)}` `${IP(a,b)}` | 函数式 | `<=>` / `inner_product()` |
| `${RANGE(n)}` | `range(n)` | `generate_series(0, n-1)` |
| `${GUC_EF_SEARCH}` 等 | `vexdb_ef_search` | `vexdb.ef_search` |
| `${SYS_INDEXES}` / `${SYS_INDEXES_NAME}` | `duckdb_indexes()` / `index_name` | `pg_indexes` / `indexname` |

完整字典见 `_lib/dialects.yaml`。

---

## CI 与故障恢复

- `.github/workflows/pr-regression.yml`：PR、main push、手工触发；检查工作流、渲染全部目标，再执行 PG、DuckDB、SQLite Linux 回归。
- `.github/workflows/build-linux.yml`：tag 或手工触发；执行 DuckDB、SQLite 的 x86_64/aarch64 回归。
- `.github/workflows/duckdb-crash-recovery.yml`：手工运行已有 hard-kill harness；没有自动定时任务。
- `.github/workflows/build-mobile.yml`：Android SQLite 库交叉编译；不执行设备测试。

完整范围与已归档流程见 [当前状态](../../documentation/current-status.md)。

高阶 recall 断言仍未由渲染器实现；不要通过 YAML 中新增未支持的 `assertions` 字段来声明覆盖。
