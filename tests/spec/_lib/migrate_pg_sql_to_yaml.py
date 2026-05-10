#!/usr/bin/env python3
"""把 pg_tests/ 风格的手写 PG .sql 反向迁移到 spec yaml.

PG sql 格式约定:
    -- expected: <expected value>
    SELECT ...;

    -- SKIPPED: <reason>
    SELECT ...;          # 跳过, 不入 yaml

    CREATE TABLE / INSERT / DROP TABLE 等 DDL/DML 直接对应 statement (无 expected)

输出 yaml 放到 tests/spec/pg/<sub>/<name>.yaml, name 直接复用 .sql basename.
不做模板化 (这些是 PG 专属用例, 字典宏对它们没意义).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml


# 拆 SQL 为 (注释行列表, sql 文本) 块
def parse_pg_sql(text: str):
    """从 .sql 解析出 step list. 每个 step 含 statement / query, 可选 expect/expect_error."""
    lines = text.splitlines()
    steps: list[dict] = []

    # 收集 leading 注释 (含 -- expected:)
    pending_comments: list[str] = []
    pending_expected: list = []
    pending_skip = False

    sql_buf: list[str] = []

    def flush_sql(force_query: bool = False):
        nonlocal pending_comments, pending_expected, pending_skip, sql_buf
        if not sql_buf:
            return
        sql = "\n".join(sql_buf).strip()
        if not sql:
            sql_buf = []
            return

        if pending_skip:
            # SKIPPED 用例不入 yaml
            sql_buf = []
            pending_comments = []
            pending_expected = []
            pending_skip = False
            return

        is_query = sql.lstrip().upper().startswith("SELECT") or force_query
        step: dict = {}
        if is_query and pending_expected:
            step["query"] = sql
            # expect 是 [[v1], [v2], ...] 形式 (单列, 1 列 = 1 个值/行)
            step["expect"] = [[v] for v in pending_expected]
        elif is_query:
            step["query"] = sql
            step["expect"] = []
        else:
            step["statement"] = sql
        steps.append(step)
        sql_buf = []
        pending_comments = []
        pending_expected = []
        pending_skip = False

    for raw in lines:
        ln = raw.rstrip("\r")
        stripped = ln.strip()

        # 注释行
        if stripped.startswith("--"):
            content = stripped[2:].strip()
            if content.lower().startswith("expected:"):
                # -- expected: foo, bar
                val_str = content[len("expected:"):].strip()
                # 多个 expected 用逗号分隔: "1, 3" 或者多行 "expected:" 累计
                # 简化: 每个值单独一行 (这里允许逗号分隔)
                if "," in val_str:
                    vals = [v.strip() for v in val_str.split(",")]
                else:
                    vals = [val_str]
                # 转 int/float, 失败保留原 string
                for v in vals:
                    if v in ("", "NULL", "(empty)"):
                        pending_expected.append(None)
                    else:
                        try:
                            pending_expected.append(int(v))
                        except ValueError:
                            try:
                                pending_expected.append(float(v))
                            except ValueError:
                                pending_expected.append(v)
            elif content.lower().startswith("skipped"):
                pending_skip = True
            else:
                pending_comments.append(content)
            continue

        # 空行: 如果 buf 有 SQL 且以 ; 结尾, 视为 SQL 终止
        if stripped == "":
            if sql_buf and sql_buf[-1].rstrip().endswith(";"):
                flush_sql()
            else:
                # 空行也重置 pending_skip - SKIPPED 注释只影响紧邻的下一条 SQL
                pending_skip = False
                pending_expected = []
                pending_comments = []
            continue

        sql_buf.append(ln)
        # SQL 以 ; 结束 → flush
        if stripped.endswith(";"):
            flush_sql()

    # 末尾残留
    flush_sql()
    return steps


def migrate_one(src: Path, out: Path, name: str | None = None) -> int:
    text = src.read_text()
    steps = parse_pg_sql(text)
    if not steps:
        print(f"!! empty: {src}", file=sys.stderr)
        return 0
    spec = {
        "name": name or src.stem,
        "tags": ["pg-only", "migrated"],
        "description": f"Migrated from pg_tests/{src.parent.name}/{src.name}",
        # PG 专属 spec: 显式标 engines, 这些用例不走 dialect 模板化, 是 PG 直写 SQL
        "engines": ["pg", "opengauss"],
        "steps": steps,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    header = "# auto-migrated from pg_tests/ — PG 专属用例, 不模板化\n"
    out.write_text(header + yaml.safe_dump(spec, sort_keys=False, allow_unicode=True, width=200))
    return len(steps)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help=".sql 文件")
    ap.add_argument("--out", required=True, help="输出 yaml 路径")
    ap.add_argument("--name")
    args = ap.parse_args()
    n = migrate_one(Path(args.src), Path(args.out), args.name)
    print(f"wrote {args.out} ({n} steps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
