# Spec 测试框架（声明式 DSL，多引擎）

**Single Source of Truth**：一份 YAML 用例编译成 DuckDB sqllogictest / PG SQL+expected / openGauss / Python pytest 多份产物。

---

## 项目目前的测试体系（**3 套**，规划只保留本框架）

```
┌─────────────────────────────────────────────────────────────────────────┐
│ ① legacy DuckDB .test          109 个    vexdb-duck/test/sql/vex/       │
│    bash build.sh test 跑的就是这个                                       │
│    锁死 DuckDB sqllogictest, 无法服用 PG/SQLite                          │
├─────────────────────────────────────────────────────────────────────────┤
│ ② pg_tests/ 手写 SQL          59 个    ../pg_tests/  (vexdb_lite 外层) │
│    "Translated from .test" 风格的 PG 翻译                               │
│    跟 ① 大量重复 (16 个完全副本); 没有定期 runner                       │
├─────────────────────────────────────────────────────────────────────────┤
│ ③ spec DSL yaml (本框架)      106 个   tests/spec/                      │
│    多引擎 SSOT, 路由 ENGINE_DIRS 加一行扩 SQLite/openGauss              │
│    PG Docker 测试通过率 24/25 (96%)                                     │
└─────────────────────────────────────────────────────────────────────────┘
```

**目标**：完成迁移后 `rm -rf` ① ②，全部走 ③。详见 `docs/analysis/2026-05-09_pg-tests-vs-spec-shared-classification.md`。

---

## 目录组织（按引擎物理分类）

```
tests/spec/
├── _lib/                          工具链
│   ├── dialects.yaml              各引擎变量字典 (vars / functions / inherit)
│   ├── render.py                  YAML → 多引擎产物 (按 ENGINE_DIRS 路由)
│   ├── classify.py                自动分类: shared / duckdb (含 strip_templates 平衡扫描器)
│   ├── migrate_test_to_yaml.py    sqllogictest .test → YAML (反向迁移工具)
│   ├── lint_review.py             spec 人工 review 提示生成
│   └── docker/                    PG 19devel 测试容器
│       ├── Dockerfile             三阶段: pg-build / vex-build / runtime (Trixie + Boost 1.83)
│       ├── run_pg.sh              build / up / down / test / shell
│       └── compare.py             容差比较器 (浮点 atol/rtol, NULL/bool 规范化, 向量字面量元素级)
│
├── shared/                        ✅ 跨引擎核心 (DuckDB + PG + openGauss 共用)
│   ├── types/      (2)
│   ├── functions/  (3)
│   └── index/      (20)
├── duckdb/                        ✅ DuckDB 独有 (~80 个)
│   ├── functions/                 含 list_transform / vex_index_info / 表达式数组等 DuckDB-only
│   └── index/                     含 ATTACH / restart / optimizer_explain / vex_simd_arch 等
├── pg/                            🟡 PG 独有 (待补; 13 个 PG 专属用例)
└── opengauss/                     🟡 openGauss 独有 (待补; ivfpq_basic_hnsw_adapted 等)
```

**为什么按引擎分目录而不用 capability tag**：物理目录直观（看路径就知道归属），`git log` 历史按引擎清晰，新引擎只在 `render.py` 的 `ENGINE_DIRS` 加一行路由，不用改业务规则。

---

## 引擎路由（`render.py` 的 `ENGINE_DIRS`）

```python
ENGINE_DIRS = {
    "duckdb":    ["shared", "duckdb"],
    "pg":        ["shared", "pg"],
    "opengauss": ["shared", "pg", "opengauss"],   # opengauss = PG 兼容 + 自家
    "sqlite":    ["shared", "sqlite"],            # 将来扩展
    "python":    ["shared", "duckdb"],            # python 后端 = DuckDB
}
```

---

## 写一个用例

```yaml
name: graph_index_basic_demo            # 全局唯一
tags: [core, index]                     # 子集筛选
description: HNSW 图索引基础 - 建索引 + KNN 查询
skip:
  opengauss: "TAPD-1078122 待修"        # 精确豁免 (shared 用例的少量例外用)

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
| `${GUC_EF_SEARCH}` 等 | `vex_ef_search` | `vexdb.ef_search` |
| `${SYS_INDEXES}` / `${SYS_INDEXES_NAME}` | `duckdb_indexes()` / `index_name` | `pg_indexes` / `indexname` |

完整字典见 `_lib/dialects.yaml`。

---

## 命令

```bash
# 渲染所有引擎 → build/spec/{duckdb,pg,opengauss,python}/
python3 tests/spec/_lib/render.py --engine all --out build/spec

# 单引擎
python3 tests/spec/_lib/render.py --engine pg --out build/spec

# 反向迁移已有 .test → spec yaml (生成在 migrated/, 需手工 review 后入库)
python3 tests/spec/_lib/migrate_test_to_yaml.py --batch \
  --src-root vexdb-duck/test/sql/vex \
  --out-root tests/spec/migrated --include-slow

# 自动分类 shared / duckdb (从 30+ DuckDB-only 模式判定)
python3 tests/spec/_lib/classify.py --apply              # 从 migrated/ 首次分类
python3 tests/spec/_lib/classify.py --from shared --apply  # 从 shared/ 重新分类

# PG Docker 测试 (本仓库自带)
bash tests/spec/_lib/docker/run_pg.sh build  # 首次 build PG 19devel + pg_vexdb (~30min)
bash tests/spec/_lib/docker/run_pg.sh up     # 启动容器
bash tests/spec/_lib/docker/run_pg.sh test   # 跑 build/spec/pg/ 全部
bash tests/spec/_lib/docker/run_pg.sh down   # 清理
```

---

## 渲染产物形态

| 引擎 | 路径 | 格式 |
|---|---|---|
| DuckDB | `build/spec/duckdb/<name>.test` | sqllogictest |
| PG | `build/spec/pg/sql/<name>.sql` + `expected/<name>.out` | pg_regress 风格 |
| openGauss | `build/spec/opengauss/...` | 同 PG |
| Python | `build/spec/python/test_<name>.py` | pytest |

**禁止手工编辑 `build/spec/`** —— CI 通过 `render-check` job 校验产物来自 spec。

---

## 当前完成度（2026-05-09）

| 项 | 数量 / 状态 |
|---|---|
| `shared/` 跨引擎 yaml | 25 |
| `duckdb/` DuckDB-only yaml | 80 |
| `pg/` PG-only yaml | 0（待迁 13 个） |
| `opengauss/` openGauss-only yaml | 0（待迁 1 个） |
| **PG Docker 通过率** | **24/25 (96%)** |
| **DuckDB 接 `build.sh test`** | ❌ 未接通；仍跑 legacy `.test` |
| `pg_tests/` 退役 | ❌ 待补迁后删 |
| `vexdb-duck/test/sql/vex/` 退役 | ❌ 待 DuckDB 接通 + diff 后删 |

---

## 退役 ① ② 的依赖步骤（按顺序）

```
[✅] 1) spec 框架建立, 工具链就绪
[✅] 2) PG Docker 测试跑通 (24/25)
[ ] 3) 加 tests/spec/_lib/docker/run_duckdb.sh  ← spec 跑 DuckDB unittest 自带 runner
[ ] 4) 跑一轮, 跟 legacy ① 字符级 diff, 列漂移清单
[ ] 5) 修漂移 (改 yaml 或 dialects)
[ ] 6) 迁 ② 中 13 个 PG 专属 → tests/spec/pg/
[ ] 7) rm -rf vexdb-duck/test/sql/vex/ + ../pg_tests/
```

---

## 高阶断言（规划中，未实现 — render.py 不处理）

```yaml
assertions:
  - recall_vs_brute_force:
      query_count: 100
      k: 10
      min_recall: 0.95
```

差分测试将在阶段 2 接入。

---

## CI

`.github/workflows/spec-tests.yml`（独立分支，未入此分支 - 待并）：

| Job | 内容 | 门禁 |
|---|---|---|
| `render-check` | YAML 语法 + render --engine all + classify 一致性 | 必须 100% |
| `pg-spec` | Docker buildx + run_pg.sh test | 通过率 ≥ 90% |
| `duckdb-spec` | (placeholder, `if: false`) 等 inline build 实现 | — |

详见 `.github/workflows/spec-tests-README.md`。
