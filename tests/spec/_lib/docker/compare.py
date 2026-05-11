#!/usr/bin/env python3
"""带容差的 expected vs actual 比较器, 替代简单 diff.

容忍:
  - 浮点末位误差 (rtol=1e-3, atol=1e-4): 2 == 1.9999998807907104
  - NULL ↔ 空字符串
  - 'NULL' / '(empty)' ↔ ''
  - 'ERROR:  XXX' 文本宽松匹配 (PG/DuckDB 报错文本不同, 看到 ERROR 就算对)

不容忍:
  - 行数不同
  - 字段数不同
  - 字符串字面值不一致 (除上述特殊处理)

退出码: 0 = 等价 / 1 = 有差异. 差异输出到 stderr (类 unified-diff).
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

# 共享 normalize: ../_normalize.py
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _normalize import normalize_cell_str  # noqa: E402

ATOL = 1e-4
RTOL = 1e-3
normalize_cell = normalize_cell_str  # 历史名别名


def try_float(s: str):
    try:
        return float(s)
    except ValueError:
        return None


def parse_vector_literal(s: str):
    """`[1.0, 2.0, 3.0]` / `[1,2,3]` → [1.0, 2.0, 3.0] (浮点列表), 不匹配返回 None."""
    s = s.strip()
    if not (s.startswith("[") and s.endswith("]")):
        return None
    inner = s[1:-1].strip()
    if not inner:
        return []
    try:
        return [float(x.strip()) for x in inner.split(",")]
    except ValueError:
        return None


def cells_equiv(exp: str, act: str) -> bool:
    e = normalize_cell(exp)
    a = normalize_cell(act)
    if e == a:
        return True
    # 浮点容差
    fe, fa = try_float(e), try_float(a)
    if fe is not None and fa is not None:
        if math.isnan(fe) and math.isnan(fa):
            return True
        if math.isinf(fe) or math.isinf(fa):
            return fe == fa
        return abs(fe - fa) <= ATOL + RTOL * max(abs(fe), abs(fa))
    # 向量字面量: 元素级容差比较
    ve, va = parse_vector_literal(e), parse_vector_literal(a)
    if ve is not None and va is not None:
        if len(ve) != len(va):
            return False
        return all(abs(x - y) <= ATOL + RTOL * max(abs(x), abs(y)) for x, y in zip(ve, va))
    return False


def line_equiv(exp: str, act: str) -> bool:
    # 错误行宽松匹配: 都以 ERROR 开头就算对 (PG/DuckDB 报错文本不同)
    if exp.lstrip().startswith("ERROR") and act.lstrip().startswith("ERROR"):
        return True
    # 列分隔优先用 \x1f (ASCII unit separator, 安全分隔符), 兼容老的 |
    sep = "\x1f" if "\x1f" in exp or "\x1f" in act else "|"
    exp_cells = exp.split(sep)
    act_cells = act.split(sep)
    if len(exp_cells) != len(act_cells):
        return False
    return all(cells_equiv(e, a) for e, a in zip(exp_cells, act_cells))


def read_lines(path: Path) -> list[str]:
    """读全部行 (rstrip \\r\\n). 仅去两端空行, 中间空行保留 - 否则会错位静默通过:
    expected 第 5 行空但 actual 第 5 行有内容, 过滤后两边长度一样会被误判等价."""
    if not path.exists():
        return []
    raw = [ln.rstrip("\r\n") for ln in path.read_text().splitlines()]
    while raw and raw[0].strip() == "":
        raw.pop(0)
    while raw and raw[-1].strip() == "":
        raw.pop()
    return raw


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <expected> <actual>", file=sys.stderr)
        return 2
    exp_path, act_path = Path(sys.argv[1]), Path(sys.argv[2])
    exp = read_lines(exp_path)
    act = read_lines(act_path)

    diffs: list[str] = []
    n = max(len(exp), len(act))
    for i in range(n):
        e = exp[i] if i < len(exp) else "<missing>"
        a = act[i] if i < len(act) else "<missing>"
        if i >= len(exp) or i >= len(act) or not line_equiv(e, a):
            diffs.append(f"  line {i+1}:")
            diffs.append(f"    expected: {e}")
            diffs.append(f"    actual:   {a}")

    if diffs:
        print(f"--- {exp_path}", file=sys.stderr)
        print(f"+++ {act_path}", file=sys.stderr)
        for ln in diffs:
            print(ln, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
