#!/usr/bin/env python3
"""SQLite spec runner 主体（由 run_sqlite.sh 调用）。

逐 spec：独立临时 db + 同一连接逐条执行（事务语义正确）；@restart 关闭重开；
@expect-error 的下一条语句必须失败；query 输出与 expected 走 compare.py 容差对比。
需要 python 构建带 enable_load_extension（anaconda 满足；macOS 系统 python 不行）。
"""
from __future__ import annotations

import os
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_sql(text: str):
    """→ list[段]，段 = list[(kind, stmt, sort)]，kind ∈ {sql, err}。段边界 = @restart。"""
    segments: list[list[tuple[str, str, bool]]] = [[]]
    expect_err = False
    sort_next = False
    buf = ""
    for line in text.splitlines():
        s = line.strip()
        if s == "-- @restart":
            segments.append([])
            continue
        if s == "-- @expect-error":
            expect_err = True
            continue
        if s == "-- @sort":
            sort_next = True
            continue
        if s.startswith("--"):
            continue
        if not s and not buf:
            continue
        buf += line + "\n"
        if s.endswith(";"):
            segments[-1].append(("err" if expect_err else "sql", buf.strip(), sort_next))
            buf = ""
            expect_err = False
            sort_next = False
    return segments


def fmt_cell(v) -> str:
    if v is None:
        return ""
    if isinstance(v, float):
        return str(int(v)) if v.is_integer() else repr(v)
    if isinstance(v, bytes):
        return v.hex().upper()
    return str(v)


def run_spec(sql_path: Path, ext_path: str, actual_path: Path) -> bool:
    segments = parse_sql(sql_path.read_text())
    db_path = tempfile.mktemp(suffix=".db", prefix="vexspec_")
    rows_out: list[str] = []
    ok = True
    try:
        for seg in segments:
            # isolation_level=None：autocommit，BEGIN/COMMIT/ROLLBACK 全交 SQL 层
            # （python 默认的隐式事务管理会与用例里的 BEGIN 冲突）。
            conn = sqlite3.connect(db_path, isolation_level=None)
            conn.enable_load_extension(True)
            conn.load_extension(ext_path)
            for kind, stmt, sort in seg:
                try:
                    cur = conn.execute(stmt)
                    rows = cur.fetchall()
                    if kind == "err":
                        rows_out.append(f"@EXPECTED-ERROR-BUT-SUCCEEDED: {stmt[:120]}")
                        ok = False
                    else:
                        lines = ["|".join(fmt_cell(c) for c in r) for r in rows]
                        if sort:
                            lines.sort()
                        rows_out.extend(lines)
                except sqlite3.Error as e:
                    if kind != "err":
                        rows_out.append(f"@UNEXPECTED-ERROR: {e} :: {stmt[:120]}")
                        ok = False
            conn.close()
    finally:
        for suffix in ("", "-wal", "-shm"):
            try:
                os.unlink(db_path + suffix)
            except FileNotFoundError:
                pass
    actual_path.write_text("\n".join(rows_out) + ("\n" if rows_out else ""))
    return ok


def main() -> int:
    root = Path(sys.argv[1])
    filt = sys.argv[2] if len(sys.argv) > 2 else ""
    spec_dir = root / "build/spec/sqlite"
    results = spec_dir / "results"
    results.mkdir(parents=True, exist_ok=True)
    for old in results.glob("*.diff"):
        old.unlink()

    configured_ext = sys.argv[3] if len(sys.argv) > 3 else ""
    if configured_ext:
        ext_path = Path(configured_ext).resolve()
        if not ext_path.is_file():
            print(f"显式指定的 loadable 扩展不存在: {ext_path}", file=sys.stderr)
            return 1
        ext = str(ext_path)
    else:
        ext = str(root / "vexdb_sqlite/build/vexdb_lite")
        if not (Path(ext + ".dylib").exists() or Path(ext + ".so").exists()):
            print("loadable 扩展不存在，先 bash build_sqlite.sh build", file=sys.stderr)
            return 1

    compare = root / "tests/spec/_lib/docker/compare.py"
    passed, failed, failed_names = 0, 0, []
    for sql_file in sorted((spec_dir / "sql").glob("*.sql")):
        name = sql_file.stem
        if filt and filt not in name:
            continue
        expected = spec_dir / "expected" / f"{name}.out"
        actual = results / f"{name}.actual"
        diff = results / f"{name}.diff"
        run_ok = run_spec(sql_file, ext, actual)
        cmp_ok = False
        if run_ok and expected.exists():
            with diff.open("w") as df:
                cmp_ok = subprocess.run(
                    [sys.executable, str(compare), str(expected), str(actual)],
                    stderr=df).returncode == 0
        if run_ok and cmp_ok:
            passed += 1
            diff.unlink(missing_ok=True)
        else:
            failed += 1
            failed_names.append(name)
            if not run_ok:
                with diff.open("a") as df:
                    for line in actual.read_text().splitlines():
                        if line.startswith("@"):
                            df.write(line + "\n")

    print(f"sqlite spec: {passed} passed, {failed} failed")
    if failed_names:
        print("failed: " + " ".join(failed_names))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
