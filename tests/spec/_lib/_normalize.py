"""共享: cell 规范化 / 数值 coerce / NULL & bool token 集合.

被以下文件复用:
    docker/compare.py        — actual 与 expected 字符串比较
    render.py:_pg_format_value() — Python 值 → PG 风格字符串
    migrate_test_to_yaml.py:_coerce — sqllogictest expect 行解析
    migrate_pg_sql_to_yaml.py — PG `-- expected:` 注释解析
"""
from __future__ import annotations

NULL_TOKENS = frozenset({"", "NULL", "(empty)", "(null)"})
BOOL_TRUE_TOKENS = frozenset({"true", "t"})
BOOL_FALSE_TOKENS = frozenset({"false", "f"})


def normalize_cell_str(s: str) -> str:
    """两侧 strip + 把 NULL/(empty) 同义词 → ''; bool 同义词 → 't'/'f'.
    其余原样返回."""
    s = s.strip()
    if s in NULL_TOKENS:
        return ""
    low = s.lower()
    if low in BOOL_TRUE_TOKENS:
        return "t"
    if low in BOOL_FALSE_TOKENS:
        return "f"
    return s


def coerce_scalar(s: str):
    """字符串 → int / float / 原 string; '' / NULL token → None.
    给 yaml expect 反序列化和 PG `-- expected:` 注释解析共用."""
    s = s.strip()
    if s in NULL_TOKENS:
        return None
    for caster in (int, float):
        try:
            return caster(s)
        except ValueError:
            continue
    return s
