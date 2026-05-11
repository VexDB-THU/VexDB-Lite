#!/usr/bin/env python3
"""把 migrated/ 下的 spec 按"是否 DuckDB-only"自动分类移动到 shared/ 或 duckdb/.

DuckDB-only 判定: lint_review.py 中 PG 不兼容模式命中.
其他 spec → shared/.
分类时同时清掉 spec 里旧的 skip:pg 字段 (不再需要).

用法:
    python tests/spec/_lib/classify.py                  # dry-run
    python tests/spec/_lib/classify.py --apply          # 实际移动
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]


# DuckDB-only 模式 (与 lint_review.py 同步, 命中即移到 duckdb/)
DUCKDB_ONLY_PATTERNS = [
    re.compile(r"\blist_transform\b"),
    re.compile(r"\bATTACH\s"),
    re.compile(r"\bDETACH\s"),
    re.compile(r"\bduckdb_\w+\("),
    re.compile(r"\bvex_index_info\b"),
    re.compile(r"\bvex_inspect\w*\b"),
    re.compile(r"::FLOAT\["),
    re.compile(r"\bEXPLAIN\s+ANALYZE\b", re.I),
    re.compile(r"\bdisabled_optimizers\b"),
    re.compile(r"\barray_inner_product\b"),
    re.compile(r"\barray_cosine_similarity\b"),
    re.compile(r"\b(?:vector_add|vector_sub|vector_mul)\b"),
    re.compile(r"<~>"),  # DuckDB-only operator
    re.compile(r"\blist_value\b"),
    re.compile(r"\blist_distance\b"),
    re.compile(r"\blist_cosine_distance\b"),
    re.compile(r"\blist_inner_product\b"),
    # 表达式构造数组: [sin(i)::FLOAT, ...] / [(i+1)::FLOAT, ...]
    re.compile(r"\[\s*\w+\([^)]*\)::"),
    # sqllogictest 物理 plan EXPLAIN (DuckDB 输出与 PG 不同, 不可对比)
    re.compile(r"\bphysical_plan\b"),
    # 裸数组字面量 (非 ${VEC_LITERAL} 模板) - PG 解析报错
    # [1.0, 2.0, 3.0] 等 (数字+逗号 必须有至少 2 个元素)
    re.compile(r"\[\s*-?\d[\d.eE+\-]*\s*,\s*-?\d"),
    re.compile(r"->\s*[a-z]"),
    re.compile(r"\$\{[a-z]\w*\}"),  # sqllogictest 循环变量
    # DuckDB array constructor 含表达式: [expr op expr, ...]::TYPE - PG 用 ARRAY[...]
    re.compile(r"\[\s*\(\s*[a-zA-Z_]"),  # [(ident...) — 表达式开头
    re.compile(r"\[\s*[a-zA-Z_]\w*\s*[,*+\-/%]"),  # [ident op ...]
    re.compile(r"\bthreads\s*=", re.I),
    # access method 不支持 multicolumn (PG_VEXDB 真不支持) → 标 duckdb-only
    re.compile(r"USING\s+(?:\$\{VEX_INDEX\}|@T@)\s*\([^)]*,\s*[^)]+\)", re.I),
    # vex_* 系统函数 (DuckDB only)
    re.compile(r"\bvex_simd_arch\b"),
    re.compile(r"\bvex_pq_search_mode\b"),
    # sqllogictest <REGEX>: 比较 (only DuckDB sqllogictest 支持)
    re.compile(r"<REGEX>:"),
    re.compile(r"<!REGEX>:"),
    # [ident*number, ...] 形式数组 - sqllogictest loop 变量内联
    re.compile(r"\[[a-z_]\w*\s*[*+\-/]"),
    # COPY ... TO/FROM ... PARQUET (DuckDB-only 格式)
    re.compile(r"\bFORMAT\s+PARQUET\b", re.I),
    re.compile(r"\bload\s+\S*\.db", re.I),
    # checkpoint 在 DuckDB sqllogictest 是控制流, PG 不直接支持 (但同名)
    # 不加 - PG 也有 CHECKPOINT, 真要差异需另外处理
]


EXPECT_DUCKDB_PATTERNS = [
    re.compile(r"<REGEX>:"),
    re.compile(r"<!REGEX>:"),
    re.compile(r"\bphysical_plan\b"),
]


_TEMPLATE_NAME_RE = re.compile(r"\$\{[A-Z_][A-Z0-9_]*")


def strip_templates(s: str) -> str:
    """剥离 ${VEC_LITERAL([(expr)::FLOAT, ...], 4)} / ${L2(a, b)} / ${SYS_INDEXES}.

    平衡扫描支持任意嵌套 () [] 和嵌套 ${...}, 替换为 @T@.
    旧实现用 `\\([^)]*\\)` 在第一个 `)` 停止, 含括号/方括号参数的模板剥不干净 →
    内部 `[(` 被裸 SQL 规则 `\\[\\s*\\(` 误判 DuckDB-only.
    """
    out: list[str] = []
    i = 0
    n = len(s)
    while i < n:
        m = _TEMPLATE_NAME_RE.match(s, i)
        if not m:
            out.append(s[i])
            i += 1
            continue
        j = m.end()
        # 可选参数 (...) - 平衡扫描
        if j < n and s[j] == "(":
            depth_p, depth_b, in_quote = 1, 0, False
            j += 1
            while j < n and depth_p > 0:
                c = s[j]
                if c == "'" and (j == 0 or s[j - 1] != "\\"):
                    in_quote = not in_quote
                elif not in_quote:
                    if c == "(":
                        depth_p += 1
                    elif c == ")":
                        depth_p -= 1
                    elif c == "[":
                        depth_b += 1
                    elif c == "]":
                        depth_b -= 1
                j += 1
        # 必须以 } 结束
        if j < n and s[j] == "}":
            out.append("@T@")
            i = j + 1
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


# 在剥离前的原文 (即模板未展开) 扫描的模式: 识别 VEC_LITERAL 内含表达式等
# DuckDB 端 dialect 渲染为 array constructor (能求值), PG 端渲染为字符串字面量 (报错)
RAW_DUCKDB_ONLY_PATTERNS = [
    re.compile(r"\$\{VEC_LITERAL\(\[[^]]*\([^)]*\)\s*::"),  # ${VEC_LITERAL([(expr)::cast, ...]
    re.compile(r"\$\{VEC_LITERAL\(\[[^]]*[a-zA-Z_]\w*\s*[*+\-/]"),  # ${VEC_LITERAL([ident*x, ...]
]


def is_duckdb_only(spec_path: Path) -> tuple[bool, str]:
    spec = yaml.safe_load(spec_path.read_text())
    for step in spec.get("steps", []):
        # 1) 扫原文 (未剥离): 找 VEC_LITERAL 内含表达式等模板级 DuckDB-only 模式
        raw_full = (step.get("statement") or step.get("query") or "")
        for pat in RAW_DUCKDB_ONLY_PATTERNS:
            m = pat.search(raw_full)
            if m:
                return True, f"raw matched: {pat.pattern} ({m.group(0)!r})"
        # 2) 扫 SQL 文本: 用 SQL 模式 (剥离已模板化的部分, 避免误判)
        raw_sql = step.get("statement") or step.get("query") or ""
        sql = strip_templates(raw_sql)
        for pat in DUCKDB_ONLY_PATTERNS:
            m = pat.search(sql)
            if m:
                return True, f"sql matched: {pat.pattern} ({m.group(0)!r})"
        # 扫 expect: 仅 sqllogictest 特殊比较语法 (REGEX / physical_plan)
        if "expect" in step:
            expect_text = " ".join(str(c) for row in step["expect"] for c in (row if isinstance(row, list) else [row]))
            for pat in EXPECT_DUCKDB_PATTERNS:
                m = pat.search(expect_text)
                if m:
                    return True, f"expect matched: {pat.pattern} ({m.group(0)!r})"
    return False, ""


def classify(apply: bool, src_subdir: str = "migrated") -> int:
    src_root = ROOT / src_subdir
    if not src_root.exists():
        print(f"source dir not found: {src_root}", file=sys.stderr)
        return 1

    duckdb_dest = ROOT / "duckdb"
    shared_dest = ROOT / "shared"
    moves: list[tuple[Path, Path, str]] = []

    for spec_path in sorted(src_root.rglob("*.yaml")):
        rel = spec_path.relative_to(src_root)  # e.g. index/foo.yaml
        is_dd, reason = is_duckdb_only(spec_path)
        target_root = duckdb_dest if is_dd else shared_dest
        target = target_root / rel
        # 同位置不动 (重分类时只挪需要变的)
        if target.resolve() == spec_path.resolve():
            continue
        moves.append((spec_path, target, reason if is_dd else "shared"))

    duckdb_count = sum(1 for _, t, _ in moves if "duckdb" in str(t))
    shared_count = len(moves) - duckdb_count
    print(f"分类结果: shared={shared_count}, duckdb={duckdb_count} (共 {len(moves)})")
    print()

    if not apply:
        print("[DRY-RUN] 加 --apply 实际移动\n")
        for src, dst, reason in moves[:5]:
            tag = "DUCKDB" if "duckdb" in str(dst) else "SHARED"
            print(f"  [{tag}] {src.name:50s} ← {reason[:60]}")
        print(f"  ... ({len(moves) - 5} more)")
        return 0

    for src, dst, reason in moves:
        dst.parent.mkdir(parents=True, exist_ok=True)
        # 同时清理旧的 skip:pg 字段, 删除 _migrated_from 元信息
        spec = yaml.safe_load(src.read_text())
        spec.pop("skip", None)
        spec.pop("_migrated_from", None)
        # 名字简化: 去掉 'index__' 前缀 (现在按目录区分了)
        if "name" in spec:
            spec["name"] = spec["name"].replace("index__", "").replace("functions__", "").replace("types__", "")
        # 修正 tags: 去掉 'migrated' (现在按目录), 加上分类 tag
        tags = [t for t in spec.get("tags", []) if t != "migrated"]
        if "duckdb" in str(dst):
            if "duckdb-only" not in tags:
                tags.append("duckdb-only")
        spec["tags"] = tags

        header = "# auto-classified spec\n"
        dst.write_text(header + yaml.safe_dump(spec, sort_keys=False, allow_unicode=True, width=200))

        # 移动: 删源
        src.unlink()

    # migrated 目录删除 (首次分类后)
    if src_subdir == "migrated" and src_root.exists():
        try:
            shutil.rmtree(src_root)
            print(f"\n移动完成. 删除旧 migrated/ 目录")
        except OSError:
            pass
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--from", dest="src", default="migrated", help="src subdir (默认 migrated; 重分类用 shared)")
    args = ap.parse_args()
    return classify(args.apply, args.src)


if __name__ == "__main__":
    sys.exit(main())
