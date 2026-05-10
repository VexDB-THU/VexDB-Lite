#!/usr/bin/env python3
"""把现有的 DuckDB .test (sqllogictest) 文件反向迁移到 spec YAML.

策略: 每个 sqllogictest block (statement ok/error 或 query) → 一条 steps 条目.
不区分 setup/teardown - 完全保留出现顺序, 语义无损.
人工 review 时可按需重组 setup/teardown.

变量反向替换是 best-effort: 只识别能高置信度还原的模式.

用法:
    单文件:
        python tests/spec/_lib/migrate_test_to_yaml.py \
            --src duckdb/vexdb-duck/test/sql/vex/index/graph_index_basic.test \
            --out tests/spec/index/graph_index_basic.yaml

    批量 (按目录映射: functions/* → tests/spec/functions/*, etc.):
        python tests/spec/_lib/migrate_test_to_yaml.py --batch \
            --src-root duckdb/vexdb-duck/test/sql/vex \
            --out-root tests/spec
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml


# ───────────────── 反向模板化 (DuckDB → spec 变量) ─────────────────
#
# 高置信度模式: 仅替换不会误伤的, 否则保留原文等人工调整.

REVERSE_PATTERNS: list[tuple[re.Pattern, object]] = [
    # FLOAT[N]                       → ${VECTOR(N)}
    (re.compile(r"\bFLOAT\[(\d+)\]"), lambda m: f"${{VECTOR({m.group(1)})}}"),
    # GRAPH_INDEX                    → ${VEX_INDEX}
    (re.compile(r"\bGRAPH_INDEX\b"), "${VEX_INDEX}"),
    # duckdb_indexes()               → ${SYS_INDEXES}
    (re.compile(r"\bduckdb_indexes\(\)"), "${SYS_INDEXES}"),
    # 系统表字段 (在 sqllogictest 中用作系统列名, 用户列罕用此名 - 全局替换)
    (re.compile(r"\bindex_name\b"), "${SYS_INDEXES_NAME}"),
    (re.compile(r"\btable_name\b"), "${SYS_INDEXES_TABLE}"),
    # 运行时 GUC
    (re.compile(r"\bvex_ef_search\b"), "${GUC_EF_SEARCH}"),
    (re.compile(r"\bvex_brute_force_threshold\b"), "${GUC_BF_THRESHOLD}"),
    (re.compile(r"\bvex_pq_search_mode\b"), "${GUC_PQ_SEARCH_MODE}"),
    (re.compile(r"\bvex_pq_refine\b"), "${GUC_PQ_REFINE}"),
    (re.compile(r"\bvex_parallel_search\b"), "${GUC_PARALLEL_SEARCH}"),
    # range(n)  → ${RANGE(n)}    (支持负号/数字)
    (re.compile(r"\brange\((\s*\d+\s*)\)"), lambda m: f"${{RANGE({m.group(1).strip()})}}"),
    # range(a, b) → ${RANGE2(a, b)}
    (re.compile(r"\brange\((\s*\d+\s*),(\s*\d+\s*)\)"), lambda m: f"${{RANGE2({m.group(1).strip()}, {m.group(2).strip()})}}"),
]


# CREATE INDEX 反向模板化:
#   `... USING ${VEX_INDEX} (col) WITH (metric='X', other=...)` →
#   `... USING ${VEX_INDEX} (col${OPS_X_COL})${IDX_OPTS_X}` 当只有 metric 时
#   `... USING ${VEX_INDEX} (col${OPS_X_COL}) WITH (other=...)` 当还有其他参数时

_CREATE_INDEX_HEAD_RE = re.compile(
    r"(CREATE\s+(?:UNIQUE\s+)?INDEX\s+\S+\s+ON\s+\S+\s+USING\s+\$\{VEX_INDEX\}\s*\([^)]*?)\)(\s*WITH\s*\(([^)]*)\))?",
    re.IGNORECASE,
)


def _templatize_create_index(sql: str) -> str:
    def repl(m: re.Match) -> str:
        head = m.group(1)
        with_clause = m.group(2)
        with_body = m.group(3) or ""

        if not with_clause:
            return f"{head}${{OPS_L2_COL}}){'${IDX_OPTS_L2}'}"

        # 解析 WITH 体内的 k=v 对; 非 metric 的保留原始字符串 (含空格)
        # 否则 `memory_mode = 'compact'` → `memory_mode='compact'` 会让 statement
        # error 期望文本对不上 (legacy 严格按字符 diff)
        params = []
        metric = None
        for kv in _split_kv(with_body):
            k, _, v = kv.partition("=")
            if k.strip().lower() == "metric":
                metric = v.strip().strip("'\"").lower()
            else:
                params.append(kv.strip())  # 保留原始 "k = v" / "k=v" 格式
        if metric not in ("l2", "cosine", "ip"):
            metric = "l2"
        ops = {"l2": "${OPS_L2_COL}", "cosine": "${OPS_COSINE_COL}", "ip": "${OPS_IP_COL}"}[metric]

        if not params:
            opts = {"l2": "${IDX_OPTS_L2}", "cosine": "${IDX_OPTS_COSINE}", "ip": "${IDX_OPTS_IP}"}[metric]
            return f"{head}{ops}){opts}"
        # 有额外参数: 用 ${IDX_WITH_X(p1=v1, p2=v2)} 函数变量
        # DuckDB 渲染会重新嵌入 metric, PG 渲染只保留其他参数
        with_fn = {"l2": "IDX_WITH_L2", "cosine": "IDX_WITH_COSINE", "ip": "IDX_WITH_IP"}[metric]
        joined = ", ".join(params)
        return f"{head}{ops})${{{with_fn}({joined})}}"

    return _CREATE_INDEX_HEAD_RE.sub(repl, sql)


def _split_kv(body: str) -> list[str]:
    out, cur, in_quote, depth = [], [], False, 0
    for ch in body:
        if ch == "'":
            in_quote = not in_quote
        if not in_quote:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == "," and depth == 0:
                if "".join(cur).strip():
                    out.append("".join(cur).strip())
                cur = []
                continue
        cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def replace_balanced_call(text: str, fn_name: str, template: str) -> str:
    """把 fn_name(arg1, arg2) 替换为 template.format(arg1, arg2), 支持嵌套括号."""
    out = []
    i = 0
    pat = re.compile(rf"\b{re.escape(fn_name)}\s*\(")
    while i < len(text):
        m = pat.search(text, i)
        if not m:
            out.append(text[i:])
            break
        out.append(text[i : m.start()])
        # 从 m.end() 开始平衡括号扫描
        depth = 1
        j = m.end()
        in_quote = False
        in_bracket = 0
        while j < len(text) and depth > 0:
            c = text[j]
            if c == "'" and (j == 0 or text[j - 1] != "\\"):
                in_quote = not in_quote
            elif not in_quote:
                if c == "[":
                    in_bracket += 1
                elif c == "]":
                    in_bracket -= 1
                elif c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
            j += 1
        # text[m.end():j-1] 是参数体
        body = text[m.end() : j - 1]
        # 用顶层逗号切两个参数
        args = split_top_level_commas(body)
        if len(args) == 2:
            out.append(template.format(args[0].strip(), args[1].strip()))
        else:
            # 参数数量不符, 保留原文
            out.append(text[m.start() : j])
        i = j
    return "".join(out)


def split_top_level_commas(s: str) -> list[str]:
    out, cur = [], []
    depth_p, depth_b, in_quote = 0, 0, False
    for ch in s:
        if ch == "'":
            in_quote = not in_quote
        if not in_quote:
            if ch == "(":
                depth_p += 1
            elif ch == ")":
                depth_p -= 1
            elif ch == "[":
                depth_b += 1
            elif ch == "]":
                depth_b -= 1
            elif ch == "," and depth_p == 0 and depth_b == 0:
                out.append("".join(cur))
                cur = []
                continue
        cur.append(ch)
    if cur:
        out.append("".join(cur))
    return out


def templatize(sql: str) -> str:
    # 1) 向量字面量 [a, b, c]::FLOAT[N] → ${VEC_LITERAL([a,b,c], N)}
    #    要在 FLOAT[N] → ${VECTOR(N)} 之前完成
    sql = _templatize_vec_literals(sql)
    for pat, repl in REVERSE_PATTERNS:
        sql = pat.sub(repl, sql) if not callable(repl) else pat.sub(repl, sql)
    sql = replace_balanced_call(sql, "l2_distance", "${{L2({0}, {1})}}")
    sql = replace_balanced_call(sql, "cosine_distance", "${{COSINE({0}, {1})}}")
    sql = replace_balanced_call(sql, "inner_product", "${{IP({0}, {1})}}")
    # 2) CREATE INDEX 模式: 必须在 GRAPH_INDEX 已替换为 ${VEX_INDEX} 之后
    sql = _templatize_create_index(sql)
    return sql


_VEC_LITERAL_RE = re.compile(r"(\[[^\[\]]*\])::FLOAT\[(\d+)\]")


def _templatize_vec_literals(sql: str) -> str:
    """`[1.0, 2.0, 3.0]::FLOAT[3]` → `${VEC_LITERAL([1.0, 2.0, 3.0], 3)}`.
    只匹配不含嵌套 [] 的字面量, 避免误伤 list_transform 等表达式."""
    return _VEC_LITERAL_RE.sub(lambda m: f"${{VEC_LITERAL({m.group(1)}, {m.group(2)})}}", sql)


# ───────────────── sqllogictest 解析 ─────────────────

HEADER_DESC = re.compile(r"^#\s*description:\s*(.+)$", re.MULTILINE)
HEADER_GROUP = re.compile(r"^#\s*group:\s*\[(.+?)\]", re.MULTILINE)
HEADER_NAME = re.compile(r"^#\s*name:\s*(.+)$", re.MULTILINE)


def parse_test_file(text: str) -> dict:
    desc_m = HEADER_DESC.search(text)
    group_m = HEADER_GROUP.search(text)
    description = desc_m.group(1).strip() if desc_m else ""
    tags = [g.strip() for g in (group_m.group(1).split(",") if group_m else [])]

    steps: list[dict] = []

    # 按 "行首是 statement/query/控制流 关键字" 切块
    # 用 [ \t] 替代 \s 避免跨行匹配 (\s 在 MULTILINE 下匹配 \n)
    # query 后允许可选的 sort modifier (rowsort/valuesort/nosort) + label
    pattern = re.compile(
        r"^(?P<header>(?:statement[ \t]+(?:ok|error)|"
        r"query[ \t]+\S+(?:[ \t]+\S+)*|"  # query I / query I rowsort / query III rowsort label
        r"onlyif[ \t]+\S+|skipif[ \t]+\S+|"
        r"loop[ \t]+.+|endloop|"
        r"concurrentloop[ \t]+.+|endconcurrentloop|"
        r"foreach[ \t]+.+|endforeach|"
        r"halt|restart|"
        r"load[ \t]+\S+|"
        r"hash-threshold[ \t]+\S+|mode[ \t]+\S+|"
        r"require[ \t]+\S+|require-env[ \t]+\S+))[ \t]*$",
        re.MULTILINE,
    )
    indices = [m.start() for m in pattern.finditer(text)]
    indices.append(len(text))
    for k in range(len(indices) - 1):
        block = text[indices[k] : indices[k + 1]]
        block_stripped = block.rstrip()
        first_nl = block_stripped.find("\n")
        header = block_stripped[:first_nl] if first_nl >= 0 else block_stripped
        rest = block_stripped[first_nl + 1 :] if first_nl >= 0 else ""

        if header.startswith("mode") or header.startswith("hash-threshold"):
            continue
        if header in ("halt",):
            continue
        if header == "restart":
            steps.append({"raw_directive": "restart"})
            continue
        if header.startswith("require"):
            # require vex 在 emit_duckdb 已默认加, 其他 require 需保留 (parquet/json 等)
            req_what = header.split(maxsplit=1)[1] if " " in header else ""
            if req_what.lower() != "vex":
                steps.append({"raw_directive": header})
            continue
        if header.startswith("load "):
            # load <path> - DuckDB 持久化数据库切换. 保留指令 + 路径, 渲染时还原
            steps.append({"raw_directive": header})
            continue
        if header in ("endloop", "endconcurrentloop", "endforeach"):
            steps.append({"raw_directive": header})
            continue
        if header.startswith(("onlyif", "skipif", "loop", "concurrentloop", "foreach")):
            # 控制流指令 - 整体保留为 raw, 渲染时原样输出
            steps.append({"raw_directive": header, "raw_body": rest.strip()})
            continue

        if header.startswith("statement ok"):
            sql = _strip_block_trailing_blanks(rest)
            if sql:
                steps.append({"statement": templatize(sql)})
        elif header.startswith("statement error"):
            # ---- 后允许直接 EOF 或 \n
            split_m = re.search(r"\n----\s*(?:\n|\Z)", rest)
            if split_m:
                sql = _strip_block_trailing_blanks(rest[: split_m.start()])
                err = rest[split_m.end():].strip()
            else:
                sql = _strip_block_trailing_blanks(rest)
                err = ""
            err = _strip_trailing_comments(err).strip()
            steps.append({"statement": templatize(sql), "expect_error": err})
        elif header.startswith("query"):
            # header: "query I" / "query III rowsort" / "query I sort" / "query I rowsort label"
            # 第一个 token 是字段类型串 (I=int / R=real / T=text 的组合);
            # 余下是 sort modifier (sort/rowsort/valuesort/nosort) + 可选 label.
            parts_h = header.split(maxsplit=1)
            args_after = parts_h[1] if len(parts_h) > 1 else "I"
            tokens = args_after.split()
            field_types = tokens[0] if tokens else "I"
            sort_modifier = " ".join(tokens[1:])  # 例如 "sort" / "rowsort" / "rowsort label"
            # ---- 后允许 trailing 空白 / EOF (block.rstrip 后可能只剩 '----' 末尾)
            split_m = re.search(r"\n----[ \t]*(?:\n|\Z)", rest)
            if split_m:
                sql = _strip_block_trailing_blanks(rest[: split_m.start()])
                expected_block = rest[split_m.end():]
            else:
                sql = _strip_block_trailing_blanks(rest)
                expected_block = ""
            rows = _parse_expected_rows(expected_block, len(field_types))
            step: dict = {"query": templatize(sql), "_field_types": field_types}
            if sort_modifier:
                step["_sort"] = sort_modifier
            step["expect"] = rows
            steps.append(step)

    return {"description": description, "tags": tags, "steps": steps}


def _strip_block_trailing_blanks(s: str) -> str:
    """去掉 block 末尾的空行 + # 注释行 (sqllogictest block 之间的注释会粘到上一个 block 末尾)."""
    lines = s.splitlines()
    while lines and (not lines[-1].strip() or lines[-1].lstrip().startswith("#")):
        lines.pop()
    return "\n".join(lines)


# 别名 (历史名, 同义)
_strip_trailing_comments = _strip_block_trailing_blanks


def _parse_expected_rows(block: str, ncols: int) -> list[list]:
    """解析 expected 区. 跳过空行和 # 注释行. 遇到第一个非数据行后停止."""
    rows: list[list] = []
    for line in block.splitlines():
        if not line.strip():
            # 第一个空行后, 通常是下一个 block 的间隔
            if rows:
                break
            continue
        if line.lstrip().startswith("#"):
            # 注释行 - 数据区不应该有, 但分割可能误带, 跳过
            if rows:
                break
            continue
        cells = line.split("\t") if "\t" in line else [line]
        if len(cells) < ncols:
            # padding
            cells = cells + [""] * (ncols - len(cells))
        rows.append([_coerce(c) for c in cells])
    return rows


def _coerce(s: str):
    s = s.rstrip()
    if s in ("NULL", "(empty)"):
        return s
    try:
        return int(s)
    except ValueError:
        try:
            return float(s)
        except ValueError:
            return s


# ───────────────── 入口 ─────────────────

def migrate_one(src: Path, out: Path, name: str | None = None, extra_tags: list[str] | None = None) -> dict:
    text = src.read_text()
    spec = parse_test_file(text)
    spec_out: dict = {
        "name": name or src.stem,
        "tags": list(dict.fromkeys((spec["tags"] or []) + (extra_tags or []))) or ["migrated"],
        "engines": ["duckdb", "pg", "opengauss"],
        "description": spec["description"],
        "steps": spec["steps"],
        "_migrated_from": str(src),
    }
    if not spec_out["description"]:
        del spec_out["description"]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        "# auto-migrated from sqllogictest; review before relying on it\n"
        "# 1. setup/teardown 未自动切分, 全在 steps; 必要时人工重组\n"
        "# 2. 变量反向替换仅做高置信度模式 (FLOAT[N]/GRAPH_INDEX/距离函数)\n"
        "# 3. 复杂指令 (loop/onlyif/skipif) 保留为 raw_directive\n"
        + yaml.safe_dump(spec_out, sort_keys=False, allow_unicode=True, width=200)
    )
    return {"src": str(src), "out": str(out), "step_count": len(spec["steps"])}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src")
    ap.add_argument("--out")
    ap.add_argument("--name")
    ap.add_argument("--tags", default="")
    ap.add_argument("--batch", action="store_true")
    ap.add_argument("--src-root")
    ap.add_argument("--out-root")
    ap.add_argument("--include-slow", action="store_true", help="也迁移 .test_slow")
    args = ap.parse_args()

    extra_tags = [t.strip() for t in args.tags.split(",") if t.strip()]

    if args.batch:
        if not (args.src_root and args.out_root):
            ap.error("--batch requires --src-root and --out-root")
        src_root = Path(args.src_root)
        out_root = Path(args.out_root)
        patterns = ["*.test"]
        if args.include_slow:
            patterns.append("*.test_slow")
        results = []
        errors = []
        for pat in patterns:
            for src in sorted(src_root.rglob(pat)):
                if src.suffix == ".test_slow":
                    rel = src.relative_to(src_root).with_suffix("")
                    rel = rel.with_name(rel.name + "_slow.yaml")
                else:
                    rel = src.relative_to(src_root).with_suffix(".yaml")
                out = out_root / rel
                slow_tags = ["slow"] if src.suffix == ".test_slow" else []
                # name = 相对路径打平 (如 index/graph_index_basic), 避免与新写用例冲突
                name_path = src.relative_to(src_root).with_suffix("")
                if src.suffix == ".test_slow":
                    name_path = name_path.with_name(name_path.name + "_slow")
                migrated_name = str(name_path).replace("/", "__")
                try:
                    info = migrate_one(src, out, name=migrated_name, extra_tags=(extra_tags or []) + slow_tags)
                    results.append(info)
                    print(f"  ✓ {src.relative_to(src_root)} → {out.relative_to(out_root)} ({info['step_count']} steps)")
                except Exception as e:
                    errors.append((str(src), str(e)))
                    print(f"  ✗ {src}: {e}", file=sys.stderr)
        print(f"\n迁移完成: {len(results)} 成功 / {len(errors)} 失败")
        if errors:
            print("失败列表:")
            for s, e in errors:
                print(f"  {s}: {e}")
            return 1
        return 0

    if not (args.src and args.out):
        ap.error("非 batch 模式需要 --src 和 --out")
    info = migrate_one(Path(args.src), Path(args.out), name=args.name, extra_tags=extra_tags)
    print(f"wrote {info['out']} ({info['step_count']} steps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
