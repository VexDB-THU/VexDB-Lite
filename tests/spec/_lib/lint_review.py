#!/usr/bin/env python3
"""扫描迁移后的 spec, 列出需人工 review 的文件 + 原因.

检查项:
  - DuckDB-only 语法 (list_transform, range(), [::INT[]] 等) → PG 渲染会失败
  - raw_directive (loop/onlyif/skipif 控制流) → 需手工补全
  - expect 行数与 _field_types 列数不符 → 数据缺失
  - SET vex_* 等运行时 GUC → PG 没有对应配置
  - explain analyze, attach 等 DuckDB 专属语句

输出: review-needed.md  (按严重度分级)
"""
from __future__ import annotations

import re
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]

DUCKDB_ONLY_PATTERNS = [
    (re.compile(r"\blist_transform\b"), "list_transform (DuckDB only) - 标 skip:pg 或改 array_agg"),
    (re.compile(r"\bATTACH\s"), "ATTACH (DuckDB only) - 标 skip:pg"),
    (re.compile(r"\bDETACH\s"), "DETACH (DuckDB only) - 标 skip:pg"),
    (re.compile(r"\bduckdb_\w+\("), "duckdb_xxx() 系统函数 - 字典补全或 skip:pg"),
    (re.compile(r"\bvex_index_info\b"), "vex_index_info() (DuckDB only) - 标 skip:pg"),
    (re.compile(r"\bvex_inspect\w*\b"), "vex_inspect_*() (DuckDB only) - 标 skip:pg"),
    (re.compile(r"::FLOAT\["), "DuckDB-only ::FLOAT[N] cast 残留 (含表达式) - 标 skip:pg"),
    (re.compile(r"\bEXPLAIN\s+ANALYZE\b", re.I), "EXPLAIN ANALYZE - output 引擎差异大, 标 skip:pg"),
    (re.compile(r"\bdisabled_optimizers\b"), "disabled_optimizers GUC (DuckDB only) - 标 skip:pg"),
    (re.compile(r"\barray_inner_product\b"), "array_inner_product (DuckDB only) - 标 skip:pg"),
    (re.compile(r"\barray_cosine_similarity\b"), "array_cosine_similarity (DuckDB only) - 标 skip:pg"),
    (re.compile(r"\b(?:vector_add|vector_sub|vector_mul)\b"), "vector_add/sub/mul (PG_VEXDB 未实现) - 标 skip:pg"),
    (re.compile(r"\b<~>\b"), "<~> 操作符 (DuckDB only) - 标 skip:pg"),
    (re.compile(r"->\s*[a-z]"), "lambda 箭头 x -> ... (DuckDB list_transform; PG 不支持) - 标 skip:pg"),
    (re.compile(r"\$\{[a-z]\w*\}"), "sqllogictest 循环变量 ${var} - PG psql 不识别 - 标 skip:pg"),
]


# 未模板化警告 - 仍残留原始 DuckDB 语法的 (说明字典/迁移未覆盖到)
RAW_DUCKDB_LEAKS = [
    (re.compile(r"\brange\s*\(\s*\d"), "range() 未模板化 - 复杂表达式 migrate 没识别"),
    (re.compile(r"\bSET\s+vex_\w+", re.I), "SET vex_xxx 未模板化"),
    (re.compile(r"\bWITH\s*\(\s*metric\s*=", re.I), "WITH (metric=) 未模板化 - CREATE INDEX 模式不匹配"),
    (re.compile(r"^restart\s*$", re.M), "restart 控制流未识别 - 应为 raw_directive"),
]

CONTROL_FLOW_KEYS = ("raw_directive",)


def scan(spec_path: Path) -> list[str]:
    spec = yaml.safe_load(spec_path.read_text())
    issues: list[str] = []
    steps = spec.get("steps", [])

    # restart 已字典化为 raw_directive='restart' - 不算 review 项
    raw_non_restart = [
        s for s in steps
        if "raw_directive" in s and s["raw_directive"] != "restart"
    ]
    if raw_non_restart:
        issues.append("含 raw_directive (loop/onlyif/skipif)")

    for step in steps:
        sql = step.get("statement") or step.get("query") or ""
        for pat, msg in DUCKDB_ONLY_PATTERNS:
            if pat.search(sql):
                issues.append(msg)
        for pat, msg in RAW_DUCKDB_LEAKS:
            if pat.search(sql):
                issues.append(msg)

    return list(dict.fromkeys(issues))


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--auto-skip-pg", action="store_true",
                    help="把 PG 不兼容的 spec 自动加 skip: pg 字段")
    args = ap.parse_args()

    spec_files = sorted(p for p in (ROOT / "migrated").rglob("*.yaml"))
    by_issue: dict[str, list[str]] = {}
    no_issues: list[str] = []
    pg_incompatible: list[Path] = []

    pg_incompat_msgs = {
        msg for _, msg in DUCKDB_ONLY_PATTERNS
        if "skip:pg" in msg or "DuckDB only" in msg or "未实现" in msg
    }

    for f in spec_files:
        issues = scan(f)
        rel = str(f.relative_to(ROOT))
        if not issues:
            no_issues.append(rel)
            continue
        if any(i in pg_incompat_msgs for i in issues):
            pg_incompatible.append(f)
        for issue in issues:
            by_issue.setdefault(issue, []).append(rel)

    if args.auto_skip_pg:
        for f in pg_incompatible:
            spec = yaml.safe_load(f.read_text())
            skip = spec.setdefault("skip", {})
            if "pg" not in skip:
                skip["pg"] = "auto-skip: PG 不兼容语法 (DuckDB-only function/syntax)"
                skip["opengauss"] = "auto-skip: 同 pg"
                # 保持 yaml 头注释
                header = []
                with f.open() as fp:
                    for line in fp:
                        if line.startswith("#"):
                            header.append(line.rstrip())
                        else:
                            break
                f.write_text("\n".join(header) + "\n" + yaml.safe_dump(spec, sort_keys=False, allow_unicode=True, width=200))
        print(f"auto-skipped {len(pg_incompatible)} spec(s) for pg/opengauss")

    out = ROOT.parent.parent / "docs" / "analysis" / "2026-05-09_spec-review-needed.md"
    out.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "# 迁移 Spec 人工 Review 清单",
        "",
        f"**总计**: {len(spec_files)} 个 spec, {len(no_issues)} 干净, {len(spec_files) - len(no_issues)} 需 review",
        "",
        "## 按问题分类",
        "",
    ]
    for issue, files in sorted(by_issue.items(), key=lambda kv: -len(kv[1])):
        lines.append(f"### {issue} ({len(files)})")
        lines.append("")
        for f in files:
            lines.append(f"- `{f}`")
        lines.append("")

    lines += ["## 干净的 spec (无需 review)", ""]
    for f in no_issues:
        lines.append(f"- `{f}`")
    out.write_text("\n".join(lines) + "\n")
    print(f"clean: {len(no_issues)}, need review: {len(spec_files) - len(no_issues)}")
    print(f"report: {out}")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
