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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _normalize import coerce_scalar  # noqa: E402


# 拆 SQL 为 (注释行列表, sql 文本) 块
class _ParseState:
    """parse_pg_sql 的累积状态. 替代之前 4 个 nonlocal + 闭包模式."""
    __slots__ = ("expected", "skip", "buf")

    def __init__(self) -> None:
        self.expected: list = []
        self.skip = False
        self.buf: list[str] = []

    def reset_pending(self) -> None:
        self.expected = []
        self.skip = False

    def flush(self, steps: list[dict]) -> None:
        sql = "\n".join(self.buf).strip()
        self.buf = []
        if not sql:
            self.reset_pending()
            return
        if self.skip:
            self.reset_pending()
            return
        is_query = sql.lstrip().upper().startswith("SELECT")
        step: dict = {"query": sql, "expect": [[v] for v in self.expected]} if is_query \
            else {"statement": sql}
        if is_query and not self.expected:
            step["expect"] = []
        steps.append(step)
        self.reset_pending()


def parse_pg_sql(text: str):
    """从 .sql 解析 step list. 每个 step 含 statement 或 query+expect."""
    steps: list[dict] = []
    st = _ParseState()

    for raw in text.splitlines():
        ln = raw.rstrip("\r")
        stripped = ln.strip()

        if stripped.startswith("--"):
            content = stripped[2:].strip()
            low = content.lower()
            if low.startswith("expected:"):
                val_str = content[len("expected:"):].strip()
                vals = [v.strip() for v in val_str.split(",")] if "," in val_str else [val_str]
                st.expected.extend(coerce_scalar(v) for v in vals)
            elif low.startswith("skipped"):
                st.skip = True
            continue

        if stripped == "":
            # buf 末尾有 ; 则当 SQL 结束; 否则重置 pending (SKIPPED 不跨空行污染)
            if st.buf and st.buf[-1].rstrip().endswith(";"):
                st.flush(steps)
            else:
                st.reset_pending()
            continue

        st.buf.append(ln)
        if stripped.endswith(";"):
            st.flush(steps)

    st.flush(steps)
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
