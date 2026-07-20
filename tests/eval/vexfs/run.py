#!/usr/bin/env python3
"""VexFS 真实 eval：功能、恢复、并发、备份、性能、CLI 和 macOS mount。

每个 case 都使用真实的磁盘 SQLite 数据库和构建产物。默认产出 JSON 与 Markdown，
便于本地回归和 CI 保存；性能数字默认记录，--enforce-performance 才作为硬门槛。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import random
import resource
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import traceback
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


MIB = 1024 * 1024
MAX_FILE_BYTES = 128 * MIB
CONTRACT_VERSION = "0.3.0"


class EvalFailure(AssertionError):
    pass


class EvalSkip(RuntimeError):
    pass


@dataclass(frozen=True)
class Mode:
    name: str
    random_ops: int
    small_files: int
    sequential_mib: int
    staged_mib: int
    random_writes: int
    parallel_workers: int
    files_per_worker: int
    include_xcode: bool


MODES = {
    "quick": Mode("quick", 300, 250, 8, 8, 100, 4, 20, False),
    "full": Mode("full", 2_000, 3_000, 100, 100, 1_000, 8, 100, True),
    "stress": Mode("stress", 10_000, 10_000, 128, 128, 5_000, 8, 500, True),
}


@dataclass
class EvalCase:
    case_id: str
    category: str
    description: str
    function: Callable[["Context"], dict[str, Any] | None]
    modes: tuple[str, ...] = ("quick", "full", "stress")


CASES: list[EvalCase] = []


def case(case_id: str, category: str, description: str,
         modes: tuple[str, ...] = ("quick", "full", "stress")):
    def register(function: Callable[["Context"], dict[str, Any] | None]):
        CASES.append(EvalCase(case_id, category, description, function, modes))
        return function
    return register


@dataclass
class Context:
    root: Path
    build_dir: Path
    extension: Path
    cli: Path
    output_dir: Path
    mode: Mode
    seed: int
    enforce_performance: bool
    checks: int = 0
    current_case: str = ""
    current_artifacts: Path | None = None

    def check(self, condition: bool, message: str) -> None:
        self.checks += 1
        if not condition:
            raise EvalFailure(message)

    def equal(self, actual: Any, expected: Any, message: str) -> None:
        self.checks += 1
        if actual != expected:
            raise EvalFailure(f"{message}: expected={expected!r}, actual={actual!r}")

    def expect_error(self, function: Callable[[], Any], contains: str) -> str:
        self.checks += 1
        try:
            function()
        except (sqlite3.Error, RuntimeError) as error:
            message = str(error)
            if contains not in message:
                raise EvalFailure(
                    f"错误信息不匹配: expected contains={contains!r}, actual={message!r}")
            return message
        raise EvalFailure(f"预期失败但成功: {contains}")

    def budget(self, metric: str, actual: float, maximum: float) -> None:
        """性能预算只有显式开启才阻断；总是检查没有出现灾难性卡死。"""
        self.check(actual >= 0, f"{metric} 不能为负数")
        if self.enforce_performance:
            self.check(actual <= maximum,
                       f"{metric} 超过预算: {actual:.3f} > {maximum:.3f}")


class Database:
    def __init__(self, ctx: Context, path: Path | None = None, initialize: bool = True,
                 timeout: float = 15.0):
        self.ctx = ctx
        self.temp_dir: tempfile.TemporaryDirectory[str] | None = None
        if path is None:
            self.temp_dir = tempfile.TemporaryDirectory(prefix="vexfs-eval-")
            path = Path(self.temp_dir.name) / "vexfs.sqlite3"
        self.path = Path(path)
        self.connection = sqlite3.connect(
            str(self.path), isolation_level=None, timeout=timeout, check_same_thread=False)
        self.connection.enable_load_extension(True)
        self.connection.load_extension(str(ctx.extension))
        self.connection.execute(f"PRAGMA busy_timeout={int(timeout * 1000)}")
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.execute("PRAGMA synchronous=FULL")
        self.connection.execute("PRAGMA foreign_keys=ON")
        if initialize:
            self.scalar("SELECT vexfs_init()")
            self.scalar("SELECT vexfs_workspace_create('default')")

    def scalar(self, sql: str, parameters: tuple[Any, ...] = ()) -> Any:
        row = self.connection.execute(sql, parameters).fetchone()
        if row is None:
            raise EvalFailure(f"SQL 没有返回行: {sql}")
        return row[0]

    def json(self, sql: str, parameters: tuple[Any, ...] = ()) -> Any:
        return json.loads(self.scalar(sql, parameters))

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None  # type: ignore[assignment]
        if self.temp_dir is not None:
            self.temp_dir.cleanup()
            self.temp_dir = None

    def __enter__(self) -> "Database":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()


def run_process(arguments: list[str], *, input_data: bytes | None = None,
                timeout: float = 120.0, check: bool = True,
                cwd: Path | None = None) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(arguments, input=input_data, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout, cwd=cwd)
    if check and result.returncode != 0:
        raise EvalFailure(
            f"命令失败 ({result.returncode}): {' '.join(arguments)}\n"
            f"stdout:\n{result.stdout.decode(errors='replace')}\n"
            f"stderr:\n{result.stderr.decode(errors='replace')}")
    return result


def extension_path(build_dir: Path) -> Path:
    for name in ("vexdb_lite.dylib", "vexdb_lite.so"):
        candidate = build_dir / name
        if candidate.exists():
            return candidate.resolve()
    raise SystemExit(f"找不到 SQLite 扩展，请先构建: {build_dir}")


def db_storage(path: Path) -> dict[str, int]:
    sizes: dict[str, int] = {}
    for label, candidate in (
        ("main", path),
        ("wal", Path(str(path) + "-wal")),
        ("shm", Path(str(path) + "-shm")),
    ):
        sizes[f"{label}_bytes"] = candidate.stat().st_size if candidate.exists() else 0
    sizes["total_bytes"] = sum(sizes.values())
    return sizes


def sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def child_max_rss_bytes() -> int:
    value = int(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    return value if platform.system() == "Darwin" else value * 1024


@case("existing.sqlite-smoke", "existing-gates", "静态注册 VexFS SQL 合同冒烟")
def existing_sqlite_smoke(ctx: Context) -> dict[str, Any]:
    binary = ctx.build_dir / "vexfs_static_smoke"
    ctx.check(binary.exists(), f"缺少 {binary}")
    result = run_process([str(binary)])
    ctx.check(b"PASS" in result.stdout, "静态 smoke 没有 PASS")
    return {"stdout": result.stdout.decode().strip()}


@case("existing.cabi-smoke", "existing-gates", "真实磁盘 C ABI、同步和崩溃 staging 冒烟")
def existing_cabi_smoke(ctx: Context) -> dict[str, Any]:
    binary = ctx.build_dir / "vexfs_mount_contract_smoke"
    ctx.check(binary.exists(), f"缺少 {binary}")
    result = run_process([str(binary)])
    ctx.check(b"PASS" in result.stdout, "C ABI smoke 没有 PASS")
    return {"stdout": result.stdout.decode().strip()}


@case("existing.cli-smoke", "existing-gates", "CLI 现有端到端冒烟脚本")
def existing_cli_smoke(ctx: Context) -> dict[str, Any]:
    script = ctx.root / "agent_files/cli/test/vexfs_cli_smoke.sh"
    ctx.check(script.exists(), f"缺少 {script}")
    result = run_process(["/bin/bash", str(script), str(ctx.cli)])
    ctx.check(b"PASS" in result.stdout, "CLI smoke 没有 PASS")
    return {"stdout": result.stdout.decode().strip()}


@case("existing.sqlite-spec", "existing-gates", "渲染并执行全部 SQLite YAML spec")
def existing_sqlite_spec(ctx: Context) -> dict[str, Any]:
    script = ctx.root / "tests/spec/_lib/docker/run_sqlite.sh"
    ctx.check(script.exists(), f"缺少 {script}")
    result = run_process(["/bin/bash", str(script)], timeout=180, cwd=ctx.root)
    output = (result.stdout + result.stderr).decode(errors="replace")
    ctx.check("0 failed" in output, "SQLite spec 存在失败")
    return {"summary": output.strip().splitlines()[-1] if output.strip() else ""}


@case("contract.schema-workspaces", "functional", "合同版本、幂等初始化和 workspace 隔离")
def schema_workspaces(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        ctx.equal(db.scalar("SELECT vexfs_contract_version()"), CONTRACT_VERSION, "合同版本")
        ctx.equal(db.scalar("SELECT vexfs_init()"), 1, "重复初始化")
        default_id = db.scalar("SELECT vexfs_workspace_create('default')")
        ctx.equal(default_id, 1, "默认 workspace id")
        other_id = db.scalar("SELECT vexfs_workspace_create('other')")
        ctx.check(other_id != default_id, "workspace id 必须隔离")
        db.scalar("SELECT vexfs_mkdir('default','/only-default')")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_stat('other','/only-default')"),
                         "path not found")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_workspace_create('')"), "1..128")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_workspace_create(?)", ("x" * 129,)),
                         "1..128")
        root = db.json("SELECT vexfs_stat('default','/')")
        ctx.equal(root["kind"], "directory", "根节点类型")
        ctx.check(root["version"] >= 1, "根目录 verifier 必须非零")
        return {"default_workspace": default_id, "other_workspace": other_id,
                "root_inode": root["inode"]}


@case("contract.binary-unicode-metadata", "functional", "二进制、Unicode 名称和元数据")
def binary_unicode_metadata(ctx: Context) -> dict[str, Any]:
    payload = bytes(range(256)) + b"\x00\xff\x00agent\n"
    path = "/资料/空 格/\"quote\"-🚀.bin"
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/资料/空 格')")
        version = db.scalar("SELECT vexfs_write('default',?,?)", (path, payload))
        ctx.equal(db.scalar("SELECT vexfs_read('default',?)", (path,)), payload, "二进制读取")
        stat = db.json("SELECT vexfs_stat('default',?)", (path,))
        ctx.equal(stat["size"], len(payload), "文件大小")
        ctx.equal(stat["version"], version, "版本号")
        ctx.equal(stat["mode"], 0o644, "文件 mode")
        ctx.check(stat["created_at"] > 0 and stat["updated_at"] > 0, "时间戳")
        listing = db.json("SELECT vexfs_list('default','/资料/空 格')")
        ctx.equal([entry["name"] for entry in listing], ['"quote"-🚀.bin'], "Unicode list")
        ctx.equal(db.scalar("SELECT vexfs_path('default',?)", (stat["inode"],)), path,
                  "inode 反查路径")
        return {"bytes": len(payload), "sha256": sha256(payload), "inode": stat["inode"]}


@case("contract.transactions", "transaction", "提交、回滚、savepoint 和读者快照")
def transactions(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/tx')")
        db.scalar("SELECT vexfs_write('default','/tx/value','old')")
        db.connection.execute("BEGIN")
        db.scalar("SELECT vexfs_write('default','/tx/value','rollback')")
        db.connection.execute("ROLLBACK")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/tx/value') AS TEXT)"),
                  "old", "事务回滚")
        db.connection.execute("BEGIN")
        db.connection.execute("SAVEPOINT nested")
        db.scalar("SELECT vexfs_write('default','/tx/value','nested')")
        db.connection.execute("ROLLBACK TO nested")
        db.scalar("SELECT vexfs_write('default','/tx/value','committed')")
        db.connection.execute("COMMIT")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/tx/value') AS TEXT)"),
                  "committed", "savepoint 后提交")

        reader = sqlite3.connect(str(db.path), isolation_level=None)
        reader.enable_load_extension(True)
        reader.load_extension(str(ctx.extension))
        reader.execute("BEGIN")
        before = reader.execute(
            "SELECT CAST(vexfs_read('default','/tx/value') AS TEXT)").fetchone()[0]
        db.scalar("SELECT vexfs_write('default','/tx/value','newer')")
        during = reader.execute(
            "SELECT CAST(vexfs_read('default','/tx/value') AS TEXT)").fetchone()[0]
        reader.execute("COMMIT")
        after = reader.execute(
            "SELECT CAST(vexfs_read('default','/tx/value') AS TEXT)").fetchone()[0]
        reader.close()
        ctx.equal((before, during, after), ("committed", "committed", "newer"),
                  "读者快照隔离")
        return {"snapshot_values": [before, during, after]}


@case("contract.path-validation", "functional", "路径边界、根目录保护和错误信息")
def path_validation(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        invalid = [
            ("relative", "path must be absolute"),
            ("/a/./b", "must not contain"),
            ("/a/../b", "must not contain"),
            ("/" + "x" * 256, "longer than 255"),
        ]
        for path, message in invalid:
            ctx.expect_error(lambda p=path: db.scalar(
                "SELECT vexfs_mkdir('default',?)", (p,)), message)
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_remove('default','/',1)"),
                         "root cannot be removed")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_move('default','/','/moved')"),
                         "root cannot be moved")
        db.scalar("SELECT vexfs_mkdir('default','/a/b')")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_move('default','/a','/a/b/inside')"), "moved into itself")
        db.scalar("SELECT vexfs_write('default','/file','x')")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_mkdir('default','/file/child')"),
                         "not a directory")
        return {"invalid_paths": len(invalid), "root_guards": 2}


@case("contract.rename-remove", "functional", "移动、替换、目录循环和递归删除")
def rename_remove(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/src/child')")
        db.scalar("SELECT vexfs_mkdir('default','/dst/nonempty')")
        db.scalar("SELECT vexfs_write('default','/dst/nonempty/child','occupied')")
        db.scalar("SELECT vexfs_write('default','/src/child/a','A')")
        db.scalar("SELECT vexfs_write('default','/src/child/live','L')")
        db.scalar("SELECT vexfs_write('default','/dst/file','D')")
        inode = db.json("SELECT vexfs_stat('default','/src/child/live')")["inode"]
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_rename('default','/src/child/a','/dst/file',0)"), "already exists")
        db.scalar("SELECT vexfs_rename('default','/src/child/a','/dst/file',1)")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/dst/file') AS TEXT)"),
                  "A", "replace 内容")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_rename('default','/src/child','/dst/file',1)"), "types differ")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_rename('default','/src/child','/dst/nonempty',1)"),
            "destination directory is not empty")
        db.scalar("SELECT vexfs_move('default','/src','/moved')")
        ctx.equal(db.scalar("SELECT vexfs_path('default',?)", (inode,)),
                  "/moved/child/live", "祖先移动后的 inode 路径")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_remove('default','/moved',0)"), "directory is not empty")
        db.scalar("SELECT vexfs_remove('default','/moved',1)")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_stat('default','/moved/child/live')"), "path not found")
        return {"stable_inode": inode, "replace": True, "recursive_remove": True}


@case("contract.version-history", "transaction", "历史查询、指定版本读取和恢复为新版本")
def version_history(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        versions = []
        for value in (b"one", b"two", b"three"):
            versions.append(db.scalar("SELECT vexfs_write('default','/history',?)", (value,)))
        ctx.equal(versions, [1, 2, 3], "文件版本单调增长")
        inode = db.json("SELECT vexfs_stat('default','/history')")["inode"]
        history = db.connection.execute(
            "SELECT version_no,content,size FROM _vexfs_file_versions "
            "WHERE inode_id=? ORDER BY version_no", (inode,)).fetchall()
        ctx.equal(history, [(1, b"one", 3), (2, b"two", 3), (3, b"three", 5)],
                  "历史版本内容")
        public_history = db.json("SELECT vexfs_history('default','/history')")
        ctx.equal([row["version"] for row in public_history], [3, 2, 1],
                  "公开历史版本倒序")
        ctx.equal([row["current"] for row in public_history], [True, False, False],
                  "公开历史当前标记")
        ctx.equal(db.scalar("SELECT vexfs_read_version('default','/history',1)"),
                  b"one", "读取指定版本")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_read_version('default','/history',99)"), "version not found")
        commits = db.connection.execute(
            "SELECT id,parent_commit,message FROM _vexfs_commits ORDER BY id").fetchall()
        ctx.equal([row[1] for row in commits], [None, 1, 2], "commit 父链")
        ctx.equal(db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'"), 3,
            "workspace head")
        restored = db.scalar(
            "SELECT vexfs_restore_version('default','/history',1,3)")
        ctx.equal(restored, 4, "恢复生成新版本")
        ctx.equal(db.scalar("SELECT vexfs_read('default','/history')"), b"one",
                  "恢复后的当前内容")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_restore_version('default','/history',2,3)"), "version conflict")
        restored_history = db.json("SELECT vexfs_history('default','/history')")
        ctx.equal([row["version"] for row in restored_history], [4, 3, 2, 1],
                  "恢复保留旧历史")
        ctx.equal(restored_history[0]["message"], "restore version 1", "恢复 commit 消息")

        db.connection.execute("BEGIN")
        ctx.equal(db.scalar(
            "SELECT vexfs_restore_version('default','/history',2,4)"), 5,
            "事务内恢复版本")
        db.connection.execute("ROLLBACK")
        ctx.equal(db.json("SELECT vexfs_stat('default','/history')")["version"], 4,
                  "回滚恢复不改变当前版本")
        ctx.equal(len(db.json("SELECT vexfs_history('default','/history')")), 4,
                  "回滚恢复不留下历史")
        return {"inode": inode, "versions": 4, "commits": 4, "restored": restored}


@case("contract.version-input-validation", "transaction",
      "版本参数必须是精确整数，非法 restore 不得留下状态")
def version_input_validation(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/strict-version','one')")
        db.scalar("SELECT vexfs_write('default','/strict-version','two')")
        before = db.connection.execute(
            "SELECT (SELECT count(*) FROM _vexfs_file_versions),"
            "(SELECT count(*) FROM _vexfs_commits),"
            "(SELECT head_commit FROM _vexfs_workspaces WHERE name='default')"
        ).fetchone()
        for invalid in (None, 0, -1, 1.9, "1garbage"):
            ctx.expect_error(lambda invalid=invalid: db.scalar(
                "SELECT vexfs_read_version('default','/strict-version',?)", (invalid,)),
                "positive integer")
            ctx.expect_error(lambda invalid=invalid: db.scalar(
                "SELECT vexfs_restore_version('default','/strict-version',?,2)", (invalid,)),
                "positive integer")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_restore_version('default','/strict-version',1,2.9)"),
            "positive integer")
        after = db.connection.execute(
            "SELECT (SELECT count(*) FROM _vexfs_file_versions),"
            "(SELECT count(*) FROM _vexfs_commits),"
            "(SELECT head_commit FROM _vexfs_workspaces WHERE name='default')"
        ).fetchone()
        ctx.equal(after, before, "非法版本参数不改变历史和 head")
        ctx.equal(db.scalar("SELECT vexfs_read('default','/strict-version')"), b"two",
                  "非法 restore 不改变内容")
        return {"rejected": 11, "state": after}


@case("contract.open-unlink", "transaction", "打开文件删除后仍可发布并关闭 handle")
def open_unlink(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/open-unlink','old')")
        inode = db.json("SELECT vexfs_stat('default','/open-unlink')")["inode"]
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/open-unlink','rw','unlink-open')")
        generation = db.scalar(
            "SELECT vexfs_handle_stage_write(?,0,'NEW','unlink-write')", (handle,))
        db.scalar("SELECT vexfs_remove('default','/open-unlink',0)")
        ctx.equal(db.scalar(
            "SELECT vexfs_handle_publish(?,?,'full','unlink-publish')", (handle, generation)),
            2, "删除后的打开 handle 仍可发布")
        ctx.equal(db.scalar("SELECT vexfs_handle_close(?,0,'unlink-close')", (handle,)),
                  "closed", "删除后的 handle 可关闭")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_stat('default','/open-unlink')"),
                         "path not found")
        ctx.equal(db.scalar(
            "SELECT content FROM _vexfs_file_versions WHERE inode_id=? AND version_no=2", (inode,)),
            b"NEW", "删除 inode 的已发布历史完整")
        return {"inode": inode, "published_version": 2}


@case("contract.directory-verifier", "functional", "目录变更 verifier 单调增长")
def directory_verifier(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/left')")
        db.scalar("SELECT vexfs_mkdir('default','/right')")
        left_0 = db.json("SELECT vexfs_stat('default','/left')")["version"]
        right_0 = db.json("SELECT vexfs_stat('default','/right')")["version"]
        db.scalar("SELECT vexfs_write('default','/left/file','x')")
        left_1 = db.json("SELECT vexfs_stat('default','/left')")["version"]
        db.scalar("SELECT vexfs_move('default','/left/file','/right/file')")
        left_2 = db.json("SELECT vexfs_stat('default','/left')")["version"]
        right_1 = db.json("SELECT vexfs_stat('default','/right')")["version"]
        db.scalar("SELECT vexfs_remove('default','/right/file',0)")
        right_2 = db.json("SELECT vexfs_stat('default','/right')")["version"]
        ctx.check(left_0 < left_1 < left_2, "源目录 verifier 必须增长")
        ctx.check(right_0 < right_1 < right_2, "目标目录 verifier 必须增长")
        return {"left": [left_0, left_1, left_2], "right": [right_0, right_1, right_2]}


@case("contract.handles", "handles", "句柄 flags、稀疏写、truncate、发布和关闭")
def handles(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/handles')")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_open('default','/handles/missing','r','open-missing')"),
            "path not found")
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/handles/value','rwc','open-create')")
        ctx.equal(len(handle), 32, "handle id 长度")
        generation = db.scalar(
            "SELECT vexfs_handle_stage_write(?,5,?,'write-sparse')", (handle, b"Z"))
        ctx.equal(generation, 1, "首次 generation")
        ctx.equal(db.scalar("SELECT vexfs_handle_read(?,0,7)", (handle,)),
                  b"\x00\x00\x00\x00\x00Z", "稀疏区域补零")
        generation = db.scalar(
            "SELECT vexfs_handle_truncate(?,3,'truncate-short')", (handle,))
        ctx.equal(generation, 2, "truncate generation")
        generation = db.scalar(
            "SELECT vexfs_handle_truncate(?,8,'truncate-grow')", (handle,))
        ctx.equal(generation, 3, "regrow generation")
        ctx.equal(db.scalar("SELECT vexfs_handle_read(?,0,16)", (handle,)), b"\x00" * 8,
                  "truncate 后旧数据不能复活")
        version = db.scalar(
            "SELECT vexfs_handle_publish(?,3,'full','publish-handle')", (handle,))
        ctx.equal(version, 2, "发布版本")
        state = db.scalar("SELECT vexfs_handle_close(?,1,'close-handle')", (handle,))
        ctx.equal(state, "closed", "已发布句柄关闭")
        ctx.equal(db.scalar("SELECT vexfs_read('default','/handles/value')"), b"\x00" * 8,
                  "发布内容")
        readonly = db.scalar(
            "SELECT vexfs_handle_open('default','/handles/value','r','open-readonly')")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_stage_write(?,0,'X','write-readonly')", (readonly,)),
            "read-only")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_open('default','/handles/value','ct','bad-flags')"),
            "flags must contain")
        return {"generation": generation, "version": version, "sparse_bytes": 8}


@case("contract.idempotency", "handles", "所有写请求的幂等重试和参数指纹")
def idempotency(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/file','abc')")
        handle = db.scalar("SELECT vexfs_handle_open('default','/file','rw','open-idem')")
        ctx.equal(db.scalar("SELECT vexfs_handle_open('default','/file','rw','open-idem')"),
                  handle, "open 重试")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_open('default','/file','r','open-idem')"),
            "different arguments")
        write_sql = "SELECT vexfs_handle_stage_write(?,3,'d','write-idem')"
        first = db.scalar(write_sql, (handle,))
        ctx.equal(db.scalar(write_sql, (handle,)), first, "write 重试")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_stage_write(?,2,'d','write-idem')", (handle,)),
            "different arguments")
        publish_sql = "SELECT vexfs_handle_publish(?,1,'data','publish-idem')"
        version = db.scalar(publish_sql, (handle,))
        ctx.equal(db.scalar(publish_sql, (handle,)), version, "publish 重试")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_truncate(?,1,'publish-idem')", (handle,)),
            "another operation")
        close_sql = "SELECT vexfs_handle_close(?,0,'close-idem')"
        ctx.equal(db.scalar(close_sql, (handle,)), "closed", "close")
        ctx.equal(db.scalar(close_sql, (handle,)), "closed", "close 重试")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/file') AS TEXT)"),
                  "abcd", "幂等后内容")
        return {"request_rows": db.scalar("SELECT count(*) FROM _vexfs_requests")}


@case("contract.write-conflict", "concurrency", "两个写句柄的乐观并发冲突")
def write_conflict(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/conflict','base')")
        first = db.scalar("SELECT vexfs_handle_open('default','/conflict','rw','open-a-eval')")
        second = db.scalar("SELECT vexfs_handle_open('default','/conflict','rw','open-b-eval')")
        db.scalar("SELECT vexfs_handle_stage_write(?,0,'A','write-a-eval')", (first,))
        db.scalar("SELECT vexfs_handle_stage_write(?,0,'B','write-b-eval')", (second,))
        db.scalar("SELECT vexfs_handle_publish(?,1,'data','publish-a-eval')", (first,))
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_publish(?,1,'data','publish-b-eval')", (second,)),
            "write conflict")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/conflict') AS TEXT)"),
                  "Aase", "冲突后已提交内容")
        return {"conflicts": 1}


@case("contract.session-reclaim", "recovery", "session 隔离、同步、retained 和过期回收")
def session_reclaim(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/session','old')")
        db.scalar("SELECT vexfs_mount_session_start('default','session-a')")
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/session','rw','open-session','session-a')")
        db.scalar("SELECT vexfs_handle_stage_write(?,0,'NEW','write-session')", (handle,))
        ctx.equal(db.scalar(
            "SELECT vexfs_mount_synchronize('default','sync-other','session-b')"), 0,
            "其他 session 不能发布")
        ctx.equal(db.scalar(
            "SELECT vexfs_mount_synchronize('default','sync-owner','session-a')"), 1,
            "owner session 发布")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/session') AS TEXT)"),
                  "NEW", "同步后的内容")
        db.scalar("SELECT vexfs_handle_stage_write(?,3,'!','write-retain')", (handle,))
        db.scalar("SELECT vexfs_mount_session_end('default','session-a')")
        ctx.equal(db.scalar("SELECT state FROM _vexfs_handles WHERE id=?", (handle,)),
                  "retained", "未发布 staging 保留")
        ctx.equal(db.scalar(
            "SELECT vexfs_mount_synchronize('default','sync-no-owner')"), 0,
            "owner staging 不能被普通同步接管")
        db.scalar("SELECT vexfs_mount_session_start('default','session-b')")
        ctx.equal(db.scalar(
            "SELECT vexfs_mount_synchronize('default','sync-recovered','session-b')"), 1,
            "新 session 接管并发布 retained staging")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/session') AS TEXT)"),
                  "NEW!", "接管后的暂存内容")
        db.scalar("SELECT vexfs_mount_session_end('default','session-b')")
        ctx.equal(db.scalar("SELECT vexfs_item_reclaim('default','reclaim-closed')"), 1,
                  "只回收已关闭句柄")
        ctx.equal(db.scalar("SELECT count(*) FROM _vexfs_staging WHERE handle_id=?", (handle,)),
                  0, "staging 回收")
        return {"published": 2, "reclaimed": 1}


@case("migration.contract-010-to-030", "migration", "0.1.0 数据库原地升级并保留 dirty staging")
def migrate_010_to_030(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-migration-") as directory:
        path = Path(directory) / "legacy.sqlite3"
        db = Database(ctx, path, initialize=False)
        db.connection.executescript("""
CREATE TABLE _vexfs_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
INSERT INTO _vexfs_meta VALUES('contract_version','0.1.0');
CREATE TABLE _vexfs_inodes(kind TEXT,deleted_at INTEGER,current_version INTEGER);
INSERT INTO _vexfs_inodes VALUES('directory',NULL,0);
CREATE TABLE _vexfs_handles(id TEXT PRIMARY KEY,dirty_generation INTEGER NOT NULL);
INSERT INTO _vexfs_handles VALUES('dirty-handle',2);
CREATE TABLE _vexfs_staging(handle_id TEXT,generation INTEGER,content BLOB,
 created_at INTEGER,PRIMARY KEY(handle_id,generation));
INSERT INTO _vexfs_staging VALUES('dirty-handle',1,X'6F6C64',1);
INSERT INTO _vexfs_staging VALUES('dirty-handle',2,X'6E6577',2);
CREATE TABLE _vexfs_requests(request_id TEXT PRIMARY KEY,operation TEXT NOT NULL,
 result_integer INTEGER,result_text TEXT,created_at INTEGER);
INSERT INTO _vexfs_requests VALUES('old-request','write',1,NULL,1);
""")
        ctx.equal(db.scalar("SELECT vexfs_init()"), 1, "迁移执行")
        ctx.equal(db.scalar("SELECT value FROM _vexfs_meta WHERE key='contract_version'"),
                  CONTRACT_VERSION, "迁移版本")
        row = db.connection.execute(
            "SELECT s.generation,d.content,s.logical_size,s.capacity "
            "FROM _vexfs_staging s JOIN _vexfs_staging_data d USING(handle_id)").fetchone()
        ctx.equal(row, (2, b"new", 3, 3), "只保留当前 dirty generation")
        ctx.equal(db.scalar("SELECT count(*) FROM _vexfs_requests"), 0, "旧请求缓存清空")
        ctx.equal(db.scalar("SELECT current_version FROM _vexfs_inodes"), 1,
                  "目录 verifier 修复")
        columns = [row[1] for row in db.connection.execute(
            "PRAGMA table_info(_vexfs_handles)").fetchall()]
        ctx.check("owner_session" in columns, "迁移 owner_session 列")
        db.close()
        return {"from": "0.1.0", "to": CONTRACT_VERSION, "staging_generation": 2}


@case("migration.staging-inline-to-split", "migration",
      "旧 0.2 inline staging 原地拆表并保留未发布内容")
def migrate_inline_staging_to_split(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-staging-migration-") as directory:
        path = Path(directory) / "legacy-020.sqlite3"
        db = Database(ctx, path)
        db.scalar("SELECT vexfs_write('default','/legacy','old')")
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/legacy','rw','legacy-split-open')")
        generation = db.scalar(
            "SELECT vexfs_handle_stage_write(?,0,'new','legacy-split-write')", (handle,))
        db.connection.executescript("""
UPDATE _vexfs_staging
SET content=(SELECT content FROM _vexfs_staging_data
             WHERE _vexfs_staging_data.handle_id=_vexfs_staging.handle_id);
DROP TABLE _vexfs_staging_data;
DELETE FROM _vexfs_meta WHERE key='staging_layout';
UPDATE _vexfs_meta SET value='0.2.0' WHERE key='contract_version';
""")
        db.close()

        migrated = Database(ctx, path, initialize=False)
        ctx.equal(migrated.scalar("SELECT vexfs_init()"), 1, "0.2 staging 拆表迁移")
        ctx.equal(migrated.scalar(
            "SELECT value FROM _vexfs_meta WHERE key='staging_layout'"),
            "split-v1", "staging 布局标记")
        ctx.equal(migrated.scalar(
            "SELECT value FROM _vexfs_meta WHERE key='contract_version'"),
            CONTRACT_VERSION, "0.2 合同升级")
        row = migrated.connection.execute(
            "SELECT length(s.content),d.content,s.logical_size,s.capacity "
            "FROM _vexfs_staging s JOIN _vexfs_staging_data d USING(handle_id) "
            "WHERE s.handle_id=?", (handle,)).fetchone()
        ctx.equal(row, (0, b"new" + b"\x00" * (64 * 1024 - 3), 3, 64 * 1024),
                  "迁移后 metadata 与 BLOB 分离")
        migrated.scalar(
            "SELECT vexfs_handle_publish(?,?,'full','legacy-split-publish')",
            (handle, generation))
        ctx.equal(migrated.scalar("SELECT vexfs_read('default','/legacy')"), b"new",
                  "迁移后未发布内容可发布")
        ctx.equal(migrated.scalar("PRAGMA integrity_check"), "ok", "迁移后完整性")
        migrated.close()
        return {"layout": "split-v1", "generation": generation, "capacity": 64 * 1024}


@case("migration.failure-rollback", "migration", "0.2 迁移失败必须完整回滚并可重试")
def migration_failure_rollback(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-migration-failure-") as directory:
        path = Path(directory) / "broken-020.sqlite3"
        db = Database(ctx, path)
        db.scalar("SELECT vexfs_write('default','/migration','value')")
        db.connection.executescript("""
DROP TABLE _vexfs_staging_data;
CREATE TABLE _vexfs_staging_data(handle_id TEXT PRIMARY KEY,broken BLOB NOT NULL);
DELETE FROM _vexfs_meta WHERE key='staging_layout';
UPDATE _vexfs_meta SET value='0.2.0' WHERE key='contract_version';
""")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_init()"), "no column named content")
        ctx.equal(db.scalar("SELECT value FROM _vexfs_meta WHERE key='contract_version'"),
                  "0.2.0", "失败后合同版本回滚")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_meta WHERE key='staging_layout'"), 0,
            "失败后布局标记不残留")
        db.connection.execute("DROP TABLE _vexfs_staging_data")
        ctx.equal(db.scalar("SELECT vexfs_init()"), 1, "修复条件后可重试")
        ctx.equal(db.scalar("SELECT value FROM _vexfs_meta WHERE key='contract_version'"),
                  CONTRACT_VERSION, "重试后完成迁移")
        db.close()
        return {"rollback_version": "0.2.0", "retry_version": CONTRACT_VERSION}


@case("migration.initialization-failure-rollback", "migration",
      "新库初始化空间不足时不得留下半张 schema")
def initialization_failure_rollback(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-init-failure-") as directory:
        path = Path(directory) / "full.sqlite3"
        db = Database(ctx, path, initialize=False)
        db.connection.execute("PRAGMA page_size=512")
        db.connection.execute("PRAGMA max_page_count=8")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_init()"), "full")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM sqlite_master WHERE name LIKE '_vexfs_%'"), 0,
            "失败后不残留 VexFS 表")
        db.connection.execute("PRAGMA max_page_count=4096")
        ctx.equal(db.scalar("SELECT vexfs_init()"), 1, "释放空间后初始化可重试")
        ctx.equal(db.scalar("SELECT value FROM _vexfs_meta WHERE key='contract_version'"),
                  CONTRACT_VERSION, "重试后合同完整")
        db.close()
        return {"page_size": 512, "failed_pages": 8}


@case("limits.invalid-handle-ranges", "limits", "负偏移、负长度、错误 generation 和 durability")
def invalid_handle_ranges(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/ranges','abc')")
        handle = db.scalar("SELECT vexfs_handle_open('default','/ranges','rw','range-open')")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_stage_write(?,-1,'x','range-negative')", (handle,)),
            "out of range")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_read(?,-1,1)", (handle,)), "non-negative")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_read(?,0,-1)", (handle,)), "non-negative")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_truncate(?,-1,'truncate-negative')", (handle,)),
            "out of range")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_publish(?,0,'invalid','publish-invalid')", (handle,)),
            "durability must be")
        db.scalar("SELECT vexfs_handle_stage_write(?,0,'x','range-write')", (handle,))
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_publish(?,2,'data','publish-future-generation')", (handle,)),
            "generation is stale")
        return {"rejected_ranges": 4, "rejected_publish": 2}


@case("recovery.reopen-integrity", "recovery", "关闭重开、WAL checkpoint 和 integrity_check")
def reopen_integrity(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-reopen-") as directory:
        path = Path(directory) / "db.sqlite3"
        db = Database(ctx, path)
        payload = os.urandom(2 * MIB)
        db.scalar("SELECT vexfs_mkdir('default','/persist')")
        db.scalar("SELECT vexfs_write('default','/persist/blob',?)", (payload,))
        db.connection.execute("PRAGMA wal_checkpoint(FULL)").fetchone()
        db.close()
        reopened = Database(ctx, path)
        ctx.equal(reopened.scalar("SELECT vexfs_read('default','/persist/blob')"), payload,
                  "重开内容")
        ctx.equal(reopened.scalar("PRAGMA integrity_check"), "ok", "integrity_check")
        fk_rows = reopened.connection.execute("PRAGMA foreign_key_check").fetchall()
        ctx.equal(fk_rows, [], "foreign key check")
        metrics = {"bytes": len(payload), "sha256": sha256(payload),
                   "storage": db_storage(path)}
        reopened.close()
        return metrics


@case("recovery.process-crash", "recovery", "进程在事务中退出和提交后立即退出")
def process_crash(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-crash-") as directory:
        path = Path(directory) / "db.sqlite3"
        with Database(ctx, path) as db:
            db.scalar("SELECT vexfs_write('default','/crash','old')")
        script = Path(__file__).resolve()
        rolled_back = run_process(
            [sys.executable, str(script), "--worker", "crash-write", str(path),
             str(ctx.extension), "/crash", "uncommitted", "transaction"],
            check=False)
        ctx.equal(rolled_back.returncode, 17, "事务中崩溃退出码")
        with Database(ctx, path) as db:
            ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/crash') AS TEXT)"),
                      "old", "崩溃事务必须回滚")
            ctx.equal(db.scalar("PRAGMA integrity_check"), "ok", "崩溃后完整性")
        committed = run_process(
            [sys.executable, str(script), "--worker", "crash-write", str(path),
             str(ctx.extension), "/crash", "durable", "autocommit"], check=False)
        ctx.equal(committed.returncode, 0, "提交后退出码")
        with Database(ctx, path) as db:
            ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/crash') AS TEXT)"),
                      "durable", "提交后立即退出仍持久")
        return {"transaction_exit": 17, "autocommit_exit": 0}


@case("backup.online-restore", "backup", "在线 SQLite backup、恢复和 staging 可见性")
def online_backup_restore(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-backup-") as directory:
        source_path = Path(directory) / "source.sqlite3"
        backup_path = Path(directory) / "backup.sqlite3"
        source = Database(ctx, source_path)
        payload = os.urandom(min(ctx.mode.sequential_mib, 64) * MIB)
        source.scalar("SELECT vexfs_write('default','/backup.bin',?)", (payload,))
        handle = source.scalar(
            "SELECT vexfs_handle_open('default','/backup.bin','rw','backup-open')")
        source.scalar("SELECT vexfs_handle_stage_write(?,0,'DIRTY','backup-write')", (handle,))
        destination = sqlite3.connect(str(backup_path), isolation_level=None)
        started = time.perf_counter()
        source.connection.backup(destination, pages=256)
        elapsed = time.perf_counter() - started
        destination.close()
        restored = Database(ctx, backup_path)
        restored_payload = restored.scalar("SELECT vexfs_read('default','/backup.bin')")
        ctx.equal(restored_payload, payload, "备份只能看到已发布内容")
        ctx.equal(restored.scalar("SELECT count(*) FROM _vexfs_staging"), 1,
                  "备份保留完整恢复状态")
        ctx.equal(restored.scalar("PRAGMA integrity_check"), "ok", "备份完整性")
        storage = db_storage(backup_path)
        bytes_copied = storage["total_bytes"]
        restored.close()
        source.close()
        ctx.budget("backup_seconds", elapsed, 30.0)
        return {"logical_bytes": len(payload), "backup_bytes": bytes_copied,
                "storage": storage,
                "seconds": round(elapsed, 6),
                "mib_per_second": round(bytes_copied / MIB / max(elapsed, 1e-9), 3)}


@case("concurrency.database-lock", "concurrency", "busy_timeout 和数据库锁错误")
def database_lock(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-lock-") as directory:
        path = Path(directory) / "db.sqlite3"
        owner = Database(ctx, path)
        contender = Database(ctx, path, timeout=0.2)
        owner.connection.execute("BEGIN IMMEDIATE")
        owner.scalar("SELECT vexfs_write('default','/locked','owner')")
        started = time.perf_counter()
        message = ctx.expect_error(lambda: contender.scalar(
            "SELECT vexfs_write('default','/other','contender')"), "locked")
        elapsed = time.perf_counter() - started
        owner.connection.execute("ROLLBACK")
        contender.close()
        owner.close()
        ctx.check(elapsed >= 0.15, f"busy_timeout 未生效: {elapsed:.3f}s")
        return {"wait_seconds": round(elapsed, 6), "error": message}


@case("concurrency.multi-process", "concurrency", "多个真实进程同时写不同文件")
def multi_process(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-parallel-") as directory:
        path = Path(directory) / "db.sqlite3"
        with Database(ctx, path) as db:
            db.scalar("SELECT vexfs_mkdir('default','/parallel')")
        script = Path(__file__).resolve()
        processes: list[subprocess.Popen[bytes]] = []
        started = time.perf_counter()
        for worker in range(ctx.mode.parallel_workers):
            processes.append(subprocess.Popen(
                [sys.executable, str(script), "--worker", "parallel-write", str(path),
                 str(ctx.extension), str(worker), str(ctx.mode.files_per_worker)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        failures: list[str] = []
        for process in processes:
            stdout, stderr = process.communicate(timeout=120)
            if process.returncode != 0:
                failures.append(
                    f"rc={process.returncode} stdout={stdout.decode(errors='replace')} "
                    f"stderr={stderr.decode(errors='replace')}")
        ctx.equal(failures, [], "并发 worker")
        elapsed = time.perf_counter() - started
        expected = ctx.mode.parallel_workers * ctx.mode.files_per_worker
        with Database(ctx, path) as db:
            listing = db.json("SELECT vexfs_list('default','/parallel')")
            ctx.equal(len(listing), expected, "并发文件数")
            for worker in range(ctx.mode.parallel_workers):
                path_name = f"/parallel/w{worker}-{ctx.mode.files_per_worker - 1}.txt"
                expected_value = f"worker={worker};item={ctx.mode.files_per_worker - 1}".encode()
                ctx.equal(db.scalar("SELECT vexfs_read('default',?)", (path_name,)),
                          expected_value, "并发文件内容")
            ctx.equal(db.scalar("PRAGMA integrity_check"), "ok", "并发后完整性")
        ctx.budget("parallel_seconds", elapsed, 60.0)
        return {"workers": ctx.mode.parallel_workers, "files": expected,
                "seconds": round(elapsed, 6),
                "files_per_second": round(expected / max(elapsed, 1e-9), 3)}


@case("concurrency.restore-race", "concurrency",
      "两个真实进程同时 restore 只能生成一个版本和一个 commit")
def restore_race(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-restore-race-") as directory:
        base = Path(directory)
        path = base / "db.sqlite3"
        script = Path(__file__).resolve()
        with Database(ctx, path) as db:
            db.scalar("SELECT vexfs_write('default','/race','one')")
            db.scalar("SELECT vexfs_write('default','/race','two')")
            inode = db.json("SELECT vexfs_stat('default','/race')")["inode"]
            commits_before = db.scalar("SELECT count(*) FROM _vexfs_commits")
        barrier = base / "go"
        ready_paths = [base / "ready-0", base / "ready-1"]
        processes = [subprocess.Popen(
            [sys.executable, str(script), "--worker", "restore-race", str(path),
             str(ctx.extension), "/race", str(ready_paths[index]), str(barrier)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE) for index in range(2)]
        deadline = time.time() + 10
        while not all(candidate.exists() for candidate in ready_paths):
            if time.time() >= deadline:
                raise EvalFailure("restore worker 未就绪")
            time.sleep(0.01)
        barrier.touch()
        results: list[tuple[int, bytes, bytes]] = []
        for process in processes:
            stdout, stderr = process.communicate(timeout=30)
            results.append((process.returncode, stdout, stderr))
        ctx.equal(sorted(result[0] for result in results), [0, 5], "restore 并发结果")
        with Database(ctx, path) as db:
            ctx.equal(db.scalar(
                "SELECT count(*) FROM _vexfs_file_versions WHERE inode_id=?", (inode,)),
                3, "并发 restore 只新增一个版本")
            ctx.equal(db.scalar("SELECT count(*) FROM _vexfs_commits"), commits_before + 1,
                      "并发 restore 只新增一个 commit")
            head = db.scalar("SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
            version_commit = db.scalar(
                "SELECT commit_id FROM _vexfs_file_versions WHERE inode_id=? AND version_no=3",
                (inode,))
            ctx.equal(head, version_commit, "workspace head 指向成功版本")
            ctx.equal(db.scalar("SELECT vexfs_read('default','/race')"), b"one",
                      "成功 restore 的内容")
        return {"exit_codes": [result[0] for result in results], "versions": 3}


@case("model.randomized-state-machine", "model", "确定性随机文件操作与参考模型逐步比对")
def randomized_state_machine(ctx: Context) -> dict[str, Any]:
    rng = random.Random(ctx.seed ^ 0x5EED_F5)
    model: dict[str, bytes] = {}
    next_id = 0
    directories = ["/model/a", "/model/b", "/model/c"]
    with Database(ctx) as db:
        for directory in directories:
            db.scalar("SELECT vexfs_mkdir('default',?)", (directory,))

        def new_path() -> str:
            nonlocal next_id
            path = f"{rng.choice(directories)}/f{next_id:06d}.bin"
            next_id += 1
            return path

        def verify(sample_all: bool = False) -> None:
            for directory in directories:
                actual = [entry["name"] for entry in db.json(
                    "SELECT vexfs_list('default',?)", (directory,))]
                expected = sorted(Path(path).name for path in model if
                                  str(Path(path).parent) == directory)
                ctx.equal(actual, expected, f"模型目录 {directory}")
            paths = list(model) if sample_all else rng.sample(
                list(model), min(len(model), 8))
            for path in paths:
                ctx.equal(db.scalar("SELECT vexfs_read('default',?)", (path,)), model[path],
                          f"模型内容 {path}")

        for operation in range(ctx.mode.random_ops):
            choice = rng.random()
            if not model or choice < 0.45:
                path = new_path()
                content = rng.randbytes(rng.randint(0, 512))
                db.scalar("SELECT vexfs_write('default',?,?)", (path, content))
                model[path] = content
            elif choice < 0.65:
                path = rng.choice(list(model))
                content = rng.randbytes(rng.randint(0, 1024))
                db.scalar("SELECT vexfs_write('default',?,?)", (path, content))
                model[path] = content
            elif choice < 0.82:
                source = rng.choice(list(model))
                destination = new_path()
                db.scalar("SELECT vexfs_move('default',?,?)", (source, destination))
                model[destination] = model.pop(source)
            elif choice < 0.94:
                path = rng.choice(list(model))
                db.scalar("SELECT vexfs_remove('default',?,0)", (path,))
                del model[path]
            else:
                path = rng.choice(list(model))
                ctx.equal(db.scalar("SELECT vexfs_read('default',?)", (path,)), model[path],
                          "随机 read")
            if operation % 50 == 0:
                verify()
        verify(sample_all=True)
        ctx.equal(db.scalar("PRAGMA integrity_check"), "ok", "随机模型后完整性")
        return {"seed": ctx.seed, "operations": ctx.mode.random_ops,
                "final_files": len(model)}


@case("cli.command-surface", "cli", "常用文件命令、版本命令、descriptor 和 doctor")
def cli_command_surface(ctx: Context) -> dict[str, Any]:
    ctx.check(ctx.cli.exists(), f"缺少 CLI: {ctx.cli}")
    with tempfile.TemporaryDirectory(prefix="vexfs-cli-eval-") as directory:
        base = Path(directory)
        database = base / "cli.sqlite3"
        descriptor = base / "volume.vexfs"
        local_file = base / "payload.bin"
        payload = bytes(range(256)) * 8 + b"\x00CLI"
        local_file.write_bytes(payload)
        prefix = [str(ctx.cli), "--db", str(database), "--workspace", "eval"]
        run_process(prefix + ["setup"])
        run_process(prefix + ["mkdir", "/cli/a", "/cli/b"])
        write = run_process(prefix + ["write", "/cli/a/payload.bin", str(local_file)])
        ctx.check(int(write.stdout.strip()) >= 1, "CLI write version")
        ctx.equal(run_process(prefix + ["cat", "/cli/a/payload.bin"]).stdout,
                  payload, "CLI binary cat")
        listing = run_process(prefix + ["--json", "ls", "/cli/a"])
        ctx.equal(json.loads(listing.stdout)[0]["name"], "payload.bin", "CLI JSON ls")
        stat = json.loads(run_process(prefix + ["stat", "/cli/a/payload.bin"]).stdout)
        ctx.equal(stat["size"], len(payload), "CLI stat")

        version_one = base / "version-one.txt"
        version_two = base / "version-two.txt"
        version_one.write_text("one\n")
        version_two.write_text("two\n")
        ctx.equal(run_process(prefix + ["write", "/cli/a/version.txt", str(version_one)]).stdout.strip(),
                  b"1", "CLI 历史版本 1")
        ctx.equal(run_process(prefix + ["write", "/cli/a/version.txt", str(version_two)]).stdout.strip(),
                  b"2", "CLI 历史版本 2")
        cli_history = json.loads(run_process(
            prefix + ["--json", "history", "/cli/a/version.txt"]).stdout)
        ctx.equal([row["version"] for row in cli_history["entries"]], [2, 1], "CLI history")
        first_page = json.loads(run_process(
            prefix + ["--json", "history", "/cli/a/version.txt", "--limit", "1"]).stdout)
        ctx.equal([row["version"] for row in first_page["entries"]], [2], "CLI history 首屏")
        ctx.equal(first_page["next_before"], 2, "CLI history cursor")
        ctx.equal(run_process(prefix + ["show", "/cli/a/version.txt", "--version", "1"]).stdout,
                  b"one\n", "CLI show")
        diff = run_process(prefix + ["diff", "/cli/a/version.txt", "--from", "1", "--to", "2"],
                           check=False)
        ctx.equal(diff.returncode, 1, "CLI diff 不同退出码")
        ctx.check(b"-one" in diff.stdout and b"+two" in diff.stdout, "CLI diff 内容")
        missing_diff = run_process(
            prefix + ["--json", "diff", "/cli/a/version.txt", "--from", "99", "--to", "2"],
            check=False)
        ctx.equal(missing_diff.returncode, 3, "CLI 缺失版本退出码")
        ctx.equal(json.loads(missing_diff.stderr)["error"]["code"], "VEXFS_NOT_FOUND",
                  "CLI JSON 错误分类")
        dry_run = run_process(prefix + ["restore", "/cli/a/version.txt", "--version", "1",
                                        "--dry-run"])
        ctx.check(b"-two" in dry_run.stdout and b"+one" in dry_run.stdout,
                  "CLI restore dry-run 预览")
        ctx.equal(json.loads(run_process(prefix + ["stat", "/cli/a/version.txt"]).stdout)["version"],
                  2, "dry-run 不改变版本")
        ctx.equal(run_process(prefix + ["restore", "/cli/a/version.txt", "--version", "1"]).stdout.strip(),
                  b"3", "CLI restore 新版本")
        ctx.equal(run_process(prefix + ["cat", "/cli/a/version.txt"]).stdout,
                  b"one\n", "CLI restore 内容")

        newline_one = base / "newline-one.txt"
        newline_two = base / "newline-two.txt"
        newline_one.write_bytes(b"same\n")
        newline_two.write_bytes(b"same")
        run_process(prefix + ["write", "/cli/a/newline.txt", str(newline_one)])
        run_process(prefix + ["write", "/cli/a/newline.txt", str(newline_two)])
        newline_diff = run_process(
            prefix + ["diff", "/cli/a/newline.txt", "--from", "1", "--to", "2"],
            check=False)
        ctx.equal(newline_diff.returncode, 1, "CLI diff 识别末尾换行差异")
        ctx.check(b"No newline at end of file" in newline_diff.stdout,
                  "CLI diff 末尾换行提示")
        run_process(prefix + ["rm", "/cli/a/newline.txt"])
        run_process(prefix + ["rm", "/cli/a/version.txt"])
        run_process(prefix + ["mv", "/cli/a/payload.bin", "/cli/b/moved.bin"])
        run_process(prefix + ["rm", "/cli/a"])
        run_process(prefix + ["rm", "-r", "/cli/b"])
        run_process(prefix + ["descriptor", str(descriptor)])
        descriptor_json = json.loads(descriptor.read_text())
        ctx.equal(descriptor_json["workspace"], "eval", "descriptor workspace")
        ctx.equal(Path(descriptor_json["database_path"]).resolve(), database.resolve(),
                  "descriptor database")
        doctor = run_process(prefix + ["--json", "doctor"], check=False)
        doctor_json = json.loads(doctor.stdout)
        ctx.equal(doctor_json["database"]["contract_version"], CONTRACT_VERSION,
                  "doctor contract")
        ctx.check(doctor.returncode in (0, 1), "doctor 退出码")
        invalid = run_process(prefix + ["unknown"], check=False)
        ctx.equal(invalid.returncode, 1, "未知命令退出码")
        ctx.check(b"unknown command" in invalid.stderr, "未知命令错误信息")
        return {"payload_bytes": len(payload), "doctor": doctor_json,
                "descriptor": descriptor_json}


@case("cli.doctor-readonly-and-permissions", "cli",
      "doctor 不迁移数据库，CLI 创建的 SQLite 文件保持私有权限")
def cli_doctor_readonly_and_permissions(ctx: Context) -> dict[str, Any]:
    ctx.check(ctx.cli.exists(), f"缺少 CLI: {ctx.cli}")
    with tempfile.TemporaryDirectory(prefix="vexfs-cli-security-") as directory:
        database = Path(directory) / "private" / "vexfs.sqlite3"
        prefix = [str(ctx.cli), "--db", str(database), "--workspace", "secure"]
        run_process(prefix + ["setup"])
        run_process(prefix + ["write", "/secret"], input_data=b"secret")
        for candidate in (database, Path(str(database) + "-wal"), Path(str(database) + "-shm")):
            if candidate.exists():
                ctx.equal(candidate.stat().st_mode & 0o777, 0o600,
                          f"{candidate.name} 私有权限")
        connection = sqlite3.connect(str(database), isolation_level=None)
        connection.execute(
            "UPDATE _vexfs_meta SET value='0.2.0' WHERE key='contract_version'")
        connection.close()
        doctor = run_process(prefix + ["--json", "doctor"], check=False)
        details = json.loads(doctor.stdout)["database"]
        ctx.equal(details["contract_version"], "0.2.0", "doctor 报告真实旧版本")
        ctx.equal(details["compatible"], False, "doctor 拒绝把旧版本报为正常")
        ctx.equal(details["upgrade_required"], True, "doctor 给出升级状态")
        connection = sqlite3.connect(str(database), isolation_level=None)
        ctx.equal(connection.execute(
            "SELECT value FROM _vexfs_meta WHERE key='contract_version'").fetchone()[0],
            "0.2.0", "doctor 结束后没有静默迁移")
        connection.close()
        readonly = sqlite3.connect(f"file:{database}?mode=ro", uri=True, isolation_level=None)
        readonly.enable_load_extension(True)
        readonly.load_extension(str(ctx.extension))
        ctx.equal(readonly.execute(
            "SELECT vexfs_read('secure','/secret')").fetchone()[0], b"secret",
            "0.2 数据库仍可只读访问")
        readonly.close()
        run_process(prefix + ["setup"])
        connection = sqlite3.connect(str(database), isolation_level=None)
        ctx.equal(connection.execute(
            "SELECT value FROM _vexfs_meta WHERE key='contract_version'").fetchone()[0],
            CONTRACT_VERSION, "显式 setup 才执行升级")
        connection.close()
        return {"mode": "0600", "doctor_exit": doctor.returncode}


@case("performance.small-files", "performance", "大量小文件创建、列表、stat 和读取")
def performance_small_files(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/small')")
        payload = b"small-file-payload"
        started = time.perf_counter()
        for index in range(ctx.mode.small_files):
            db.scalar("SELECT vexfs_write('default',?,?)",
                      (f"/small/f{index:06d}.txt", payload))
        create_seconds = time.perf_counter() - started
        started = time.perf_counter()
        listing = db.json("SELECT vexfs_list('default','/small')")
        list_seconds = time.perf_counter() - started
        ctx.equal(len(listing), ctx.mode.small_files, "小文件列表数量")
        ctx.equal([entry["name"] for entry in listing[:3]],
                  [f"f{i:06d}.txt" for i in range(3)], "列表二进制排序")
        started = time.perf_counter()
        for index in range(0, ctx.mode.small_files,
                           max(1, ctx.mode.small_files // 100)):
            stat = db.json("SELECT vexfs_stat('default',?)",
                           (f"/small/f{index:06d}.txt",))
            ctx.equal(stat["size"], len(payload), "小文件 stat")
            ctx.equal(db.scalar("SELECT vexfs_read('default',?)",
                                (f"/small/f{index:06d}.txt",)), payload, "小文件 read")
        sample_seconds = time.perf_counter() - started
        ctx.budget("small_create_seconds", create_seconds, 60.0)
        ctx.budget("small_list_seconds", list_seconds, 5.0)
        return {"files": ctx.mode.small_files,
                "create_seconds": round(create_seconds, 6),
                "create_files_per_second": round(
                    ctx.mode.small_files / max(create_seconds, 1e-9), 3),
                "list_seconds": round(list_seconds, 6),
                "sample_stat_read_seconds": round(sample_seconds, 6),
                "storage": db_storage(db.path)}


@case("performance.sequential-file", "performance", "大文件写入、读取、重开和哈希")
def performance_sequential_file(ctx: Context) -> dict[str, Any]:
    payload = (bytes(range(256)) * (ctx.mode.sequential_mib * MIB // 256))
    with tempfile.TemporaryDirectory(prefix="vexfs-sequential-") as directory:
        path = Path(directory) / "db.sqlite3"
        db = Database(ctx, path)
        started = time.perf_counter()
        version = db.scalar("SELECT vexfs_write('default','/large.bin',?)", (payload,))
        write_seconds = time.perf_counter() - started
        db.close()
        reopened = Database(ctx, path)
        started = time.perf_counter()
        actual = reopened.scalar("SELECT vexfs_read('default','/large.bin')")
        read_seconds = time.perf_counter() - started
        ctx.equal(sha256(actual), sha256(payload), "大文件哈希")
        ctx.equal(len(actual), len(payload), "大文件长度")
        ctx.equal(reopened.scalar("PRAGMA integrity_check"), "ok", "大文件 DB 完整性")
        storage_before_checkpoint = db_storage(path)
        reopened.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        storage_after_checkpoint = db_storage(path)
        reopened.close()
        ctx.budget("sequential_write_seconds", write_seconds, 60.0)
        ctx.budget("sequential_read_seconds", read_seconds, 30.0)
        return {"logical_bytes": len(payload), "version": version,
                "sha256": sha256(payload),
                "storage_before_checkpoint": storage_before_checkpoint,
                "storage_after_checkpoint": storage_after_checkpoint,
                "write_seconds": round(write_seconds, 6),
                "write_mib_per_second": round(len(payload) / MIB / max(write_seconds, 1e-9), 3),
                "read_seconds": round(read_seconds, 6),
                "read_mib_per_second": round(len(payload) / MIB / max(read_seconds, 1e-9), 3)}


@case("performance.staged-overwrite", "performance", "分块 staging 写入的空间上界和吞吐")
def performance_staged_overwrite(ctx: Context) -> dict[str, Any]:
    total = ctx.mode.staged_mib * MIB
    chunk = bytes((index * 17) % 251 for index in range(256 * 1024))
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/staged.bin',X'')")
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/staged.bin','rw','staged-open')")
        started = time.perf_counter()
        generation = 0
        for offset in range(0, total, len(chunk)):
            data = chunk[:min(len(chunk), total - offset)]
            generation = db.scalar(
                "SELECT vexfs_handle_stage_write(?,?,?,?)",
                (handle, offset, data, f"staged-{offset}"))
        stage_seconds = time.perf_counter() - started
        row = db.connection.execute(
            "SELECT count(*),s.logical_size,s.capacity,length(d.content) "
            "FROM _vexfs_staging s JOIN _vexfs_staging_data d USING(handle_id) "
            "WHERE s.handle_id=?", (handle,)).fetchone()
        ctx.equal(row[0], 1, "每个 handle 只能有一行 staging")
        ctx.equal(row[1], total, "staging logical_size")
        ctx.equal(row[2], row[3], "capacity 与 BLOB 长度")
        ctx.check(total <= row[2] <= MAX_FILE_BYTES, "staging capacity 边界")
        ctx.check(row[2] < max(total * 2, 64 * 1024 + 1), "几何扩容不能超过 2x")
        started = time.perf_counter()
        version = db.scalar(
            "SELECT vexfs_handle_publish(?,?,'full','staged-publish')",
            (handle, generation))
        publish_seconds = time.perf_counter() - started
        db.scalar("SELECT vexfs_handle_close(?,0,'staged-close')", (handle,))
        ctx.equal(db.scalar("SELECT length(vexfs_read('default','/staged.bin'))"), total,
                  "staged 发布长度")
        sample = db.scalar(
            "SELECT substr(vexfs_read('default','/staged.bin'),?,?)",
            (total - min(total, len(chunk)) + 1, min(total, len(chunk))))
        ctx.equal(sample, chunk[:len(sample)], "staged 尾部内容")
        ctx.budget("staged_seconds", stage_seconds, 120.0)
        storage_before_checkpoint = db_storage(db.path)
        db.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        storage_after_checkpoint = db_storage(db.path)
        vacuum_started = time.perf_counter()
        db.connection.execute("VACUUM")
        vacuum_seconds = time.perf_counter() - vacuum_started
        db.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        storage_after_vacuum = db_storage(db.path)
        return {"logical_bytes": total, "capacity_bytes": row[2],
                "staging_rows": row[0], "generation": generation, "version": version,
                "stage_seconds": round(stage_seconds, 6),
                "stage_mib_per_second": round(total / MIB / max(stage_seconds, 1e-9), 3),
                "publish_seconds": round(publish_seconds, 6),
                "storage_before_checkpoint": storage_before_checkpoint,
                "storage_after_checkpoint": storage_after_checkpoint,
                "vacuum_seconds": round(vacuum_seconds, 6),
                "storage_after_vacuum": storage_after_vacuum}


@case("performance.random-patches", "performance", "随机 4 KiB 覆盖与最终抽样校验")
def performance_random_patches(ctx: Context) -> dict[str, Any]:
    logical_size = min(ctx.mode.staged_mib, 32) * MIB
    patch_size = 4096
    rng = random.Random(ctx.seed ^ 0xA11CE)
    expected: dict[int, bytes] = {}
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/random.bin',zeroblob(?))", (logical_size,))
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/random.bin','rw','random-open')")
        started = time.perf_counter()
        generation = 0
        for index in range(ctx.mode.random_writes):
            slot = rng.randrange(logical_size // patch_size)
            offset = slot * patch_size
            data = hashlib.sha256(f"{ctx.seed}:{index}".encode()).digest() * (patch_size // 32)
            generation = db.scalar(
                "SELECT vexfs_handle_stage_write(?,?,?,?)",
                (handle, offset, data, f"random-write-{index}"))
            expected[offset] = data
        seconds = time.perf_counter() - started
        for offset, data in list(expected.items())[-min(len(expected), 50):]:
            ctx.equal(db.scalar("SELECT vexfs_handle_read(?,?,?)",
                                (handle, offset, patch_size)), data, "随机 patch 内容")
        row = db.connection.execute(
            "SELECT count(*),logical_size,capacity FROM _vexfs_staging WHERE handle_id=?",
            (handle,)).fetchone()
        ctx.equal(row[0], 1, "随机写 staging 行数")
        ctx.equal(row[1], logical_size, "随机写 logical_size")
        ctx.check(row[2] <= MAX_FILE_BYTES, "随机写 capacity")
        ctx.budget("random_patch_seconds", seconds, 120.0)
        storage_before_checkpoint = db_storage(db.path)
        db.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        storage_after_checkpoint = db_storage(db.path)
        return {"writes": ctx.mode.random_writes, "unique_slots": len(expected),
                "logical_bytes": logical_size, "capacity_bytes": row[2],
                "generation": generation, "seconds": round(seconds, 6),
                "operations_per_second": round(ctx.mode.random_writes / max(seconds, 1e-9), 3),
                "request_rows": db.scalar("SELECT count(*) FROM _vexfs_requests"),
                "storage_before_checkpoint": storage_before_checkpoint,
                "storage_after_checkpoint": storage_after_checkpoint}


@case("performance.version-history", "performance", "重复整文件覆盖的历史版本吞吐和空间")
def performance_version_history(ctx: Context) -> dict[str, Any]:
    version_count = {"quick": 5, "full": 25, "stress": 50}[ctx.mode.name]
    payload_mib = 1 if ctx.mode.name == "quick" else 4
    payload_size = payload_mib * MIB
    with Database(ctx) as db:
        started = time.perf_counter()
        last_payload = b""
        for index in range(version_count):
            marker = index.to_bytes(4, "big")
            last_payload = marker + bytes([index % 251]) * (payload_size - len(marker))
            version = db.scalar("SELECT vexfs_write('default','/versions.bin',?)",
                                (last_payload,))
            ctx.equal(version, index + 1, "历史版本号")
        seconds = time.perf_counter() - started
        ctx.equal(db.scalar("SELECT vexfs_read('default','/versions.bin')"), last_payload,
                  "最新历史版本")
        inode = db.json("SELECT vexfs_stat('default','/versions.bin')")["inode"]
        rows = db.scalar(
            "SELECT count(*) FROM _vexfs_file_versions WHERE inode_id=?", (inode,))
        ctx.equal(rows, version_count, "历史版本行数")
        history_started = time.perf_counter()
        history_page = db.json(
            "SELECT vexfs_history('default','/versions.bin',?,0)", (version_count,))
        history_seconds = time.perf_counter() - history_started
        ctx.equal(len(history_page["entries"]), version_count, "历史分页返回量")
        old_started = time.perf_counter()
        oldest = db.scalar("SELECT vexfs_read_version('default','/versions.bin',1)")
        old_read_seconds = time.perf_counter() - old_started
        ctx.equal(len(oldest), payload_size, "读取最旧版本")
        restore_started = time.perf_counter()
        restored = db.scalar(
            "SELECT vexfs_restore_version('default','/versions.bin',1,?)", (version_count,))
        restore_seconds = time.perf_counter() - restore_started
        ctx.equal(restored, version_count + 1, "恢复历史版本")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_file_versions WHERE inode_id=?", (inode,)),
            version_count + 1, "恢复后历史行数")
        storage_before_checkpoint = db_storage(db.path)
        db.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        storage_after_checkpoint = db_storage(db.path)
        return {"versions": version_count, "bytes_per_version": payload_size,
                "logical_written_bytes": version_count * payload_size,
                "seconds": round(seconds, 6),
                "write_mib_per_second": round(
                    version_count * payload_mib / max(seconds, 1e-9), 3),
                "history_seconds": round(history_seconds, 6),
                "old_read_seconds": round(old_read_seconds, 6),
                "restore_seconds": round(restore_seconds, 6),
                "storage_before_checkpoint": storage_before_checkpoint,
                "storage_after_checkpoint": storage_after_checkpoint}


@case("limits.maximum-file", "limits", "128 MiB 文件上限和越界拒绝", modes=("full", "stress"))
def maximum_file(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        started = time.perf_counter()
        version = db.scalar(
            "SELECT vexfs_write('default','/max.bin',zeroblob(?))", (MAX_FILE_BYTES,))
        seconds = time.perf_counter() - started
        ctx.equal(db.scalar("SELECT length(vexfs_read('default','/max.bin'))"),
                  MAX_FILE_BYTES, "最大文件长度")
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/max.bin','rw','max-open')")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_stage_write(?,?,X'00','max-overflow')",
            (handle, MAX_FILE_BYTES)), "larger than 128 MiB")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_handle_truncate(?,?,'max-truncate')",
            (handle, MAX_FILE_BYTES + 1)), "out of range")
        db.scalar("SELECT vexfs_handle_close(?,0,'max-close')", (handle,))
        ctx.equal(db.scalar(
            "SELECT vexfs_write('default','/max.bin',zeroblob(?))", (MAX_FILE_BYTES,)),
            2, "最大文件第二版本")
        diff_rss = None
        restore_rss = None
        if platform.system() == "Darwin" and ctx.cli.exists():
            prefix = [str(ctx.cli), "--db", str(db.path), "--workspace", "default"]
            run_process(prefix + ["diff", "/max.bin", "--from", "1", "--to", "2"],
                        timeout=60)
            diff_rss = child_max_rss_bytes()
            ctx.check(diff_rss < 256 * MIB,
                      f"128 MiB diff 内存过高: {diff_rss / MIB:.1f} MiB")
            run_process(prefix + ["restore", "/max.bin", "--version", "1"], timeout=60)
            restore_rss = child_max_rss_bytes()
            ctx.check(restore_rss < 256 * MIB,
                      f"128 MiB restore 内存过高: {restore_rss / MIB:.1f} MiB")
            ctx.equal(db.scalar(
                "SELECT source_version_no FROM _vexfs_file_versions "
                "WHERE version_no=3 AND inode_id=(SELECT inode_id FROM _vexfs_dentries "
                "WHERE name='max.bin')"), 1, "restore 复用不可变版本内容")
            ctx.equal(db.scalar("SELECT length(vexfs_read('default','/max.bin'))"),
                      MAX_FILE_BYTES, "复用内容的恢复版本可读")
            ctx.equal(db.scalar(
                "SELECT sum(length(content)) FROM _vexfs_file_versions WHERE inode_id="
                "(SELECT inode_id FROM _vexfs_dentries WHERE name='max.bin')"),
                2 * MAX_FILE_BYTES, "restore 不重复保存 128 MiB BLOB")
        storage_before_checkpoint = db_storage(db.path)
        db.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        storage_after_checkpoint = db_storage(db.path)
        vacuum_started = time.perf_counter()
        db.connection.execute("VACUUM")
        vacuum_seconds = time.perf_counter() - vacuum_started
        db.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        storage_after_vacuum = db_storage(db.path)
        return {"logical_bytes": MAX_FILE_BYTES, "version": version,
                "write_seconds": round(seconds, 6),
                "storage_before_checkpoint": storage_before_checkpoint,
                "storage_after_checkpoint": storage_after_checkpoint,
                "vacuum_seconds": round(vacuum_seconds, 6),
                "storage_after_vacuum": storage_after_vacuum,
                "diff_max_rss": diff_rss, "restore_max_rss": restore_rss}


@case("build.fskit-unsigned", "build", "macOS FSKit App 与扩展无签名编译",
      modes=("full", "stress"))
def fskit_unsigned_build(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Darwin" or shutil.which("xcodebuild") is None:
        raise EvalSkip("只在装有 Xcode 的 macOS 执行")
    project = ctx.root / "agent_files/macos/VexFS.xcodeproj"
    if not project.exists():
        raise EvalSkip("没有 FSKit Xcode 工程")
    mount_output = run_process(["/sbin/mount"], check=False).stdout.decode(
        errors="replace").lower()
    fskit_mount_markers = (
        "(vexfs,", "(exfat,", "(msdos,",
        " type vexfs", " type exfat", " type msdos",
    )
    if any(marker in mount_output for marker in fskit_mount_markers):
        raise EvalSkip("已有 FSKit 文件系统挂载，跳过会刷新扩展缓存的无签名构建检查")
    derived = ctx.output_dir / "xcode-derived-data"
    started = time.perf_counter()
    app: Path | None = None
    try:
        result = run_process(
            ["xcodebuild", "-project", str(project), "-scheme", "VexFSApp",
             "-configuration", "Debug", "-derivedDataPath", str(derived),
             "CODE_SIGNING_ALLOWED=NO", "build"], timeout=300, cwd=ctx.root)
        seconds = time.perf_counter() - started
        output = result.stdout + result.stderr
        ctx.check(b"BUILD SUCCEEDED" in output, "Xcode 没有 BUILD SUCCEEDED")
        app = next(derived.glob("Build/Products/Debug/VexFS.app"), None)
        ctx.check(app is not None, "缺少 VexFS.app")
        return {"seconds": round(seconds, 6), "app": str(app)}
    finally:
        # xcodebuild 会把测试产物登记到 LaunchServices。若不撤销登记，未签名的
        # Debug extension 会和 /Applications 中的正式开发签名版本争用同一标识。
        if app is None:
            app = next(derived.glob("Build/Products/Debug/VexFS.app"), None)
        lsregister = Path(
            "/System/Library/Frameworks/CoreServices.framework/Frameworks/"
            "LaunchServices.framework/Support/lsregister")
        if app is not None and lsregister.exists():
            # Xcode 返回后，LaunchServices 的构建后登记仍可能延迟一两秒落盘。
            # 等它完成再撤销，否则测试进程结束后会重新出现一条 Debug App 记录。
            time.sleep(2.0)
            for attempt in range(3):
                run_process([str(lsregister), "-u", str(app)], check=False, timeout=30)
                if attempt != 2:
                    time.sleep(0.5)
        # 撤销同标识的测试 App 后，LaunchServices 也可能把 /Applications 中的
        # 正式扩展从 FSKit 的可用列表移除，并让 fskit_agent 保留失效进程缓存。
        # 立即恢复正式版本并刷新空闲代理，保证后续真实 mount 和用户环境不受影响。
        installed_app = Path("/Applications/VexFS.app")
        installed_extension = (
            installed_app / "Contents/Extensions/VexFSAppEx.appex")
        if (lsregister.exists() and installed_app.exists() and
                installed_extension.exists()):
            run_process([str(lsregister), "-f", str(installed_app)],
                        check=False, timeout=30)
            if shutil.which("pluginkit") is not None:
                run_process(["pluginkit", "-a", str(installed_extension)],
                            check=False, timeout=30)
            refreshed_mount_output = run_process(
                ["/sbin/mount"], check=False).stdout.decode(errors="replace").lower()
            if not any(marker in refreshed_mount_output
                       for marker in fskit_mount_markers):
                # fskit_agent 会忽略 TERM；没有 FSKit 挂载时用 KILL 让
                # launchd 拉起一个不含无签名测试进程缓存的新代理。
                run_process(["pkill", "-KILL", "-x", "fskit_agent"],
                            check=False, timeout=30)
                time.sleep(2.0)


@case("mount.real-bash", "mount", "真实 FSKit mount 后用 bash 常用文件命令操作")
def real_mount_bash(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise EvalSkip("真实 FSKit mount 只在 macOS 执行")
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-eval-") as directory:
        base = Path(directory)
        database = base / "mount.sqlite3"
        mount_point = base / "mnt"
        prefix = [str(ctx.cli), "--db", str(database), "--workspace", "eval"]
        run_process(prefix + ["setup"])
        doctor = run_process(prefix + ["--json", "doctor"], check=False)
        details = json.loads(doctor.stdout)
        if details.get("extension") != "enabled":
            raise EvalSkip(
                f"FSKit extension={details.get('extension', 'unknown')}，需安装并在系统设置中启用")
        mount_point.mkdir()
        expected_target = os.path.realpath(mount_point)
        run_process(prefix + ["mount", str(mount_point)], timeout=60)
        descriptor: dict[str, Any] = {}
        mounts: list[dict[str, Any]] = []
        unmount = None
        try:
            descriptor = json.loads((base / ".vexfs-volume.json").read_text())
            ctx.equal(descriptor, {"version": 2, "database_file": "mount.sqlite3",
                                   "workspace": "eval"}, "FSKit 目录资源描述文件")
            status = run_process(prefix + ["--json", "mount", "status", str(mount_point)])
            mounts = json.loads(status.stdout)
            ctx.equal(len(mounts), 1, "挂载状态数量")
            ctx.equal(mounts[0]["target"], expected_target, "挂载状态目标")
            shell = r'''
set -e
mkdir -p "$1/project/sub"
printf 'alpha\nbeta\nagent\n' > "$1/project/input.txt"
grep agent "$1/project/input.txt" > "$1/project/sub/result.txt"
cp "$1/project/input.txt" "$1/project/copy.txt"
mv "$1/project/copy.txt" "$1/project/moved.txt"
cmp "$1/project/input.txt" "$1/project/moved.txt"
find "$1/project" -type f | sort
rm "$1/project/moved.txt"
test "$(cat "$1/project/sub/result.txt")" = agent
'''
            result = run_process(["/bin/bash", "-c", shell, "vexfs-eval", str(mount_point)],
                                 timeout=120)
            ctx.check(b"input.txt" in result.stdout and b"result.txt" in result.stdout,
                      "bash find 输出")
            ctx.equal((mount_point / "project/sub/result.txt").read_text(), "agent\n",
                      "挂载路径结果")
            reverse = run_process(prefix + ["cat", "/project/sub/result.txt"])
            ctx.equal(reverse.stdout, b"agent\n", "数据库 CLI 反向读取挂载写入")
        finally:
            unmount = run_process(prefix + ["unmount", str(mount_point)],
                                  check=False, timeout=60)
        ctx.equal(unmount.returncode, 0, "卸载退出码")
        after = json.loads(run_process(prefix + ["--json", "doctor"]).stdout)
        ctx.equal(after["mount_count"], 0, "卸载后挂载数")
        ctx.equal(after["database"]["pending_handles"], 0, "卸载后未发布句柄")
        ctx.equal(after["database"]["retained_handles"], 0, "卸载后保留句柄")
        ctx.equal(after["database"]["staging_bytes"], 0, "卸载后暂存字节")
        return {"doctor_before": details, "doctor_after": after, "mounts": mounts,
                "descriptor": descriptor,
                "commands": ["mkdir", "printf", "grep", "cp", "mv", "cmp", "find",
                             "rm", "cat", "vexfs cat"]}


@case("mount.performance", "mount", "真实 FSKit 分块顺序写和随机覆盖性能")
def real_mount_performance(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise EvalSkip("真实 FSKit mount 只在 macOS 执行")
    sequential_mib = {"quick": 8, "full": 32, "stress": 64}[ctx.mode.name]
    random_writes = {"quick": 100, "full": 1_000, "stress": 2_000}[ctx.mode.name]
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-perf-") as directory:
        base = Path(directory)
        database = base / "mount.sqlite3"
        mount_point = base / "mnt"
        prefix = [str(ctx.cli), "--db", str(database), "--workspace", "perf"]
        run_process(prefix + ["setup"])
        details = json.loads(run_process(
            prefix + ["--json", "doctor"], check=False).stdout)
        if details.get("extension") != "enabled":
            raise EvalSkip(
                f"FSKit extension={details.get('extension', 'unknown')}，需安装并在系统设置中启用")
        mount_point.mkdir()
        run_process(prefix + ["mount", str(mount_point)], timeout=60)
        fd = -1
        try:
            path = mount_point / "bench.bin"
            fd = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o600)
            chunk = b"x" * (256 * 1024)
            chunk_count = sequential_mib * MIB // len(chunk)
            started = time.perf_counter()
            for _ in range(chunk_count):
                os.write(fd, chunk)
            os.fsync(fd)
            sequential_seconds = time.perf_counter() - started

            rng = random.Random(ctx.seed ^ 0xF5A17)
            patch = b"p" * 4096
            slots = sequential_mib * MIB // len(patch)
            sampled_offsets: list[int] = []
            started = time.perf_counter()
            for index in range(random_writes):
                offset = rng.randrange(slots) * len(patch)
                os.pwrite(fd, patch, offset)
                if index >= random_writes - 10:
                    sampled_offsets.append(offset)
            os.fsync(fd)
            random_seconds = time.perf_counter() - started
            for offset in sampled_offsets:
                ctx.equal(os.pread(fd, len(patch), offset), patch,
                          "FSKit 随机覆盖读回")
            os.close(fd)
            fd = -1
            ctx.equal(path.stat().st_size, sequential_mib * MIB, "FSKit 性能文件长度")
            ctx.budget("mount_sequential_seconds", sequential_seconds, 60.0)
            ctx.budget("mount_random_seconds", random_seconds, 120.0)
        finally:
            if fd >= 0:
                os.close(fd)
            unmount = run_process(prefix + ["unmount", str(mount_point)],
                                  check=False, timeout=60)
        ctx.equal(unmount.returncode, 0, "FSKit 性能测试卸载退出码")
        after = json.loads(run_process(prefix + ["--json", "doctor"]).stdout)
        ctx.equal(after["mount_count"], 0, "FSKit 性能测试卸载后挂载数")
        return {
            "sequential_mib": sequential_mib,
            "sequential_seconds": round(sequential_seconds, 6),
            "sequential_mib_per_second": round(
                sequential_mib / max(sequential_seconds, 1e-9), 3),
            "random_writes": random_writes,
            "random_seconds": round(random_seconds, 6),
            "random_operations_per_second": round(
                random_writes / max(random_seconds, 1e-9), 3),
            "doctor_after": after,
        }


def worker_main(arguments: list[str]) -> int:
    operation = arguments[0]
    if operation == "parallel-write":
        path, extension, worker_text, count_text = arguments[1:]
        worker = int(worker_text)
        count = int(count_text)
        connection = sqlite3.connect(path, isolation_level=None, timeout=30)
        connection.enable_load_extension(True)
        connection.load_extension(extension)
        connection.execute("PRAGMA busy_timeout=30000")
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA synchronous=FULL")
        for index in range(count):
            content = f"worker={worker};item={index}".encode()
            connection.execute("SELECT vexfs_write('default',?,?)",
                               (f"/parallel/w{worker}-{index}.txt", content)).fetchone()
        connection.close()
        return 0
    if operation == "restore-race":
        path, extension, file_path, ready_path, barrier_path = arguments[1:]
        connection = sqlite3.connect(path, isolation_level=None, timeout=30)
        connection.enable_load_extension(True)
        connection.load_extension(extension)
        connection.execute("PRAGMA busy_timeout=30000")
        connection.execute("PRAGMA journal_mode=WAL")
        Path(ready_path).touch()
        deadline = time.time() + 10
        while not Path(barrier_path).exists():
            if time.time() >= deadline:
                connection.close()
                return 9
            time.sleep(0.005)
        try:
            version = connection.execute(
                "SELECT vexfs_restore_version('default',?,1,2)", (file_path,)).fetchone()[0]
            print(version)
            connection.close()
            return 0
        except sqlite3.Error as error:
            connection.close()
            if "conflict" in str(error):
                print(str(error), file=sys.stderr)
                return 5
            raise
    if operation == "crash-write":
        path, extension, file_path, value, mode = arguments[1:]
        connection = sqlite3.connect(path, isolation_level=None, timeout=30)
        connection.enable_load_extension(True)
        connection.load_extension(extension)
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA synchronous=FULL")
        if mode == "transaction":
            connection.execute("BEGIN IMMEDIATE")
            connection.execute("SELECT vexfs_write('default',?,?)",
                               (file_path, value.encode())).fetchone()
            os._exit(17)
        connection.execute("SELECT vexfs_write('default',?,?)",
                           (file_path, value.encode())).fetchone()
        os._exit(0)
    raise SystemExit(f"unknown worker operation: {operation}")


def system_details(root: Path, build_dir: Path, extension: Path) -> dict[str, Any]:
    git = run_process(["git", "rev-parse", "HEAD"], cwd=root, check=False)
    build_type = "unknown"
    cache = build_dir / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:STRING="):
                build_type = line.split("=", 1)[1] or "unset"
                break
    return {
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "sqlite": sqlite3.sqlite_version,
        "cpu_count": os.cpu_count(),
        "git_commit": git.stdout.decode().strip() if git.returncode == 0 else "unknown",
        "build_type": build_type,
        "extension_bytes": extension.stat().st_size,
    }


def write_markdown(report: dict[str, Any], path: Path) -> None:
    summary = report["summary"]
    lines = [
        "# VexFS Eval Report",
        "",
        f"- run: `{report['run_id']}`",
        f"- mode: `{report['mode']}`",
        f"- seed: `{report['seed']}`",
        f"- build: `{report['environment']['build_type']}`",
        f"- result: **{summary['status']}**",
        f"- cases: {summary['passed']} passed / {summary['failed']} failed / "
        f"{summary['skipped']} skipped",
        f"- checks: {summary['checks']}",
        f"- duration: {summary['duration_seconds']:.3f}s",
        "",
        "| Case | Category | Status | Time | Checks |",
        "|---|---|---:|---:|---:|",
    ]
    for result in report["cases"]:
        lines.append(
            f"| `{result['id']}` | {result['category']} | {result['status']} | "
            f"{result['duration_seconds']:.3f}s | {result['checks']} |")
        if result.get("error"):
            lines.extend(["", f"> `{result['id']}`: {result['error']}", ""])
    lines.extend(["", "## Metrics", ""])
    for result in report["cases"]:
        if result.get("metrics"):
            lines.extend([f"### {result['id']}", "", "```json",
                          json.dumps(result["metrics"], ensure_ascii=False, indent=2),
                          "```", ""])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_eval(options: argparse.Namespace) -> int:
    root = Path(options.root).resolve()
    build_dir = Path(options.build_dir).resolve()
    ext = extension_path(build_dir)
    cli = (build_dir / "vexfs").resolve()
    mode = MODES[options.mode]
    seed = options.seed
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + f"-{mode.name}-{seed}"
    output_dir = Path(options.output_dir).resolve() / run_id
    output_dir.mkdir(parents=True, exist_ok=False)
    ctx = Context(root, build_dir, ext, cli, output_dir, mode, seed,
                  options.enforce_performance)
    selected = [item for item in CASES if mode.name in item.modes]
    if options.filter:
        selected = [item for item in selected if options.filter in item.case_id or
                    options.filter in item.category]
    started_all = time.perf_counter()
    results: list[dict[str, Any]] = []
    print(f"VexFS eval: mode={mode.name} seed={seed} cases={len(selected)}")
    print(f"extension: {ext}")
    for index, item in enumerate(selected, 1):
        before_checks = ctx.checks
        ctx.current_case = item.case_id
        ctx.current_artifacts = output_dir / "artifacts" / item.case_id
        ctx.current_artifacts.mkdir(parents=True, exist_ok=True)
        started = time.perf_counter()
        status = "PASS"
        metrics: dict[str, Any] = {}
        error = ""
        trace = ""
        print(f"[{index:02d}/{len(selected):02d}] {item.case_id} ... ", end="", flush=True)
        try:
            metrics = item.function(ctx) or {}
        except EvalSkip as exception:
            status = "SKIP"
            error = str(exception)
        except Exception as exception:  # 每个 case 独立记录，继续跑完全部。
            status = "FAIL"
            error = str(exception)
            trace = traceback.format_exc()
        duration = time.perf_counter() - started
        checks = ctx.checks - before_checks
        results.append({
            "id": item.case_id,
            "category": item.category,
            "description": item.description,
            "status": status,
            "duration_seconds": round(duration, 6),
            "checks": checks,
            "metrics": metrics,
            "error": error,
            "traceback": trace,
        })
        print(f"{status} ({duration:.3f}s, {checks} checks)")
        if error:
            print(f"    {error}")
        if status != "FAIL":
            try:
                ctx.current_artifacts.rmdir()
            except OSError:
                pass
    duration_all = time.perf_counter() - started_all
    passed = sum(item["status"] == "PASS" for item in results)
    failed = sum(item["status"] == "FAIL" for item in results)
    skipped = sum(item["status"] == "SKIP" for item in results)
    report = {
        "schema_version": 1,
        "run_id": run_id,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "mode": mode.name,
        "seed": seed,
        "enforce_performance": options.enforce_performance,
        "environment": system_details(root, build_dir, ext),
        "extension": str(ext),
        "cases": results,
        "summary": {
            "status": "FAIL" if failed else "PASS",
            "passed": passed,
            "failed": failed,
            "skipped": skipped,
            "checks": ctx.checks,
            "duration_seconds": round(duration_all, 6),
        },
    }
    json_path = output_dir / "report.json"
    markdown_path = output_dir / "report.md"
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                         encoding="utf-8")
    write_markdown(report, markdown_path)
    latest = output_dir.parent / "latest.json"
    latest.write_text(json.dumps({"run_id": run_id, "report": str(json_path),
                                  "status": report["summary"]["status"]}, indent=2) + "\n")
    print()
    print(f"result: {report['summary']['status']} - {passed} passed, {failed} failed, "
          f"{skipped} skipped, {ctx.checks} checks, {duration_all:.3f}s")
    print(f"JSON: {json_path}")
    print(f"Markdown: {markdown_path}")
    return 1 if failed else 0


def parse_arguments(arguments: list[str]) -> argparse.Namespace:
    if arguments and arguments[0] == "--worker":
        return argparse.Namespace(worker=arguments[1:])
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(root))
    parser.add_argument("--build-dir", default=str(root / "vexdb_sqlite/build"))
    parser.add_argument("--output-dir", default=str(root / "vexdb_sqlite/build/eval/vexfs"))
    parser.add_argument("--mode", choices=sorted(MODES), default="quick")
    parser.add_argument("--seed", type=int, default=20260718)
    parser.add_argument("--filter", default="", help="按 case id 或 category 过滤")
    parser.add_argument("--enforce-performance", action="store_true")
    options = parser.parse_args(arguments)
    options.worker = None
    return options


def main(arguments: list[str] | None = None) -> int:
    options = parse_arguments(sys.argv[1:] if arguments is None else arguments)
    if options.worker is not None:
        return worker_main(options.worker)
    return run_eval(options)


if __name__ == "__main__":
    raise SystemExit(main())
