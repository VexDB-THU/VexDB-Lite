#!/usr/bin/env python3
"""L2 spec DSL 渲染器: YAML 用例 → 多引擎产物.

用法:
    python tests/spec/_lib/render.py --engine duckdb --out build/spec/duckdb
    python tests/spec/_lib/render.py --engine pg     --out build/spec/pg
    python tests/spec/_lib/render.py --check         # CI 校验产物一致

支持引擎: duckdb / pg / opengauss / python / java
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

import yaml

from _normalize import normalize_cell_str

ROOT = Path(__file__).resolve().parents[1]
DIALECTS_FILE = ROOT / "_lib" / "dialects.yaml"


def load_dialects() -> dict[str, dict]:
    raw = yaml.safe_load(DIALECTS_FILE.read_text())
    resolved: dict[str, dict] = {}
    for name, body in raw.items():
        base = body.get("inherit")
        if base:
            parent = resolved[base]
            merged = {
                "vars": {**parent.get("vars", {}), **body.get("vars", {})},
                "functions": {**parent.get("functions", {}), **body.get("functions", {})},
            }
            resolved[name] = merged
        else:
            resolved[name] = {
                "vars": body.get("vars", {}),
                "functions": body.get("functions", {}),
            }
    return resolved


# ${NAME} 或 ${NAME(arg1, arg2, ...)} - 名字大写; 小写 ${var} 是 sqllogictest 循环变量, 保留
VAR_NAME_PATTERN = re.compile(r"[A-Z_][A-Z0-9_]*")


def find_var_end(text: str, start: int) -> int:
    """从 ${ 之后扫描, 返回闭合 } 之后的位置. 支持任意嵌套 ${...} / () / []."""
    depth_brace = 1
    i = start
    while i < len(text) and depth_brace > 0:
        c = text[i]
        if c == "$" and i + 1 < len(text) and text[i + 1] == "{":
            depth_brace += 1
            i += 2
            continue
        if c == "{":
            depth_brace += 1
        elif c == "}":
            depth_brace -= 1
            if depth_brace == 0:
                return i + 1
        i += 1
    return -1  # 未闭合


def split_args(arg_str: str) -> list[str]:
    """逗号分隔参数, 但忽略 [] / () / '' 内的逗号."""
    if not arg_str.strip():
        return []
    args, depth_b, depth_p, in_quote, cur = [], 0, 0, False, []
    for ch in arg_str:
        if ch == "'" and (not cur or cur[-1] != "\\"):
            in_quote = not in_quote
            cur.append(ch)
        elif in_quote:
            cur.append(ch)
        elif ch == "[":
            depth_b += 1; cur.append(ch)
        elif ch == "]":
            depth_b -= 1; cur.append(ch)
        elif ch == "(":
            depth_p += 1; cur.append(ch)
        elif ch == ")":
            depth_p -= 1; cur.append(ch)
        elif ch == "," and depth_b == 0 and depth_p == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        args.append("".join(cur).strip())
    return args


def render_template(text: str, dialect: dict) -> str:
    """递归替换 text 中所有 ${...}. 未知变量保留原样 (sqllogictest 循环变量如 ${limit_val})."""
    vars_map = dialect["vars"]
    funcs_map = dialect["functions"]
    for _ in range(10):
        new_text = _render_one_pass(text, vars_map, funcs_map)
        if new_text == text:
            return new_text
        text = new_text
    return text


def _render_one_pass(text: str, vars_map: dict, funcs_map: dict) -> str:
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] == "$" and i + 1 < n and text[i + 1] == "{":
            # 找名字
            name_match = VAR_NAME_PATTERN.match(text, i + 2)
            if not name_match:
                # ${小写...} 或非法: 视为 sqllogictest 变量, 保留 ${ 单字符让外层继续
                out.append(text[i])
                i += 1
                continue
            name = name_match.group(0)
            after_name = name_match.end()
            # 扫描整个 ${...} 块的结束
            end = find_var_end(text, i + 2)
            if end < 0:
                out.append(text[i])
                i += 1
                continue
            # 检查是否带参数: 名字后紧跟 (
            if after_name < end - 1 and text[after_name] == "(":
                # 找匹配的 )
                paren_end = _find_paren_end(text, after_name + 1)
                # end 是 } 之后的位置, end-1 是 }, end-2 应是 ); paren_end 应等于 end-2
                if paren_end < 0 or paren_end != end - 2:
                    out.append(text[i:end])
                    i = end
                    continue
                args_str = text[after_name + 1 : paren_end]
                if name in funcs_map:
                    args = split_args(args_str)
                    template = funcs_map[name]
                    # 模板只有 1 个 placeholder 但调用传了多个 args:
                    # 把所有 args 用 ", " 重新 join 作为单参数
                    # (IDX_WITH_L2(quantizer='pq', pq_m=4) → " WITH (quantizer='pq', pq_m=4)")
                    placeholder_count = template.count("{")
                    if placeholder_count == 1 and len(args) > 1:
                        args = [", ".join(args)]
                    try:
                        out.append(template.format(*args))
                        i = end
                        continue
                    except IndexError:
                        pass
                # 未知函数或参数不匹配: 保留原样
                out.append(text[i:end])
                i = end
            else:
                # 简单变量 ${NAME}, 不带参数
                if name in vars_map:
                    out.append(vars_map[name])
                else:
                    out.append(text[i:end])
                i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _find_paren_end(text: str, start: int) -> int:
    """从 ( 之后位置开始, 找匹配的 ). 支持嵌套 () [] 和 ${...}."""
    depth_p = 1
    depth_b = 0
    in_quote = False
    i = start
    while i < len(text):
        c = text[i]
        if c == "'" and (i == 0 or text[i - 1] != "\\"):
            in_quote = not in_quote
            i += 1
            continue
        if in_quote:
            i += 1
            continue
        if c == "$" and i + 1 < len(text) and text[i + 1] == "{":
            # 跳过整个 ${...}
            sub_end = find_var_end(text, i + 2)
            if sub_end < 0:
                return -1
            i = sub_end
            continue
        if c == "(":
            depth_p += 1
        elif c == ")":
            depth_p -= 1
            if depth_p == 0 and depth_b == 0:
                return i
        elif c == "[":
            depth_b += 1
        elif c == "]":
            depth_b -= 1
        i += 1
    return -1


# ─────────────────────────── 各引擎 emitter ────────────────────────────

# onlyif/skipif 修饰紧邻下一条 statement/query, 它和被修饰的 SQL 之间不能有空行.
MODIFIER_DIRECTIVES = ("onlyif", "skipif")


def _has_real_sql(body: str) -> bool:
    """body 是否含非注释非空白行 (sqllogictest 视空行 + # 为分隔/注释)."""
    return any(ln.strip() and not ln.lstrip().startswith("#") for ln in body.splitlines())


def _query_header(field_types: str, sort_mod: str) -> str:
    """sqllogictest query header: 'query I' / 'query I rowsort' / ..."""
    return f"query {field_types}" + (f" {sort_mod}" if sort_mod else "")


def _sort_rows(rows: list, sort_mod: str) -> list:
    """按 sqllogictest sort 语义对 rows 重排.
    - rowsort: 行级排序 (lexicographic on string repr of each row)
    - sort / valuesort: 拍平所有 cell 后排序, 重新切回 row
    - nosort / 空: 不动
    """
    head = (sort_mod or "").split()[:1]
    head = head[0] if head else ""
    if head == "rowsort":
        return sorted(rows, key=lambda r: tuple(str(v) for v in r))
    if head in ("sort", "valuesort"):
        ncols = len(rows[0]) if rows else 1
        flat = sorted((str(v) for r in rows for v in r))
        return [flat[i:i + ncols] for i in range(0, len(flat), ncols)]
    return rows


def emit_duckdb(spec: dict, dialect: dict) -> str:
    """sqllogictest .test 格式."""
    lines = [
        f"# name: test/sql/vex/{spec['name']}.test",
        f"# description: {spec.get('description', spec['name'])}",
        f"# group: [vex]",
        "",
        "require vex",
        "",
    ]
    if spec.get("setup"):
        for stmt in split_sql(render_template(spec["setup"], dialect)):
            lines += ["statement ok", stmt, ""]
    for step in spec.get("steps", []):
        if "raw_directive" in step:
            d = step["raw_directive"]
            body = step.get("raw_body", "").strip()
            head = d.split(maxsplit=1)[0]
            if head in MODIFIER_DIRECTIVES:
                lines.append(d)  # 紧贴下一行 (无空行)
            else:
                lines += [d, ""]
            if _has_real_sql(body):
                lines += [body, ""]
            continue
        if "statement" in step and "expect_error" not in step:
            for stmt in split_sql(render_template(step["statement"], dialect)):
                lines += ["statement ok", stmt, ""]
        elif "expect_error" in step:
            sql = step.get("query") or step.get("statement", "")
            lines += [
                "statement error",
                render_template(sql, dialect),
                "----",
                step["expect_error"],
                "",
            ]
        elif "query" in step:
            rows = step.get("expect", [])
            field_types = step.get("_field_types") or ("I" * (len(rows[0]) if rows else 1))
            lines += [
                _query_header(field_types, step.get("_sort", "")),
                render_template(step["query"], dialect),
                "----",
            ]
            for row in rows:
                lines.append("\t".join(str(v) for v in row))
            lines.append("")
    if spec.get("teardown"):
        for stmt in split_sql(render_template(spec["teardown"], dialect)):
            lines += ["statement ok", stmt, ""]
    return "\n".join(lines)


def _pg_format_value(v) -> str:
    """Python 值 → PG psql 默认输出规范字符串. None/bool/float 先按 Python 类型
    处理 (保精度), 字符串走 _normalize.normalize_cell_str 兜底."""
    if v is None:
        return ""
    if isinstance(v, bool):
        return "t" if v else "f"
    if isinstance(v, float):
        return str(int(v)) if v.is_integer() else repr(v)
    return normalize_cell_str(str(v))


def emit_pg(spec: dict, dialect: dict) -> tuple[str, str]:
    """返回 (sql_file, expected_out) 二元组. expected 按 PG psql 默认输出规范."""
    sql_lines = [
        f"-- spec: {spec['name']}",
        "-- 抑制 NOTICE/WARNING (扩展输出噪声), 让 expected 纯净",
        "SET client_min_messages = ERROR;",
        "",
    ]
    exp_lines: list[str] = []
    if spec.get("setup"):
        rendered = render_template(spec["setup"], dialect)
        sql_lines.append(rendered.rstrip() + "\n")
    for step in spec.get("steps", []):
        if "raw_directive" in step:
            d = step["raw_directive"]
            if d == "restart":
                sql_lines.append("-- @restart")
            else:
                sql_lines.append(f"-- raw: {d}")
                if step.get("raw_body"):
                    sql_lines.append("-- " + step["raw_body"].replace("\n", "\n-- "))
            continue
        if "statement" in step and "expect_error" not in step:
            for stmt in split_sql(render_template(step["statement"], dialect)):
                sql_lines.append(stmt + ";")
        elif "expect_error" in step:
            q = step.get("query") or step.get("statement", "")
            sql_lines.append(render_template(q, dialect).rstrip(";") + ";")
            # PG ERROR 文本与 DuckDB 不同, 这里只标记位置, runner 端用宽松匹配
            exp_lines.append(f"ERROR:  {step['expect_error']}")
        elif "query" in step:
            rendered = render_template(step["query"], dialect).rstrip(";") + ";"
            sql_lines.append(rendered)
            # 标了 sort 的 query: PG runner 不会自动排序 actual, 这里也对 expected
            # 按相同语义排序写出 → 跟 DuckDB sqllogictest sort 行为对齐 (避免 ANN
            # 顺序不稳定造成假 fail; runner 那侧需要对 actual 也做相同排序).
            rows = _sort_rows(step.get("expect", []), step.get("_sort", ""))
            for row in rows:
                # PG 多列用 '|' (不带空格, psql -A -F'|' 模式)
                exp_lines.append("|".join(_pg_format_value(v) for v in row))
    if spec.get("teardown"):
        sql_lines.append(render_template(spec["teardown"], dialect).rstrip() + "\n")
    return "\n".join(sql_lines) + "\n", "\n".join(exp_lines) + "\n"


def emit_python(spec: dict, dialect: dict) -> str:
    """pytest 文件 - DuckDB Python binding 用."""
    name = spec["name"].replace("-", "_")
    lines = [
        '"""auto-generated from spec; do not edit."""',
        "import duckdb",
        "import pytest",
        "",
        f"def test_{name}():",
        "    con = duckdb.connect()",
        "    con.execute(\"INSTALL vex; LOAD vex;\")",
    ]
    if spec.get("setup"):
        for stmt in split_sql(render_template(spec["setup"], dialect)):
            lines.append(f"    con.execute({stmt!r})")
    for step in spec.get("steps", []):
        if "raw_directive" in step:
            lines.append(f"    # raw: {step['raw_directive']}")
            continue
        if "statement" in step and "expect_error" not in step:
            for stmt in split_sql(render_template(step["statement"], dialect)):
                lines.append(f"    con.execute({stmt!r})")
        elif "expect_error" in step:
            q = step.get("query", step.get("statement", ""))
            lines += [
                f"    with pytest.raises(Exception, match={step['expect_error']!r}):",
                f"        con.execute({render_template(q, dialect)!r}).fetchall()",
            ]
        elif "query" in step:
            q = render_template(step["query"], dialect)
            sort_mod = step.get("_sort", "")
            expected = _sort_rows(step.get("expect", []), sort_mod)
            sort_op = ""
            if sort_mod.split()[:1] == ["rowsort"]:
                sort_op = "    rows = sorted(rows, key=lambda r: tuple(str(v) for v in r))"
            elif sort_mod.split()[:1] in (["sort"], ["valuesort"]):
                sort_op = (
                    "    flat = sorted(str(v) for r in rows for v in r); "
                    "ncols = len(rows[0]) if rows else 1; "
                    "rows = [flat[i:i+ncols] for i in range(0, len(flat), ncols)]"
                )
            lines.append(f"    rows = con.execute({q!r}).fetchall()")
            if sort_op:
                lines.append(sort_op)
            lines.append(f"    assert rows == {[tuple(r) for r in expected]!r}")
    if spec.get("teardown"):
        for stmt in split_sql(render_template(spec["teardown"], dialect)):
            lines.append(f"    con.execute({stmt!r})")
    return "\n".join(lines) + "\n"


def split_sql(block: str) -> list[str]:
    """按 ; 切多语句, 忽略空白."""
    out, cur, in_quote = [], [], False
    for ch in block:
        if ch == "'":
            in_quote = not in_quote
        if ch == ";" and not in_quote:
            stmt = "".join(cur).strip()
            if stmt:
                out.append(stmt)
            cur = []
        else:
            cur.append(ch)
    last = "".join(cur).strip()
    if last:
        out.append(last)
    return out


EMITTERS = {
    "duckdb": emit_duckdb,
    "pg": emit_pg,
    "opengauss": emit_pg,
    "python": emit_python,
}


# 引擎 → 目录: 物理目录决定 spec 适用范围, 不再用 lint hardcode skip
ENGINE_DIRS = {
    "duckdb":    ["shared", "duckdb"],
    "pg":        ["shared", "pg"],
    "opengauss": ["shared", "pg", "opengauss"],   # opengauss = pg 兼容 + 自家特性
    "sqlite":    ["shared", "sqlite"],
    "python":    ["shared", "duckdb"],            # python 用 DuckDB 后端
}


def discover_specs(spec_root: Path, engine: str) -> list[Path]:
    """根据引擎从对应物理目录收集 spec."""
    dirs = ENGINE_DIRS.get(engine, ["shared"])
    paths: list[Path] = []
    for d in dirs:
        sub = spec_root / d
        if sub.exists():
            paths.extend(p for p in sub.rglob("*.yaml") if "_lib" not in p.parts)
    return sorted(paths)


def render_one(spec_path: Path, engine: str, out_root: Path, dialects: dict) -> list[Path]:
    spec = yaml.safe_load(spec_path.read_text())
    # spec.skip 仍然支持 (用作精确豁免, 例如 shared/ 下少数 spec 在 opengauss 不可用)
    if spec.get("skip", {}).get(engine):
        return []
    dialect = dialects.get(engine if engine != "python" else "duckdb")
    if dialect is None:
        raise KeyError(f"no dialect for engine {engine}")
    rel = spec_path.relative_to(ROOT).with_suffix("")
    name = spec["name"]
    written: list[Path] = []
    if engine == "duckdb":
        out = out_root / f"{name}.test"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(emit_duckdb(spec, dialect))
        written.append(out)
    elif engine in ("pg", "opengauss"):
        sql, exp = emit_pg(spec, dialect)
        sql_out = out_root / "sql" / f"{name}.sql"
        exp_out = out_root / "expected" / f"{name}.out"
        sql_out.parent.mkdir(parents=True, exist_ok=True)
        exp_out.parent.mkdir(parents=True, exist_ok=True)
        sql_out.write_text(sql)
        exp_out.write_text(exp)
        written += [sql_out, exp_out]
    elif engine == "python":
        out = out_root / f"test_{name}.py"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(emit_python(spec, dialect))
        written.append(out)
    return written


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True, choices=list(EMITTERS) + ["all"])
    parser.add_argument("--out", default="build/spec")
    parser.add_argument("--check", action="store_true", help="校验产物已存在 (CI 用)")
    args = parser.parse_args()

    dialects = load_dialects()
    spec_root = ROOT
    project_root = ROOT.parent.parent
    out_base = (project_root / args.out).resolve()

    engines = list(EMITTERS) if args.engine == "all" else [args.engine]
    total = 0
    for eng in engines:
        eng_out = out_base / eng
        specs = discover_specs(spec_root, eng)
        print(f"[{eng}] sourcing from {ENGINE_DIRS.get(eng, ['shared'])}: {len(specs)} spec(s)")
        for spec_path in specs:
            written = render_one(spec_path, eng, eng_out, dialects)
            total += len(written)
    print(f"\nrendered {total} files across {len(engines)} engine(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
