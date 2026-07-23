#!/usr/bin/env python3
"""VexFS 真实 eval：功能、恢复、并发、备份、性能、CLI 和 macOS mount。

每个 case 都使用真实的磁盘 SQLite 数据库和构建产物。默认产出 JSON 与 Markdown，
便于本地回归和 CI 保存；性能数字默认记录，--enforce-performance 才作为硬门槛。
"""

from __future__ import annotations

import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import os
import platform
import random
import resource
import signal
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
CONTRACT_VERSION = "0.9.0"


class EvalFailure(AssertionError):
    pass


class EvalFailureWithMetrics(EvalFailure):
    def __init__(self, message: str, metrics: dict[str, Any]):
        super().__init__(message)
        self.metrics = metrics


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
    fail_on_skip: bool = False
    mount_cli_override: Path | None = None
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
                cwd: Path | None = None,
                env: dict[str, str] | None = None) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(arguments, input=input_data, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout, cwd=cwd, env=env)
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


def self_max_rss_bytes() -> int:
    value = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    return value if platform.system() == "Darwin" else value * 1024


@case("existing.sqlite-smoke", "existing-gates", "静态注册 VexFS SQL 合同冒烟")
def existing_sqlite_smoke(ctx: Context) -> dict[str, Any]:
    binary = ctx.build_dir / "vexfs_static_smoke"
    ctx.check(binary.exists(), f"缺少 {binary}")
    result = run_process([str(binary)])
    ctx.check(b"PASS" in result.stdout, "静态 smoke 没有 PASS")
    return {"stdout": result.stdout.decode().strip()}


@case("existing.cabi-smoke", "existing-gates",
      "真实 runtime、跨连接 cache generation、同步和崩溃 staging 冒烟")
def existing_cabi_smoke(ctx: Context) -> dict[str, Any]:
    binary = ctx.build_dir / "vexfs_runtime_smoke"
    ctx.check(binary.exists(), f"缺少 {binary}")
    result = run_process([str(binary)])
    ctx.check(b"PASS" in result.stdout, "C ABI smoke 没有 PASS")
    return {"stdout": result.stdout.decode().strip()}


@case("existing.platform-boundary-smoke", "existing-gates",
      "Linux libfuse3 和 Windows WinFsp 平台边界可独立编译并明确报告状态")
def existing_platform_boundary_smoke(ctx: Context) -> dict[str, Any]:
    outputs = {}
    for platform_name in ("linux", "windows"):
        binary = ctx.build_dir / f"vexfs_platform_{platform_name}_smoke"
        ctx.check(binary.exists(), f"缺少 {binary}")
        result = run_process([str(binary)])
        ctx.check(b"PASS" in result.stdout, f"{platform_name} 平台边界 smoke 没有 PASS")
        outputs[platform_name] = result.stdout.decode().strip()
    return outputs


@case("existing.linux-fuse-helper-smoke", "existing-gates",
      "Linux FUSE3 helper 通过真实 SQLite C ABI 完成写入、读取和清理")
def existing_linux_fuse_helper_smoke(ctx: Context) -> dict[str, Any]:
    binary = ctx.build_dir / "vexfs-fuse"
    if platform.system() != "Linux" or not binary.exists():
        raise EvalSkip("当前构建没有 Linux vexfs-fuse helper")
    with tempfile.TemporaryDirectory(prefix="vexfs-fuse-helper-eval-") as directory:
        database = Path(directory) / "helper.sqlite3"
        result = run_process(
            [str(binary), "--db", str(database), "--workspace", "eval", "--self-test"])
        ctx.check(b"VEXFS FUSE SELF TEST: PASS" in result.stdout,
                  "Linux FUSE helper self-test 没有 PASS")
        ctx.check(database.exists(), "Linux FUSE helper 没有创建 SQLite 数据库")
        return {"stdout": result.stdout.decode().strip(),
                "database_bytes": database.stat().st_size}


@case("existing.cli-smoke", "existing-gates", "CLI 现有端到端冒烟脚本")
def existing_cli_smoke(ctx: Context) -> dict[str, Any]:
    script = ctx.root / "agent_files/cli/test/vexfs_cli_smoke.sh"
    ctx.check(script.exists(), f"缺少 {script}")
    result = run_process(["/bin/bash", str(script), str(ctx.cli)])
    ctx.check(b"PASS" in result.stdout, "CLI smoke 没有 PASS")
    return {"stdout": result.stdout.decode().strip()}


@case("existing.unified-cli-smoke", "existing-gates",
      "vexdb 内置 SQLite、向量、文件和数据库备份统一入口")
def existing_unified_cli_smoke(ctx: Context) -> dict[str, Any]:
    script = ctx.root / "agent_files/cli/test/vexdb_unified_smoke.sh"
    binary = ctx.build_dir / "vexdb"
    ctx.check(script.exists(), f"缺少 {script}")
    ctx.check(binary.exists(), f"缺少 {binary}")
    result = run_process(["/bin/bash", str(script), str(binary)], timeout=120)
    ctx.check(b"PASS" in result.stdout, "统一 CLI smoke 没有 PASS")
    return {"stdout": result.stdout.decode().strip()}


def package_stage(ctx: Context) -> Path:
    configured_stage = os.environ.get("VEXDB_LITE_PACKAGE_STAGE", "")
    if configured_stage:
        stage = Path(configured_stage).resolve()
        ctx.check(stage.is_dir(), f"指定的 VexDB-Lite stage 不存在: {stage}")
        return stage
    if ctx.fail_on_skip:
        raise EvalFailure(
            "发行 package Gate 必须通过 VEXDB_LITE_PACKAGE_STAGE 显式绑定本次构建")
    dist = ctx.root / "dist/vexdb-lite"
    candidates = [path for path in dist.glob("vexdb-lite-*-macos-*")
                  if path.is_dir()]
    if not candidates:
        raise EvalSkip("没有已构建的 VexDB-Lite macOS stage")
    return max(candidates, key=lambda path: path.stat().st_mtime)


@case("package.unified-install", "package",
      "统一包全新安装、重复安装、文件命令和保留数据卸载")
def package_unified_install(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise EvalSkip("统一 macOS 安装包只在 macOS 执行")
    stage = package_stage(ctx)
    packaged_cli = stage / "bin/vexdb"
    ctx.check(packaged_cli.exists(), "统一包缺少 bin/vexdb")
    ctx.check((stage / "bin/vexfs").is_symlink(), "vexfs 必须是兼容链接")
    ctx.check((stage / "VexDB Lite.app").is_dir(), "统一包缺少 VexDB Lite.app")
    ctx.check((stage / "lib/vexdb_lite.dylib").exists(), "统一包缺少 SQLite 扩展")
    manifest = dict(
        line.split("=", 1) for line in (stage / "MANIFEST.txt").read_text().splitlines()
        if "=" in line)
    ctx.equal(manifest.get("contract_version"), CONTRACT_VERSION, "manifest schema 版本")
    ctx.check(manifest.get("source_commit") not in (None, "", "unknown"),
              "manifest 必须记录源码 commit")
    if manifest.get("signature") == "developer-id":
        if os.environ.get("VEXDB_LITE_ALLOW_DIRTY_PACKAGE_TEST") == "1":
            ctx.equal(manifest.get("source_dirty"), "true",
                      "显式脏包测试必须真实标记 source_dirty")
        else:
            ctx.equal(manifest.get("source_dirty"), "false",
                      "Developer ID 包不能来自脏工作树")

    with tempfile.TemporaryDirectory(prefix="vexdb-package-eval-") as directory:
        home = Path(directory)
        current = home / "Library/Application Support/VexDB-Lite/default.sqlite3"
        install_env = dict(os.environ)
        install_env.update({
            "HOME": str(home),
            "VEXDB_LITE_APP_DIR": str(home / "Applications"),
            "VEXDB_LITE_BIN_DIR": str(home / "bin"),
            "VEXDB_LITE_LIB_DIR": str(home / "lib"),
        })
        if manifest.get("signature") == "ad-hoc":
            install_env["VEXDB_LITE_ALLOW_ADHOC_INSTALL"] = "1"
        if manifest.get("source_dirty") == "true" and \
                os.environ.get("VEXDB_LITE_ALLOW_DIRTY_PACKAGE_TEST") == "1":
            install_env["VEXDB_LITE_ALLOW_DIRTY_INSTALL"] = "1"
        run_process([str(stage / "install.sh")], env=install_env)
        installed = home / "bin/vexdb"
        installed_app = home / "Applications/VexDB Lite.app"
        ctx.check(installed.exists(), "安装后缺少 vexdb")
        ctx.check((home / "bin/vexfs").is_symlink(), "安装后 vexfs 不是兼容链接")
        # Developer ID 导出的 extension 合法包含 embedded.provisionprofile，不能把它
        # 当作残留。注入一个发行包绝不会包含的额外资源，模拟旧 bundle 合并污染。
        stale_resource = installed_app / "Contents/vexdb-stale-install-regression.txt"
        stale_resource.write_bytes(b"stale-resource-from-previous-version")
        invalid = run_process(
            ["/usr/bin/codesign", "--verify", "--deep", "--strict", str(installed_app)],
            check=False)
        ctx.check(invalid.returncode != 0, "测试残留必须先破坏旧 App 签名")
        run_process([str(stage / "install.sh")], env=install_env)
        ctx.check(not stale_resource.exists(), "重复安装必须删除旧 bundle 的多余资源")
        verified = run_process(
            ["/usr/bin/codesign", "--verify", "--deep", "--strict", str(installed_app)],
            check=False)
        ctx.equal(verified.returncode, 0, "整体替换后 App 签名有效")
        run_process([str(installed), "fs", "--db", str(current), "setup"])
        doctor = json.loads(run_process(
            [str(installed), "fs", "--db", str(current), "--json", "doctor"],
            check=False).stdout)
        ctx.equal(str(doctor["runtime_abi"]), manifest.get("mount_abi_version"),
                  "manifest 与 CLI runtime ABI")
        run_process([str(installed), "fs", "--db", str(current),
                     "write", "/kept.txt"], input_data=b"kept file")
        ctx.equal(run_process([str(installed), str(current),
                              "SELECT vexfs_contract_version();"]).stdout.strip(),
                  CONTRACT_VERSION.encode(), "全新数据库 schema")
        ctx.equal(run_process([str(installed), "fs", "--db", str(current),
                              "cat", "/kept.txt"]).stdout.strip(),
                  b"kept file", "新建 VexFS 文件")
        run_process([str(stage / "uninstall.sh")], env=install_env)
        ctx.check(current.exists(), "卸载不能删除新数据库")
        return {"stage": str(stage),
                "atomic_reinstall": True,
                "version": run_process([str(packaged_cli), "--version"])
                .stdout.decode().strip()}


@case("package.documentation-smoke", "package",
      "按交付使用说明验证安装、SQLite、向量、文件、版本、备份、动态加载和卸载")
def package_documentation_smoke(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise EvalSkip("统一 macOS 安装包只在 macOS 执行")
    stage = package_stage(ctx)
    script = ctx.root / "tests/eval/vexfs/documentation_smoke.sh"
    ctx.check(script.exists(), f"缺少 {script}")
    result = run_process(["/bin/bash", str(script), str(stage)], timeout=180)
    output = (result.stdout + result.stderr).decode(errors="replace")
    ctx.check("VEXDB DOCUMENTATION SMOKE: PASS" in output,
              "使用说明端到端测试没有 PASS")
    return {"stage": str(stage), "summary": output.strip().splitlines()[-1]}


@case("existing.sqlite-spec", "existing-gates", "渲染并执行全部 SQLite YAML spec")
def existing_sqlite_spec(ctx: Context) -> dict[str, Any]:
    script = ctx.root / "tests/spec/_lib/docker/run_sqlite.sh"
    ctx.check(script.exists(), f"缺少 {script}")
    environment = os.environ.copy()
    environment["VEXDB_SQLITE_EXTENSION"] = str(ctx.extension)
    result = run_process(["/bin/bash", str(script)], timeout=180, cwd=ctx.root,
                         env=environment)
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


@case("contract.posix-mode", "functional", "可执行权限持久化、事务回滚且不干扰打开句柄")
def posix_mode(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        inode = db.scalar("SELECT vexfs_create('default','/run.sh','file',?)", (0o750,))
        initial = db.json("SELECT vexfs_stat('default','/run.sh')")
        ctx.equal(initial["mode"], 0o750, "创建时 mode")
        ctx.equal(initial["version"], 1, "空文件初始内容版本")

        head_before = db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        ctx.equal(db.scalar("SELECT vexfs_set_mode('default',?,?)", (inode, 0o750)),
                  0o750, "相同 mode 幂等")
        ctx.equal(db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'"),
            head_before, "相同 mode 不产生提交")

        ctx.equal(db.scalar("SELECT vexfs_set_mode('default',?,?)", (inode, 0o700)),
                  0o700, "修改 mode")
        changed = db.json("SELECT vexfs_stat('default','/run.sh')")
        ctx.equal(changed["mode"], 0o700, "mode 已修改")
        ctx.equal(changed["version"], 1, "chmod 不修改内容版本")

        db.connection.execute("BEGIN")
        db.scalar("SELECT vexfs_set_mode('default',?,?)", (inode, 0o600))
        db.connection.execute("ROLLBACK")
        ctx.equal(db.json("SELECT vexfs_stat('default','/run.sh')")["mode"],
                  0o700, "chmod 事务回滚")
        ctx.expect_error(
            lambda: db.scalar("SELECT vexfs_set_mode('default',?,-1)", (inode,)),
            "0..0777")
        ctx.expect_error(
            lambda: db.scalar("SELECT vexfs_set_mode('default',?,512)", (inode,)),
            "0..0777")

        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/run.sh','rw','mode-open')")
        generation = db.scalar(
            "SELECT vexfs_handle_stage_write(?,0,'#!/bin/sh\necho ok\n','mode-write')",
            (handle,))
        db.scalar("SELECT vexfs_set_mode('default',?,?)", (inode, 0o755))
        published = db.scalar(
            "SELECT vexfs_handle_publish(?,?,'full','mode-publish')",
            (handle, generation))
        db.scalar("SELECT vexfs_handle_close(?,1,'mode-close')", (handle,))
        final = db.json("SELECT vexfs_stat('default','/run.sh')")
        ctx.equal((final["mode"], final["version"]), (0o755, published),
                  "打开句柄期间 chmod 后仍可发布")
        return {"inode": inode, "mode": final["mode"], "version": final["version"]}


@case("contract.timestamps", "functional",
      "独立 atime/mtime/ctime、元数据变更和 workspace 快照恢复")
def contract_timestamps(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        inode = db.scalar("SELECT vexfs_create('default','/times.txt','file',420)")
        initial = db.json("SELECT vexfs_stat('default','/times.txt')")
        for field in ("created_at", "accessed_at", "updated_at", "changed_at"):
            ctx.check(initial[field] > 0, f"{field} 必须存在")

        requested_atime = 1_700_000_000_123
        requested_mtime = 1_700_000_100_456
        ctx.equal(db.scalar("SELECT vexfs_set_times('default',?,?,?,3)",
                            (inode, requested_atime, requested_mtime)), 3,
                  "同时设置 atime/mtime")
        changed = db.json("SELECT vexfs_stat('default','/times.txt')")
        ctx.equal(changed["accessed_at"], requested_atime, "atime 独立持久化")
        ctx.equal(changed["updated_at"], requested_mtime, "mtime 独立持久化")
        ctx.check(changed["changed_at"] >= initial["changed_at"], "ctime 不能倒退")
        ctx.equal(changed["version"], initial["version"], "时间戳不修改内容版本")

        db.scalar("SELECT vexfs_snapshot_create('default','before-time-change')")
        second_atime = requested_atime + 10_000
        db.scalar("SELECT vexfs_set_times('default',?,?,?,1)",
                  (inode, second_atime, 0))
        access_only = db.json("SELECT vexfs_stat('default','/times.txt')")
        ctx.equal(access_only["accessed_at"], second_atime, "只更新 atime")
        ctx.equal(access_only["updated_at"], requested_mtime, "只更新 atime 不改 mtime")

        root_before = db.json("SELECT vexfs_stat('default','/')")
        time.sleep(0.01)
        db.scalar("SELECT vexfs_create('default','/child.txt','file',420)")
        root_after = db.json("SELECT vexfs_stat('default','/')")
        ctx.check(root_after["updated_at"] > root_before["updated_at"],
                  "目录项变化必须以毫秒精度推进目录 mtime")

        head = db.scalar("SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        db.scalar("SELECT vexfs_snapshot_restore('default','before-time-change',?)", (head,))
        restored = db.json("SELECT vexfs_stat('default','/times.txt')")
        ctx.equal(restored["accessed_at"], requested_atime, "快照恢复 atime")
        ctx.equal(restored["updated_at"], requested_mtime, "快照恢复 mtime")
        ctx.expect_error(
            lambda: db.scalar("SELECT vexfs_set_times('default',?,?,?,0)",
                              (inode, requested_atime, requested_mtime)),
            "time mask")
        ctx.expect_error(
            lambda: db.scalar("SELECT vexfs_set_times('default',?,-1,0,1)", (inode,)),
            "non-negative")
        return {"inode": inode, "atime": restored["accessed_at"],
                "mtime": restored["updated_at"], "ctime": restored["changed_at"]}


@case("contract.owner-acl", "functional", "owner uid/gid、chown 和数据库 ACL 元数据持久化")
def owner_acl(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        inode = db.scalar("SELECT vexfs_create('default','/owned','file',?)", (0o640,))
        db.scalar("SELECT vexfs_chown('default',?,?,?)", (inode, 1234, 2345))
        stat = db.json("SELECT vexfs_stat('default','/owned')")
        ctx.equal((stat["uid"], stat["gid"]), (1234, 2345), "chown uid/gid")
        ctx.equal(db.scalar("SELECT vexfs_chown('default',?,?,?)", (inode, -1, 3456)), 1234,
                  "chown -1 保留 uid")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_create('default','/fifo','fifo',?)", (0o644,)),
            "special files")
        db.scalar("SELECT vexfs_mkdir('default','/owned-dir')")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_link('default','/owned-dir','/owned-link')"),
            "regular files")
        acl = '[{"principal":"alice","effect":"allow","permissions":"read,write","inherit":1}]'
        ctx.equal(db.scalar("SELECT vexfs_acl_set('default',?,?)", (inode, acl)), 1,
                  "设置 ACL")
        ctx.equal(db.json("SELECT vexfs_acl_get('default',?)", (inode,))[0]["principal"],
                  "alice", "读取 ACL")
        db.scalar("SELECT vexfs_snapshot_create('default','owner-acl')")
        db.scalar("SELECT vexfs_chown('default',?,?,?)", (inode, 9, 9))
        db.scalar("SELECT vexfs_acl_delete('default',?)", (inode,))
        head = db.scalar("SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        db.scalar("SELECT vexfs_snapshot_restore('default','owner-acl',?)", (head,))
        restored = db.json("SELECT vexfs_stat('default','/owned')")
        ctx.equal((restored["uid"], restored["gid"]), (1234, 3456), "快照恢复 owner")
        ctx.equal(db.json("SELECT vexfs_acl_get('default',?)", (inode,))[0]["principal"],
                  "alice", "快照恢复 ACL")
        return {"inode": inode, "uid": restored["uid"], "gid": restored["gid"]}


@case("contract.symbolic-links", "functional", "相对、绝对、悬空符号链接和边界输入")
def symbolic_links(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-symlink-") as directory:
        path = Path(directory) / "links.sqlite3"
        db = Database(ctx, path)
        db.scalar("SELECT vexfs_mkdir('default','/workspace/bin')")
        db.scalar("SELECT vexfs_write('default','/workspace/bin/run.sh','echo ok')")
        targets = {
            "/workspace/run": b"bin/run.sh",
            "/workspace/dangling": b"missing/target",
            "/workspace/absolute": b"/workspace/bin/run.sh",
        }
        inodes: dict[str, int] = {}
        for link_path, target in targets.items():
            inode = db.scalar("SELECT vexfs_symlink('default',?,?)", (link_path, target))
            inodes[link_path] = inode
            stat = db.json("SELECT vexfs_stat('default',?)", (link_path,))
            ctx.equal((stat["kind"], stat["mode"], stat["size"]),
                      ("symlink", 0o777, len(target)), "符号链接元数据")
            ctx.equal(db.scalar("SELECT vexfs_readlink('default',?)", (inode,)),
                      target, "readlink 原样返回 target")

        listing = db.json("SELECT vexfs_list('default','/workspace')")
        listed = {entry["name"]: entry["kind"] for entry in listing}
        ctx.equal(listed["run"], "symlink", "list 暴露 symlink 类型")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_symlink('default','/workspace/run','other')"),
            "already exists")
        regular = db.json("SELECT vexfs_stat('default','/workspace/bin/run.sh')")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_readlink('default',?)", (regular["inode"],)),
            "not a symbolic link")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_set_mode('default',?,448)", (inodes["/workspace/run"],)),
            "cannot be changed")
        for invalid_target, message in ((b"", "must not be empty"),
                                        (b"bad\x00target", "contains NUL"),
                                        (b"x" * 4097, "longer than 4096")):
            ctx.expect_error(lambda target=invalid_target: db.scalar(
                "SELECT vexfs_symlink('default','/workspace/invalid',?)", (target,)),
                message)

        moved_inode = inodes["/workspace/run"]
        db.scalar("SELECT vexfs_move('default','/workspace/run','/workspace/moved')")
        ctx.equal(db.scalar("SELECT vexfs_readlink('default',?)", (moved_inode,)),
                  b"bin/run.sh", "重命名不修改 target")
        db.scalar("SELECT vexfs_remove('default','/workspace/moved',0)")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/workspace/bin/run.sh') AS TEXT)"),
                  "echo ok", "删除链接不删除目标")
        db.close()

        reopened = Database(ctx, path)
        dangling = reopened.json("SELECT vexfs_stat('default','/workspace/dangling')")
        ctx.equal(reopened.scalar("SELECT vexfs_readlink('default',?)", (dangling["inode"],)),
                  b"missing/target", "重开数据库后 target 持久化")
        reopened.close()
        return {"links": len(targets), "persisted": True}


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
            "SELECT version_no,size,manifest_id FROM _vexfs_file_versions "
            "WHERE inode_id=? ORDER BY version_no", (inode,)).fetchall()
        ctx.equal([(row[0], row[1]) for row in history], [(1, 3), (2, 3), (3, 5)],
                  "历史版本大小")
        ctx.check(all(row[2] is not None for row in history),
                  "canonical 版本均引用不可变 manifest")
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
        ctx.equal([row[1] for row in commits], [None, 1, 2, 3], "commit 父链")
        ctx.equal(db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'"), 4,
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
        return {"inode": inode, "versions": 4, "commits": 5, "restored": restored}


@case("contract.workspace-snapshot", "transaction",
      "完整 workspace 快照覆盖目录、内容、权限、符号链接和扩展属性")
def workspace_snapshot(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/project/src')")
        script = db.scalar(
            "SELECT vexfs_create('default','/project/run.sh','file',?)", (0o755,))
        db.scalar("SELECT vexfs_write('default','/project/run.sh','alpha')")
        db.scalar("SELECT vexfs_write('default','/project/src/keep.txt','keep')")
        link = db.scalar(
            "SELECT vexfs_symlink('default','/project/current',X'737263')")
        db.scalar(
            "SELECT vexfs_xattr_set('default',?,'user.vexfs.eval','old',0)", (script,))
        snapshot_head = db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        ctx.equal(db.scalar(
            "SELECT vexfs_snapshot_create('default','before',?)", (snapshot_head,)),
            snapshot_head, "创建快照")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_snapshot_create('default','before')"), "UNIQUE")
        shown = db.json("SELECT vexfs_snapshot_show('default','before')")
        ctx.equal(shown["commit"], snapshot_head, "快照绑定不可变 commit")
        ctx.equal({entry["path"] for entry in shown["entries"]},
                  {"/", "/project", "/project/current", "/project/run.sh",
                   "/project/src", "/project/src/keep.txt"},
                  "快照包含完整目录树")

        db.scalar("SELECT vexfs_write('default','/project/run.sh','beta')")
        db.scalar("SELECT vexfs_set_mode('default',?,?)", (script, 0o600))
        db.scalar(
            "SELECT vexfs_xattr_set('default',?,'user.vexfs.eval','new',0)", (script,))
        db.scalar("SELECT vexfs_move('default','/project/src','/project/lib')")
        db.scalar("SELECT vexfs_remove('default','/project/current',0)")
        db.scalar("SELECT vexfs_symlink('default','/project/current',X'6C6962')")
        db.scalar("SELECT vexfs_write('default','/project/new.txt','new')")
        changed_head = db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        difference = db.json(
            "SELECT vexfs_snapshot_diff('default','before','HEAD')")
        changes = {(entry["path"], entry["change"])
                   for entry in difference["changes"]}
        ctx.check(("/project/run.sh", "modify") in changes, "快照识别内容、mode 和 xattr")
        ctx.check(("/project/src", "delete") in changes and
                  ("/project/lib", "add") in changes, "快照识别目录移动")
        ctx.check(("/project/new.txt", "add") in changes, "快照识别新增文件")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_snapshot_restore('default','before',?)", (snapshot_head,)),
            "workspace head conflict")

        workspace_id = db.scalar(
            "SELECT id FROM _vexfs_workspaces WHERE name='default'")
        db.connection.execute(
            "INSERT INTO _vexfs_mount_sessions(workspace_id,session_id,lease_until) "
            "VALUES(?, 'eval-live-mount', (CAST(strftime('%s','now') AS INTEGER)+30)*1000)",
            (workspace_id,))
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_snapshot_restore('default','before',?)", (changed_head,)),
            "active mount session")
        db.connection.execute(
            "DELETE FROM _vexfs_mount_sessions WHERE workspace_id=?", (workspace_id,))

        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/project/run.sh','r','snapshot-open')")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_snapshot_restore('default','before',?)", (changed_head,)),
            "open or retained")
        db.scalar("SELECT vexfs_handle_close(?,0,'snapshot-close')", (handle,))
        restored_head = db.scalar(
            "SELECT vexfs_snapshot_restore('default','before',?)", (changed_head,))
        ctx.check(restored_head > changed_head, "还原产生新的 workspace commit")
        ctx.equal(db.scalar(
            "SELECT CAST(vexfs_read('default','/project/run.sh') AS TEXT)"),
            "alpha", "还原文件内容")
        ctx.equal(db.json("SELECT vexfs_stat('default','/project/run.sh')")["mode"],
                  0o755, "还原可执行权限")
        ctx.equal(db.scalar("SELECT CAST(vexfs_readlink('default',?) AS TEXT)", (link,)),
                  "src", "还原符号链接及 inode")
        ctx.equal(db.scalar(
            "SELECT CAST(vexfs_xattr_get('default',?,'user.vexfs.eval') AS TEXT)",
            (script,)), "old", "还原扩展属性")
        ctx.equal(db.scalar(
            "SELECT CAST(vexfs_read('default','/project/src/keep.txt') AS TEXT)"),
            "keep", "还原被移动的目录")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_stat('default','/project/new.txt')"), "path not found")
        ctx.equal(db.json(
            "SELECT vexfs_snapshot_diff('default','before','HEAD')")["changes"], [],
            "还原后逻辑状态和快照完全一致")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_snapshot_restore('default','before',?)", (restored_head,)),
            "already matches")

        maximum_version = db.scalar(
            "SELECT max(version_no) FROM _vexfs_file_versions WHERE inode_id=?", (script,))
        new_version = db.scalar(
            "SELECT vexfs_write('default','/project/run.sh','after-restore')")
        ctx.check(new_version > maximum_version, "还原后的再次写入版本继续单调增长")
        listing = db.json("SELECT vexfs_snapshot_list('default')")
        ctx.equal([(entry["name"], entry["commit"]) for entry in listing],
                  [("before", snapshot_head)], "列出快照")
        ctx.equal(db.scalar("SELECT vexfs_snapshot_drop('default','before')"), 1,
                  "删除快照")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_snapshot_show('default','before')"), "snapshot not found")
        ctx.equal(db.scalar("PRAGMA integrity_check"), "ok", "快照还原后完整性")
        ctx.equal(db.connection.execute("PRAGMA foreign_key_check").fetchall(), [],
                  "快照还原后外键完整性")
        return {"snapshot_commit": snapshot_head, "changed_commit": changed_head,
                "restored_commit": restored_head, "changes": len(changes),
                "next_version": new_version}


@case("contract.workspace-snapshot-hardlinks", "transaction",
      "workspace 快照保留普通文件的多路径硬链接关系并可完整还原")
def workspace_snapshot_hardlinks(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/links')")
        db.scalar("SELECT vexfs_mkdir('default','/links/left')")
        db.scalar("SELECT vexfs_mkdir('default','/links/right')")
        db.scalar("SELECT vexfs_write('default','/links/left/shared.txt','before')")
        inode = db.json("SELECT vexfs_stat('default','/links/left/shared.txt')")["inode"]
        db.scalar("SELECT vexfs_link('default','/links/left/shared.txt',"
                  "'/links/right/shared.txt')")
        right = db.json("SELECT vexfs_stat('default','/links/right/shared.txt')")
        ctx.equal(right["inode"], inode, "硬链接复用原 inode")

        snapshot_commit = db.scalar(
            "SELECT vexfs_snapshot_create('default','hardlinks')")
        shown = db.json("SELECT vexfs_snapshot_show('default','hardlinks')")
        entries = {entry["path"]: entry["state"] for entry in shown["entries"]}
        ctx.equal(entries["/links/left/shared.txt"]["inode"], inode,
                  "快照记录第一条硬链接")
        ctx.equal(entries["/links/right/shared.txt"]["inode"], inode,
                  "快照记录第二条硬链接")

        # Mutate through one name and remove the other.  Restore must put back
        # the deleted dentry and the content/metadata observed at the snapshot.
        db.scalar("SELECT vexfs_write('default','/links/left/shared.txt','after')")
        db.scalar("SELECT vexfs_remove('default','/links/right/shared.txt',0)")
        changed_head = db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        restored_head = db.scalar(
            "SELECT vexfs_snapshot_restore('default','hardlinks',?)", (changed_head,))
        ctx.check(restored_head > changed_head, "硬链接还原生成新 commit")
        left = db.json("SELECT vexfs_stat('default','/links/left/shared.txt')")
        right = db.json("SELECT vexfs_stat('default','/links/right/shared.txt')")
        ctx.equal(left["inode"], inode, "还原保留第一条硬链接 inode")
        ctx.equal(right["inode"], inode, "还原补回第二条硬链接 inode")
        ctx.equal(db.scalar(
            "SELECT CAST(vexfs_read('default','/links/right/shared.txt') AS TEXT)"),
            "before", "硬链接还原文件内容")
        ctx.equal(db.json(
            "SELECT vexfs_snapshot_diff('default','hardlinks','HEAD')")["changes"],
            [], "硬链接还原后快照无差异")
        return {"snapshot_commit": snapshot_commit, "changed_commit": changed_head,
                "restored_commit": restored_head, "inode": inode}


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
            "SELECT chunk.content FROM _vexfs_file_versions version "
            "JOIN _vexfs_manifest_chunks entry ON entry.manifest_id=version.manifest_id "
            "JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id "
            "WHERE version.inode_id=? AND version.version_no=2 AND entry.chunk_no=0", (inode,)),
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


@case("contract.atomic-append", "concurrency",
      "多个打开句柄按最新 inode 版本原子追加并刷新自身视图")
def atomic_append(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/append','base')")
        first = db.scalar("SELECT vexfs_handle_open('default','/append','rw','append-open-a')")
        second = db.scalar("SELECT vexfs_handle_open('default','/append','rw','append-open-b')")
        first_version = db.scalar(
            "SELECT vexfs_handle_append(?,'-a','append-a')", (first,))
        ctx.equal(db.scalar("SELECT vexfs_handle_read(?,0,64)", (first,)), b"base-a",
                  "追加后刷新当前句柄")
        second_version = db.scalar(
            "SELECT vexfs_handle_append(?,'-b','append-b')", (second,))
        ctx.check(second_version > first_version, "第二个句柄基于最新版本追加")
        ctx.equal(db.scalar("SELECT vexfs_handle_read(?,0,64)", (second,)), b"base-a-b",
                  "第二个句柄看到完整追加结果")
        final_version = db.scalar(
            "SELECT vexfs_handle_append(?,'-c','append-c')", (first,))
        ctx.equal(db.scalar("SELECT vexfs_handle_append(?,'-c','append-c')", (first,)),
                  final_version, "原子 append 请求可幂等重试")
        ctx.equal(db.scalar("SELECT vexfs_read('default','/append')"), b"base-a-b-c",
                  "多句柄追加不丢失")
        db.scalar("SELECT vexfs_handle_close(?,0,'append-close-a')", (first,))
        db.scalar("SELECT vexfs_handle_close(?,0,'append-close-b')", (second,))
        return {"first_version": first_version, "second_version": second_version,
                "final_version": final_version}


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


@case("maintenance.request-retention", "handles",
      "幂等请求日志有上限，并保留最近请求的重试结果")
def request_retention(ctx: Context) -> dict[str, Any]:
    retention_rows = 64 * 1024
    prune_interval = 4 * 1024
    with Database(ctx) as db:
        maximum = int(db.scalar("SELECT COALESCE(max(rowid),0) FROM _vexfs_requests"))
        trigger_rowid = ((maximum + retention_rows + prune_interval) // prune_interval
                         * prune_interval)
        seed_count = trigger_rowid - maximum - 1
        db.connection.execute("BEGIN")
        try:
            db.connection.executemany(
                "INSERT INTO _vexfs_requests(request_id,operation,request_fingerprint,"
                "result_integer) VALUES(?, 'retention-seed', X'', 0)",
                ((f"retention-seed-{index}",) for index in range(seed_count)))
            db.connection.execute("COMMIT")
        except Exception:
            db.connection.execute("ROLLBACK")
            raise

        result = db.scalar(
            "SELECT vexfs_item_reclaim('default','retention-trigger')")
        ctx.equal(db.scalar("SELECT max(rowid) FROM _vexfs_requests"), trigger_rowid,
                  "清理触发行号")
        ctx.equal(db.scalar("SELECT count(*) FROM _vexfs_requests"), retention_rows,
                  "幂等请求保留水位")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_requests WHERE request_id='retention-seed-0'"),
            0, "最旧请求被清理")
        ctx.equal(db.scalar(
            "SELECT vexfs_item_reclaim('default','retention-trigger')"), result,
            "最近请求仍可幂等重试")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_requests WHERE request_id='retention-trigger'"),
            1, "幂等重试不会新增请求")
        return {"retention_rows": retention_rows,
                "prune_interval": prune_interval,
                "seed_rows": seed_count,
                "retained_rows": db.scalar("SELECT count(*) FROM _vexfs_requests")}


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


@case("contract.cross-session-snapshot-barrier", "concurrency",
      "管理会话不能静默快照另一个挂载会话尚未发布的写入")
def cross_session_snapshot_barrier(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/snapshot')")
        db.scalar("SELECT vexfs_write('default','/snapshot/value.txt','base')")
        db.scalar("SELECT vexfs_mount_session_start('default','mount-owner')")
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/snapshot/value.txt','rw',"
            "'snapshot-open','mount-owner')")
        db.scalar("SELECT vexfs_handle_stage_write(?,0,'dirty','snapshot-stage')",
                  (handle,))
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_snapshot_create('default','must-block')"),
            "unpublished file handles")

        prefix = [str(ctx.cli), "--db", str(db.path), "--workspace", "default",
                  "--json", "snapshot", "create"]
        blocked = run_process(prefix + ["cli-must-block"], check=False)
        ctx.check(blocked.returncode != 0, "CLI consistent snapshot 必须失败")
        ctx.equal(json.loads(blocked.stderr)["error"]["code"], "VEXFS_BUSY",
                  "CLI 明确报告 BUSY")
        committed = json.loads(run_process(
            prefix + ["published-only", "--committed-only"]).stdout)
        ctx.equal(committed["consistency"], "committed-only",
                  "CLI 明确标记 committed-only")
        names = {item["name"] for item in db.json(
            "SELECT vexfs_snapshot_list('default')")}
        ctx.check("cli-must-block" not in names and "must-block" not in names,
                  "失败的快照没有留下名字")
        ctx.check("published-only" in names, "committed-only 快照已创建")

        ctx.equal(db.scalar(
            "SELECT vexfs_mount_synchronize('default','snapshot-sync','mount-owner')"),
            1, "owner 发布 staging")
        changes = db.json(
            "SELECT vexfs_snapshot_diff('default','published-only','HEAD')")["changes"]
        ctx.equal([item["path"] for item in changes], ["/snapshot/value.txt"],
                  "committed-only 与发布后 HEAD 的差异明确")
        db.scalar("SELECT vexfs_mount_session_end('default','mount-owner')")
        return {"blocked_status": "VEXFS_BUSY",
                "committed_snapshot": committed["commit"],
                "changes_after_publish": len(changes)}


@case("contract.unpublished-create-history", "transaction",
      "无关提交不能把尚未发布的 handle_create version 0 写入历史")
def unpublished_create_history(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        handle = db.scalar(
            "SELECT vexfs_handle_create('default','/index.lock',420,'create-lock')")
        inode = db.json("SELECT vexfs_stat('default','/index.lock')")["inode"]
        ctx.equal(db.json("SELECT vexfs_stat('default','/index.lock')")["version"], 0,
                  "新建句柄在首次发布前仍是暂存版本")
        db.scalar("SELECT vexfs_xattr_set('default',?,'user.vexfs.test','pending',0)",
                  (inode,))

        # Git 会在 index.lock 仍打开时创建 refs 等目录；这个独立提交过去会
        # 错把 version 0 刷进不可变历史，最终令快照无法完整恢复。
        db.scalar("SELECT vexfs_mkdir('default','/refs')")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_inode_states "
            "WHERE inode_id=? AND current_version=0", (inode,)), 0,
            "无关提交不记录未发布 inode")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_dentry_states WHERE inode_id=?", (inode,)), 0,
            "无关提交不记录未发布 dentry")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_dirty_inodes WHERE inode_id=?", (inode,)), 1,
            "未发布 inode 保留 dirty 状态")

        generation = db.scalar(
            "SELECT vexfs_handle_stage_write(?,0,'git-index','write-lock')", (handle,))
        ctx.equal(db.scalar(
            "SELECT vexfs_handle_publish(?,?,'data','publish-lock')",
            (handle, generation)), 1, "首次发布生成 version 1")
        db.scalar("SELECT vexfs_handle_close(?,0,'close-lock')", (handle,))
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_inode_states "
            "WHERE inode_id=? AND current_version=1", (inode,)), 1,
            "首次发布原子写入有效历史")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_dirty_inodes WHERE inode_id=?", (inode,)), 0,
            "首次发布清理 dirty inode")
        report = db.json("SELECT vexfs_check('default',1)")
        ctx.check(report["ok"], "真实 Git 锁文件顺序通过深度检查")
        return {"inode": inode, "generation": generation,
                "version": 1, "issue_count": report["issue_count"]}


@case("integrity.checksum-and-aliases", "integrity",
      "SHA-256 覆盖普通版本、单文件恢复和 workspace 快照恢复，损坏内容不得返回")
def integrity_checksum_and_aliases(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        ctx.equal(db.scalar("SELECT vexfs_write('default','/checked.txt','alpha')"), 1,
                  "写入第一个版本")
        ctx.equal(db.json("SELECT vexfs_stat('default','/checked.txt')")["checksum"],
                  sha256(b"alpha"), "stat 返回真实 SHA-256")
        ctx.equal(db.scalar("SELECT vexfs_write('default','/checked.txt','beta')"), 2,
                  "写入第二个版本")
        ctx.equal(db.scalar(
            "SELECT vexfs_restore_version('default','/checked.txt',1,2)"), 3,
            "单文件恢复创建别名版本")
        snapshot_commit = db.scalar(
            "SELECT vexfs_snapshot_create('default','checked-baseline')")
        ctx.equal(db.scalar("SELECT vexfs_write('default','/checked.txt','gamma')"), 4,
                  "快照后写入新版本")
        head = db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        restored_commit = db.scalar(
            "SELECT vexfs_snapshot_restore('default','checked-baseline',?)", (head,))
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/checked.txt') AS TEXT)"),
                  "alpha", "快照恢复后的内容")
        inode = db.json("SELECT vexfs_stat('default','/checked.txt')")["inode"]
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_file_versions alias "
            "JOIN _vexfs_file_versions source ON source.inode_id=alias.inode_id "
            "AND source.version_no=alias.source_version_no "
            "WHERE alias.inode_id=? AND alias.source_version_no IS NOT NULL "
            "AND (alias.checksum<>source.checksum OR alias.size<>source.size "
            "OR source.source_version_no IS NOT NULL)", (inode,)), 0,
            "所有恢复别名直接引用 canonical 版本并复制校验和")
        aliases = db.scalar(
            "SELECT count(*) FROM _vexfs_file_versions WHERE inode_id=? "
            "AND source_version_no IS NOT NULL", (inode,))
        ctx.check(aliases >= 2, "单文件恢复和快照恢复都产生版本别名")

        deep = db.json("SELECT vexfs_check('default',1)")
        quick = db.json("SELECT vexfs_check('default',0)")
        ctx.check(deep["ok"] and quick["ok"], "干净 workspace 深度和快速检查通过")
        ctx.equal(deep["content_model"], "chunked-v1", "检查明确当前内容模型")
        ctx.equal(deep["checked"]["snapshots"], 1, "检查覆盖快照引用")

        db.connection.execute(
            "UPDATE _vexfs_chunks SET content=zeroblob(size) WHERE id=("
            "SELECT entry.chunk_id FROM _vexfs_file_versions version "
            "JOIN _vexfs_manifest_chunks entry ON entry.manifest_id=version.manifest_id "
            "WHERE version.inode_id=? AND version.version_no=1 AND entry.chunk_no=0)", (inode,))
        quick_after = db.json("SELECT vexfs_check('default',0)")
        ctx.check(quick_after["ok"], "快速检查明确不读取 BLOB 校验和")
        deep_after = db.json("SELECT vexfs_check('default',1)")
        codes = {issue["code"] for issue in deep_after["issues"]}
        ctx.check(not deep_after["ok"] and "VEXFS_CHUNK_INVALID" in codes,
                  "深度检查发现同长度内容损坏")
        ctx.expect_error(
            lambda: db.scalar("SELECT vexfs_read('default','/checked.txt')"),
            "chunk is corrupt")

        cli = run_process(
            [str(ctx.cli), "--db", str(db.path), "--workspace", "default",
             "--json", "check"], check=False)
        ctx.equal(cli.returncode, 8, "CLI 对损坏返回稳定的 corruption 退出码")
        cli_report = json.loads(cli.stdout)
        ctx.check(not cli_report["ok"], "CLI 仍把完整检查报告写到 stdout")
        return {"snapshot_commit": snapshot_commit,
                "restored_commit": restored_commit,
                "aliases": aliases,
                "clean_versions": deep["checked"]["versions"],
                "corruption_codes": sorted(codes),
                "cli_exit_code": cli.returncode}


@case("integrity.structural-corruption", "integrity",
      "目录、提交、快照、版本别名和 staging 引用损坏均被只读检查发现")
def integrity_structural_corruption(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/tree')")
        db.scalar("SELECT vexfs_write('default','/tree/value.txt','one')")
        db.scalar("SELECT vexfs_write('default','/tree/value.txt','two')")
        db.scalar("SELECT vexfs_restore_version('default','/tree/value.txt',1,2)")
        db.scalar("SELECT vexfs_snapshot_create('default','broken-snapshot')")
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/tree/value.txt','rw','check-staging')")
        db.scalar("SELECT vexfs_handle_stage_write(?,0,'dirty','check-stage')", (handle,))

        db.connection.execute(
            "UPDATE _vexfs_dentries SET parent_inode=999999 WHERE name='value.txt'")
        db.connection.execute(
            "UPDATE _vexfs_commits SET parent_commit=999998 "
            "WHERE id=(SELECT max(id) FROM _vexfs_commits)")
        db.connection.execute(
            "UPDATE _vexfs_snapshots SET commit_id=999997 WHERE name='broken-snapshot'")
        db.connection.execute(
            "UPDATE _vexfs_file_versions SET source_version_no=999996 "
            "WHERE source_version_no IS NOT NULL")
        db.connection.execute("DELETE FROM _vexfs_staging_data WHERE handle_id=?", (handle,))

        report = db.json("SELECT vexfs_check('default',0)")
        codes = {issue["code"] for issue in report["issues"]}
        expected = {
            "VEXFS_DENTRY_PARENT_INVALID", "VEXFS_INODE_UNREACHABLE",
            "VEXFS_COMMIT_PARENT_MISSING", "VEXFS_SNAPSHOT_COMMIT_MISSING",
            "VEXFS_VERSION_SOURCE_INVALID", "VEXFS_STAGING_MISSING",
        }
        ctx.check(not report["ok"], "结构损坏必须失败")
        ctx.equal(expected - codes, set(), "所有结构损坏均有稳定问题码")
        ctx.check(all(issue["object"] and issue["type"] and issue["suggestion"]
                      for issue in report["issues"]),
                  "每个问题都包含对象、类型和建议动作")
        return {"issue_count": report["issue_count"], "codes": sorted(codes)}


@case("maintenance.retention-gc", "maintenance",
      "保留数、天数、快照、句柄、内容别名、活动挂载和分批 GC 的安全边界")
def retention_gc(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        for value in ("one", "two", "three"):
            db.scalar("SELECT vexfs_write('default','/history.txt',?)", (value,))
        snapshot_commit = db.scalar(
            "SELECT vexfs_snapshot_create('default','protected-version-3')")
        db.scalar("SELECT vexfs_write('default','/history.txt','four')")
        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/history.txt','r','gc-open')")
        db.scalar("SELECT vexfs_restore_version('default','/history.txt',1,4)")
        policy = db.json("SELECT vexfs_retention_set('default',1,0)")
        ctx.equal(policy["keep_versions"], 1, "保留最近版本数")
        ctx.equal(policy["keep_days"], 0, "关闭按天保留")
        ctx.check(policy["reclaimable_versions"] >= 1, "存在可回收历史")

        db.scalar("SELECT vexfs_mount_session_start('default','gc-mount')")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_gc('default',1)"),
                         "active mount session")
        db.scalar("SELECT vexfs_mount_session_end('default','gc-mount')")
        ctx.check(db.json("SELECT vexfs_gc_pause('default',1)")["gc_paused"],
                  "显式暂停 GC")
        ctx.expect_error(lambda: db.scalar("SELECT vexfs_gc('default',1)"),
                         "GC is paused")
        ctx.check(not db.json("SELECT vexfs_gc_pause('default',0)")["gc_paused"],
                  "显式恢复 GC")

        batches = 0
        deleted = 0
        while True:
            result = db.json("SELECT vexfs_gc('default',1)")
            batches += 1
            deleted += result["deleted_versions"]
            if not result["has_more"]:
                break
            ctx.check(batches < 100, "GC 必须在有界批次数内完成")
        ctx.check(deleted >= 1, "GC 实际删除历史版本")
        ctx.equal(db.scalar(
            "SELECT CAST(vexfs_read_version('default','/history.txt',3) AS TEXT)"),
            "three", "快照引用版本未删除")
        ctx.equal(db.scalar(
            "SELECT CAST(vexfs_read_version('default','/history.txt',4) AS TEXT)"),
            "four", "打开句柄引用版本未删除")
        ctx.equal(db.scalar(
            "SELECT CAST(vexfs_read('default','/history.txt') AS TEXT)"),
            "one", "当前别名版本和 canonical 内容未删除")
        check = db.json("SELECT vexfs_check('default',1)")
        ctx.check(check["ok"], "GC 后深度检查通过")
        db.scalar("SELECT vexfs_handle_close(?,0,'gc-close')", (handle,))
        return {"snapshot_commit": snapshot_commit, "batches": batches,
                "deleted_versions": deleted,
                "remaining_versions": check["checked"]["versions"]}


@case("limits.workspace-quota", "limits",
      "文件数、当前字节、单文件配额在发布和快照恢复前原子检查")
def workspace_quota(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_write('default','/quota.txt','abc')")
        inode = db.json("SELECT vexfs_stat('default','/quota.txt')")["inode"]
        before_commit = db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        quota = db.json("SELECT vexfs_quota_set('default',3,1,3)")
        ctx.equal((quota["live_bytes"], quota["live_files"]), (3, 1),
                  "配额统计当前 live usage")
        db.scalar("SELECT vexfs_link('default','/quota.txt','/quota-link.txt')")
        ctx.equal(db.json("SELECT vexfs_quota_get('default')")["live_files"], 1,
                  "hardlink 不重复计算文件数")
        ctx.expect_error(
            lambda: db.scalar("SELECT vexfs_write('default','/second.txt','x')"),
            "file quota exceeded")
        ctx.expect_error(
            lambda: db.scalar("SELECT vexfs_write('default','/quota.txt','abcd')"),
            "maximum file size quota exceeded")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/quota.txt') AS TEXT)"),
                  "abc", "失败写入不改正文")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_file_versions WHERE inode_id=?", (inode,)),
            1, "失败写入不产生版本")
        ctx.equal(db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'"),
            before_commit + 1, "只有 hardlink 成功操作产生 commit")

        db.scalar("SELECT vexfs_quota_set('default',NULL,NULL,NULL)")
        snapshot = db.scalar("SELECT vexfs_snapshot_create('default','large')")
        db.scalar("SELECT vexfs_write('default','/quota.txt','x')")
        current_head = db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        db.scalar("SELECT vexfs_quota_set('default',NULL,NULL,2)")
        ctx.expect_error(lambda: db.scalar(
            "SELECT vexfs_snapshot_restore('default','large',?)", (current_head,)),
            "maximum file size quota")
        ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/quota.txt') AS TEXT)"),
                  "x", "失败快照恢复不修改工作区")
        ctx.equal(db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'"),
            current_head, "失败快照恢复不产生 commit")
        return {"snapshot_commit": snapshot, "live_files": 1,
                "rejected_operations": 3}


@case("backup.logical-export-import", "backup",
      "逻辑包记录、整包和 BLOB 校验，指定快照导出，导入原子发布")
def logical_export_import(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-export-eval-") as directory:
        base = Path(directory)
        source_path = base / "source.sqlite3"
        destination_path = base / "destination.sqlite3"
        archive = base / "workspace.vexfs"
        dirty_archive = base / "dirty-head.vexfs"
        corrupt_archive = base / "corrupt.vexfs"
        missing_source = base / "missing.sqlite3"
        missing_source_archive = base / "missing-source.vexfs"
        missing_export = run_process(
            [str(ctx.cli), "--db", str(missing_source), "--workspace", "default",
             "export", "--output", str(missing_source_archive)], check=False)
        ctx.check(missing_export.returncode != 0 and not missing_source.exists() and
                  not missing_source_archive.exists(),
                  "不存在的源数据库导出失败且不创建空数据库或归档")
        with Database(ctx, source_path) as source:
            source.scalar("SELECT vexfs_mkdir('default','/project')")
            source.scalar("SELECT vexfs_write('default','/project/data.bin',?)",
                          (sqlite3.Binary(b"A" * min(ctx.mode.sequential_mib, 16) * MIB),))
            source.scalar("SELECT vexfs_write('default','/project/version.txt','one')")
            source.scalar("SELECT vexfs_write('default','/project/version.txt','two')")
            inode = source.json(
                "SELECT vexfs_stat('default','/project/version.txt')")["inode"]
            source.scalar("SELECT vexfs_link('default','/project/version.txt',"
                          "'/project/version-link.txt')")
            source.scalar("SELECT vexfs_xattr_set('default',?,'user.export','value',0)",
                          (inode,))
            source.scalar("SELECT vexfs_acl_set('default',?,"
                          "'[{\"principal\":\"alice\",\"effect\":\"allow\","
                          "\"permissions\":\"read\",\"inherit\":1}]')", (inode,))
            source.scalar("SELECT vexfs_write('default','/project/deleted.lock','temp')")
            deleted_inode = source.json(
                "SELECT vexfs_stat('default','/project/deleted.lock')")["inode"]
            source.scalar(
                "SELECT vexfs_xattr_set('default',?,'user.deleted','temp',0)",
                (deleted_inode,))
            source.scalar(
                "SELECT vexfs_acl_set('default',?,"
                "'[{\"principal\":\"local\",\"effect\":\"allow\","
                "\"permissions\":\"read,write\",\"inherit\":0}]')", (deleted_inode,))
            source.scalar("SELECT vexfs_remove('default','/project/deleted.lock',0)")
            snapshot_commit = source.scalar(
                "SELECT vexfs_snapshot_create('default','portable')")
            source.scalar("SELECT vexfs_write('default','/project/version.txt','after')")
            dirty_handle = source.scalar(
                "SELECT vexfs_handle_open('default','/project/version.txt','rw','export-open')")
            source.scalar(
                "SELECT vexfs_handle_stage_write(?,0,'dirty','export-stage')",
                (dirty_handle,))

        prefix = [str(ctx.cli), "--db", str(source_path), "--workspace", "default"]
        dirty_export = run_process(
            prefix + ["export", "--output", str(dirty_archive)], check=False)
        ctx.check(dirty_export.returncode != 0 and
                  b"unpublished file handles" in dirty_export.stderr and
                  not dirty_archive.exists(),
                  "HEAD 导出拒绝未发布句柄且不留半个包")
        export_started = time.perf_counter()
        exported = json.loads(run_process(
            prefix + ["export", "--snapshot", "portable", "--output", str(archive)]
        ).stdout)
        export_seconds = time.perf_counter() - export_started
        ctx.equal(archive.stat().st_mode & 0o777, 0o600,
                  "逻辑包默认只允许当前用户读写")
        archive.chmod(0o400)
        verified = json.loads(run_process(
            [str(ctx.cli), "archive", "verify", str(archive)]).stdout)
        ctx.check(verified["ok"], "逻辑包独立校验通过")
        ctx.equal(exported["source_commit"], snapshot_commit, "导出固定快照 commit")
        ctx.equal(exported["package_checksum"], verified["package_checksum"],
                  "导出和校验整包 hash 一致")
        with sqlite3.connect(f"file:{archive}?mode=ro", uri=True) as package:
            stale_metadata = package.execute(
                "SELECT (SELECT count(*) FROM xattrs attribute "
                "WHERE NOT EXISTS(SELECT 1 FROM inodes inode "
                "WHERE inode.source_id=attribute.source_inode "
                "AND inode.deleted_at IS NULL)) + "
                "(SELECT count(*) FROM acl_entries entry "
                "WHERE NOT EXISTS(SELECT 1 FROM inodes inode "
                "WHERE inode.source_id=entry.source_inode "
                "AND inode.deleted_at IS NULL))").fetchone()[0]
        ctx.equal(stale_metadata, 0,
                  "快照导出不把已删除 inode 的 xattr/ACL 当作当前元数据")

        import_started = time.perf_counter()
        imported = json.loads(run_process(
            [str(ctx.cli), "--db", str(destination_path), "--workspace", "restored",
             "import", str(archive)]).stdout)
        import_seconds = time.perf_counter() - import_started
        ctx.budget("logical_export_seconds", export_seconds, 15.0)
        ctx.budget("logical_import_seconds", import_seconds, 15.0)
        with Database(ctx, destination_path, initialize=False) as destination:
            ctx.equal(destination_path.stat().st_mode & 0o777, 0o600,
                      "新目标数据库默认只允许当前用户读写")
            ctx.equal(destination.scalar(
                "SELECT CAST(vexfs_read('restored','/project/version.txt') AS TEXT)"),
                "two", "指定快照内容导入")
            left = destination.json(
                "SELECT vexfs_stat('restored','/project/version.txt')")
            right = destination.json(
                "SELECT vexfs_stat('restored','/project/version-link.txt')")
            ctx.equal((left["inode"], left["link_count"]),
                      (right["inode"], 2), "hardlink 关系导入")
            ctx.equal(destination.scalar(
                "SELECT CAST(vexfs_read_version('restored','/project/version.txt',1) AS TEXT)"),
                "one", "文件版本历史导入")
            ctx.equal(destination.scalar(
                "SELECT CAST(vexfs_xattr_get('restored',?,'user.export') AS TEXT)",
                (left["inode"],)), "value", "xattr 导入")
            ctx.equal(destination.json(
                "SELECT vexfs_acl_get('restored',?)", (left["inode"],))[0]["principal"],
                "alice", "ACL 导入")
            ctx.check(destination.json("SELECT vexfs_check('restored',1)")["ok"],
                      "导入后深度检查通过")

        shutil.copyfile(archive, corrupt_archive)
        tampered = sqlite3.connect(corrupt_archive)
        tampered_row = tampered.execute(
            "SELECT id FROM chunks WHERE size>0 ORDER BY id LIMIT 1").fetchone()
        ctx.check(tampered_row is not None, "损坏测试找到 canonical 内容")
        tampered.execute(
            "UPDATE chunks SET content=zeroblob(size) WHERE id=?",
            (tampered_row[0],))
        tampered.commit()
        tampered.close()
        rejected = run_process(
            [str(ctx.cli), "archive", "verify", str(corrupt_archive)], check=False)
        ctx.check(rejected.returncode != 0 and b"checksum mismatch" in rejected.stderr,
                  "内容损坏必须被校验拒绝")
        rejected_import = run_process(
            [str(ctx.cli), "--db", str(destination_path), "--workspace", "partial",
             "import", str(corrupt_archive)], check=False)
        ctx.check(rejected_import.returncode != 0, "损坏包导入失败")
        with Database(ctx, destination_path, initialize=False) as destination:
            ctx.equal(destination.scalar(
                "SELECT count(*) FROM _vexfs_workspaces WHERE name='partial'"), 0,
                "失败导入不发布半个 workspace")
        corrupt_destination = base / "corrupt-destination.sqlite3"
        rejected_new_database = run_process(
            [str(ctx.cli), "--db", str(corrupt_destination), "--workspace", "partial",
             "import", str(corrupt_archive)], check=False)
        ctx.check(rejected_new_database.returncode != 0 and
                  not corrupt_destination.exists(),
                  "损坏包导入新目标失败时删除新建数据库")
        return {"source_commit": snapshot_commit,
                "archive_bytes": archive.stat().st_size,
                "versions": verified["versions"],
                "content_bytes": verified["content_bytes"],
                "export_seconds": round(export_seconds, 6),
                "import_seconds": round(import_seconds, 6),
                "package_checksum": imported["package_checksum"]}


@case("performance.quota-and-gc-scale", "performance",
      "开启配额后的文件创建为常数时间计数，历史 GC 保持固定批次和受控内存")
def quota_and_gc_scale(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        file_count = ctx.mode.small_files
        db.scalar("SELECT vexfs_mkdir('default','/quota-scale')")
        db.scalar("SELECT vexfs_quota_set('default',?,?,?)",
                  (file_count + 1, file_count + 1, 1024))
        started = time.perf_counter()
        db.connection.execute("BEGIN")
        try:
            for index in range(file_count):
                db.scalar("SELECT vexfs_write('default',?,'x')",
                          (f"/quota-scale/f{index:06d}",))
            db.connection.execute("COMMIT")
        except Exception:
            db.connection.execute("ROLLBACK")
            raise
        quota_seconds = time.perf_counter() - started
        usage = db.json("SELECT vexfs_quota_get('default')")
        ctx.equal(usage["live_files"], file_count, "规模写入文件计数")
        ctx.equal(usage["live_bytes"], file_count, "规模写入字节计数")

        versions = ctx.mode.random_ops
        db.scalar("SELECT vexfs_quota_set('default',NULL,NULL,NULL)")
        for index in range(versions):
            db.scalar("SELECT vexfs_write('default','/gc-scale',?)",
                      (f"value-{index:08d}",))
        db.scalar("SELECT vexfs_retention_set('default',1,0)")
        gc_started = time.perf_counter()
        batches = 0
        deleted = 0
        while True:
            result = db.json("SELECT vexfs_gc('default',128)")
            batches += 1
            deleted += result["deleted_versions"]
            if not result["has_more"]:
                break
            ctx.check(batches <= (versions // 128) + 4, "GC 批次数受控")
        gc_seconds = time.perf_counter() - gc_started
        ctx.equal(deleted, versions - 1, "只保留当前版本")
        ctx.check(db.json("SELECT vexfs_check('default',0)")["ok"],
                  "规模 GC 后结构检查通过")
        ctx.budget("quota_scale_seconds", quota_seconds,
                   max(5.0, file_count / 300.0))
        ctx.budget("gc_scale_seconds", gc_seconds,
                   max(5.0, versions / 1000.0))
        return {"files": file_count, "versions": versions,
                "quota_seconds": round(quota_seconds, 6),
                "quota_files_per_second": round(file_count / max(quota_seconds, 1e-9), 3),
                "gc_seconds": round(gc_seconds, 6), "gc_batches": batches,
                "deleted_versions": deleted}


@case("storage.chunk-manifest-reuse", "storage",
      "64 KiB manifest/chunk、随机覆盖复用未变化块、恢复别名和 GC 共享块安全")
def chunk_manifest_reuse(ctx: Context) -> dict[str, Any]:
    with Database(ctx) as db:
        chunk_bytes = 64 * 1024
        original = b"".join(bytes([index + 1]) * chunk_bytes for index in range(4))
        ctx.equal(db.scalar("SELECT vexfs_write('default','/chunked.bin',?)", (original,)),
                  1, "创建四块 canonical 版本")
        inode = db.json("SELECT vexfs_stat('default','/chunked.bin')")["inode"]
        first_manifest = db.scalar(
            "SELECT manifest_id FROM _vexfs_file_versions "
            "WHERE inode_id=? AND version_no=1", (inode,))
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_manifest_chunks WHERE manifest_id=?",
            (first_manifest,)), 4, "首版本 manifest 有四个块引用")

        handle = db.scalar(
            "SELECT vexfs_handle_open('default','/chunked.bin','rw','chunk-open')")
        patch_offset = chunk_bytes + 123
        patch = b"changed-block"
        generation = db.scalar(
            "SELECT vexfs_handle_stage_write(?,?,?,'chunk-patch')",
            (handle, patch_offset, patch))
        ctx.equal(db.scalar(
            "SELECT vexfs_handle_publish(?,?,'data','chunk-publish')",
            (handle, generation)), 2, "随机覆盖发布第二版本")
        db.scalar("SELECT vexfs_handle_close(?,0,'chunk-close')", (handle,))
        second_manifest = db.scalar(
            "SELECT manifest_id FROM _vexfs_file_versions "
            "WHERE inode_id=? AND version_no=2", (inode,))
        reused = db.scalar(
            "SELECT count(*) FROM _vexfs_manifest_chunks first "
            "JOIN _vexfs_manifest_chunks second "
            "ON second.chunk_no=first.chunk_no AND second.chunk_id=first.chunk_id "
            "WHERE first.manifest_id=? AND second.manifest_id=?",
            (first_manifest, second_manifest))
        ctx.equal(reused, 3, "只重写受影响的一块，其余三块复用")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_chunks WHERE inode_id=?", (inode,)), 5,
            "两个四块版本只保存五个物理块")
        expected = bytearray(original)
        expected[patch_offset:patch_offset + len(patch)] = patch
        ctx.equal(db.scalar("SELECT vexfs_read('default','/chunked.bin')"), bytes(expected),
                  "分块随机覆盖后的完整内容")

        ctx.equal(db.scalar(
            "SELECT vexfs_restore_version('default','/chunked.bin',1,2)"), 3,
            "恢复版本仍使用文件版本别名")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_manifests WHERE workspace_id=("
            "SELECT id FROM _vexfs_workspaces WHERE name='default')"), 2,
            "恢复别名不创建重复 manifest")
        db.scalar("SELECT vexfs_retention_set('default',1,0)")
        gc = db.json("SELECT vexfs_gc('default',100)")
        ctx.equal(gc["deleted_versions"], 1, "GC 删除不再需要的覆盖版本")
        ctx.equal(db.scalar(
            "SELECT count(*) FROM _vexfs_chunks WHERE inode_id=?", (inode,)), 4,
            "GC 只删除覆盖版本独有块，保留共享块")
        report = db.json("SELECT vexfs_check('default',1)")
        ctx.check(report["ok"], "共享块 GC 后深度检查通过")
        ctx.equal((report["checked"]["manifests"], report["checked"]["chunks"]),
                  (1, 4), "检查报告包含真实 manifest 和物理块数量")
        return {"logical_bytes": len(original), "chunk_bytes": chunk_bytes,
                "reused_chunks": reused, "physical_chunks_before_gc": 5,
                "physical_chunks_after_gc": 4}


@case("recovery.initialization-failure-rollback", "recovery",
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
        snapshot_commit = db.scalar(
            "SELECT vexfs_snapshot_create('default','before-reopen')")
        db.scalar("SELECT vexfs_write('default','/persist/blob','changed')")
        db.connection.execute("PRAGMA wal_checkpoint(FULL)").fetchone()
        db.close()
        reopened = Database(ctx, path)
        ctx.equal(reopened.json(
            "SELECT vexfs_snapshot_list('default')")[0]["commit"],
            snapshot_commit, "重开后快照仍存在")
        current_head = reopened.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        reopened.scalar(
            "SELECT vexfs_snapshot_restore('default','before-reopen',?)", (current_head,))
        ctx.equal(reopened.scalar("SELECT vexfs_read('default','/persist/blob')"), payload,
                  "重开后可从快照还原内容")
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
        snapshot_commit = source.scalar(
            "SELECT vexfs_snapshot_create('default','backup-point')")
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
        ctx.equal(restored.json(
            "SELECT vexfs_snapshot_list('default')")[0]["commit"],
            snapshot_commit, "备份保留 workspace 快照")
        ctx.equal(restored.json(
            "SELECT vexfs_snapshot_diff('default','backup-point','HEAD')")["changes"],
            [], "未发布 staging 不污染快照 head")
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


@case("concurrency.workspace-snapshot-restore-race", "concurrency",
      "两个真实进程用同一 workspace head 还原快照时只能有一个成功")
def workspace_snapshot_restore_race(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-snapshot-race-") as directory:
        base = Path(directory)
        path = base / "db.sqlite3"
        script = Path(__file__).resolve()
        with Database(ctx, path) as db:
            db.scalar("SELECT vexfs_write('default','/race','before')")
            snapshot_commit = db.scalar(
                "SELECT vexfs_snapshot_create('default','race-point')")
            db.scalar("SELECT vexfs_write('default','/race','after')")
            expected_head = db.scalar(
                "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
            commits_before = db.scalar("SELECT count(*) FROM _vexfs_commits")
        barrier = base / "go"
        ready_paths = [base / "ready-snapshot-0", base / "ready-snapshot-1"]
        processes = [subprocess.Popen(
            [sys.executable, str(script), "--worker", "snapshot-restore-race", str(path),
             str(ctx.extension), str(expected_head), str(ready_paths[index]), str(barrier)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE) for index in range(2)]
        deadline = time.time() + 10
        while not all(candidate.exists() for candidate in ready_paths):
            if time.time() >= deadline:
                raise EvalFailure("snapshot restore worker 未就绪")
            time.sleep(0.01)
        barrier.touch()
        results: list[tuple[int, bytes, bytes]] = []
        for process in processes:
            stdout, stderr = process.communicate(timeout=30)
            results.append((process.returncode, stdout, stderr))
        ctx.equal(sorted(result[0] for result in results), [0, 5],
                  "workspace snapshot restore 并发结果")
        with Database(ctx, path) as db:
            ctx.equal(db.scalar("SELECT count(*) FROM _vexfs_commits"), commits_before + 1,
                      "并发 workspace restore 只新增一个 commit")
            ctx.equal(db.scalar("SELECT CAST(vexfs_read('default','/race') AS TEXT)"),
                      "before", "成功还原完整 workspace")
            ctx.equal(db.json(
                "SELECT vexfs_snapshot_diff('default','race-point','HEAD')")["changes"],
                [], "成功还原后的 workspace 与快照一致")
            ctx.equal(db.json(
                "SELECT vexfs_snapshot_show('default','race-point')")["commit"],
                snapshot_commit, "并发还原不移动快照")
        return {"exit_codes": [result[0] for result in results],
                "snapshot_commit": snapshot_commit, "expected_head": expected_head}


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
        grep_json = json.loads(run_process(
            prefix + ["--json", "grep", "ONE", "/cli/a", "-i", "-n"]).stdout)
        ctx.equal(grep_json["match_count"], 1, "CLI 数据库 grep 命中数")
        ctx.equal(grep_json["matches"][0],
                  {"path": "/cli/a/version.txt", "line": 1, "text": "one"},
                  "CLI 数据库 grep 当前版本")
        ctx.equal(grep_json["binary_files_skipped"], 1, "CLI 数据库 grep 跳过二进制")
        index_before = json.loads(run_process(prefix + ["index", "status"]).stdout)
        ctx.equal(index_before["enabled"], False, "CLI 文本索引默认关闭")
        index_enabled = json.loads(run_process(prefix + ["index", "enable"]).stdout)
        ctx.equal(index_enabled["available"], True, "CLI 启用 trigram 文本索引")
        indexed_grep = json.loads(run_process(
            prefix + ["--json", "grep", "one", "/cli/a"]).stdout)
        ctx.equal(indexed_grep["index_used"], True, "CLI grep 使用 trigram 索引")
        ctx.equal(indexed_grep["files_scanned"], 1, "CLI 索引只读取候选文件")
        grep_text = run_process(prefix + ["grep", "-n", "one", "/cli/a"])
        ctx.equal(grep_text.stdout, b"/cli/a/version.txt:1:one\n", "CLI grep 文本输出")
        grep_files = run_process(prefix + ["grep", "-i", "-l", "ONE", "/cli/a"])
        ctx.equal(grep_files.stdout, b"/cli/a/version.txt\n", "CLI grep 文件名输出")
        ctx.equal(run_process(prefix + ["grep", "absent", "/cli/a"], check=False).returncode,
                  1, "CLI grep 无匹配退出码")

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
        snapshot = json.loads(run_process(
            prefix + ["--json", "snapshot", "create", "cli-stable"]).stdout)
        ctx.check(snapshot["commit"] > 0, "CLI 创建 workspace 快照")
        run_process(prefix + ["rm", "/cli/a/newline.txt"])
        run_process(prefix + ["rm", "/cli/a/version.txt"])
        run_process(prefix + ["mv", "/cli/a/payload.bin", "/cli/b/moved.bin"])
        run_process(prefix + ["rm", "/cli/a"])
        snapshot_diff = run_process(
            prefix + ["--json", "snapshot", "diff", "cli-stable"], check=False)
        ctx.equal(snapshot_diff.returncode, 1, "CLI workspace diff 不同退出码")
        ctx.check(json.loads(snapshot_diff.stdout)["changes"], "CLI workspace diff 内容")
        preview = json.loads(run_process(
            prefix + ["--json", "snapshot", "restore", "cli-stable", "--dry-run"]).stdout)
        ctx.check(preview["changes"], "CLI workspace restore dry-run")
        restored_snapshot = json.loads(run_process(
            prefix + ["--json", "snapshot", "restore", "cli-stable"]).stdout)
        ctx.check(restored_snapshot["commit"] > restored_snapshot["previous_head"],
                  "CLI workspace restore 新 commit")
        ctx.equal(run_process(prefix + ["cat", "/cli/a/payload.bin"]).stdout,
                  payload, "CLI workspace restore 文件树")
        restored_grep = json.loads(run_process(
            prefix + ["--json", "grep", "one", "/cli/a"]).stdout)
        ctx.equal(restored_grep["index_used"], True, "CLI workspace restore 后索引仍启用")
        ctx.equal(restored_grep["match_count"], 1, "CLI workspace restore 后索引内容正确")
        ctx.equal(run_process(
            prefix + ["snapshot", "diff", "cli-stable"], check=False).returncode,
            0, "CLI workspace restore 后无差异")
        snapshot_list = json.loads(run_process(
            prefix + ["--json", "snapshot", "list"]).stdout)
        ctx.equal(snapshot_list[0]["name"], "cli-stable", "CLI snapshot list")
        shown_snapshot = json.loads(run_process(
            prefix + ["snapshot", "show", "cli-stable"]).stdout)
        ctx.equal(shown_snapshot["commit"], snapshot["commit"], "CLI snapshot show")
        run_process(prefix + ["snapshot", "drop", "cli-stable"])
        run_process(prefix + ["rm", "/cli/a/newline.txt"])
        run_process(prefix + ["rm", "/cli/a/version.txt"])
        run_process(prefix + ["rm", "/cli/a/payload.bin"])
        run_process(prefix + ["rm", "/cli/a"])
        run_process(prefix + ["rm", "-r", "/cli/b"])
        index_after_remove = json.loads(run_process(prefix + ["index", "status"]).stdout)
        ctx.equal(index_after_remove["indexed_files"], 0,
                  "CLI 删除文件后同步回收文本索引")
        index_disabled = json.loads(run_process(prefix + ["index", "disable"]).stdout)
        ctx.equal(index_disabled["enabled"], False, "CLI 关闭文本索引")
        run_process(prefix + ["descriptor", str(descriptor)])
        descriptor_json = json.loads(descriptor.read_text())
        ctx.equal(descriptor_json["workspace"], "eval", "descriptor workspace")
        ctx.equal(Path(descriptor_json["database_path"]).resolve(), database.resolve(),
                  "descriptor database")
        doctor = run_process(prefix + ["--json", "doctor"], check=False)
        doctor_json = json.loads(doctor.stdout)
        expected_platform = {"Darwin": "macos", "Linux": "linux",
                             "Windows": "windows"}.get(platform.system(), "unsupported")
        expected_driver = {"Darwin": "FSKit", "Linux": "libfuse3",
                           "Windows": "WinFsp"}.get(platform.system(), "none")
        ctx.equal(doctor_json["platform"], expected_platform, "doctor platform")
        ctx.equal(doctor_json["mount_driver"], expected_driver, "doctor mount driver")
        ctx.check(isinstance(doctor_json["mount_ready"], bool), "doctor mount ready")
        ctx.equal(doctor_json["database"]["schema_version"], CONTRACT_VERSION,
                  "doctor schema")
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
        ctx.equal(details["schema_version"], "0.2.0", "doctor 报告真实 schema 版本")
        ctx.equal(details["schema_ready"], False, "doctor 拒绝把错误版本报为正常")
        connection = sqlite3.connect(str(database), isolation_level=None)
        ctx.equal(connection.execute(
            "SELECT value FROM _vexfs_meta WHERE key='contract_version'").fetchone()[0],
            "0.2.0", "doctor 结束后没有静默迁移")
        connection.close()
        setup = run_process(prefix + ["setup"], check=False)
        ctx.check(setup.returncode != 0, "错误 schema 版本不做自动升级")
        connection = sqlite3.connect(str(database), isolation_level=None)
        ctx.equal(connection.execute(
            "SELECT value FROM _vexfs_meta WHERE key='contract_version'").fetchone()[0],
                  "0.2.0", "setup 失败后仍不改写 schema")
        connection.close()
        if os.name != "nt":
            target = Path(directory) / "symlink-target"
            target.write_bytes(b"must-not-change")
            target.chmod(0o644)
            linked_database = Path(directory) / "linked.sqlite3"
            linked_database.symlink_to(target)
            rejected = run_process(
                [str(ctx.cli), "--db", str(linked_database), "setup"], check=False)
            ctx.check(rejected.returncode != 0, "数据库符号链接必须被拒绝")
            ctx.equal(target.read_bytes(), b"must-not-change", "符号链接目标内容不变")
            ctx.equal(target.stat().st_mode & 0o777, 0o644, "符号链接目标权限不变")
        return {"mode": "0600", "doctor_exit": doctor.returncode,
                "symlink_rejected": os.name != "nt"}


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
        started = time.perf_counter()
        snapshot_commit = db.scalar(
            "SELECT vexfs_snapshot_create('default','small-files')")
        snapshot_create_seconds = time.perf_counter() - started
        db.scalar("SELECT vexfs_write('default','/small/f000000.txt','changed')")
        started = time.perf_counter()
        snapshot_changes = db.json(
            "SELECT vexfs_snapshot_diff('default','small-files','HEAD')")["changes"]
        snapshot_diff_seconds = time.perf_counter() - started
        ctx.equal([entry["path"] for entry in snapshot_changes],
                  ["/small/f000000.txt"], "大量小文件 workspace diff")
        restore_head = db.scalar(
            "SELECT head_commit FROM _vexfs_workspaces WHERE name='default'")
        started = time.perf_counter()
        db.scalar(
            "SELECT vexfs_snapshot_restore('default','small-files',?)", (restore_head,))
        snapshot_restore_seconds = time.perf_counter() - started
        ctx.equal(db.scalar("SELECT vexfs_read('default','/small/f000000.txt')"), payload,
                  "大量小文件 workspace restore")
        ctx.budget("small_create_seconds", create_seconds, 60.0)
        ctx.budget("small_list_seconds", list_seconds, 5.0)
        ctx.budget("snapshot_create_seconds", snapshot_create_seconds, 1.0)
        ctx.budget("snapshot_diff_seconds", snapshot_diff_seconds, 15.0)
        ctx.budget("snapshot_restore_seconds", snapshot_restore_seconds, 30.0)
        return {"files": ctx.mode.small_files,
                "create_seconds": round(create_seconds, 6),
                "create_files_per_second": round(
                    ctx.mode.small_files / max(create_seconds, 1e-9), 3),
                "list_seconds": round(list_seconds, 6),
                "sample_stat_read_seconds": round(sample_seconds, 6),
                "snapshot_commit": snapshot_commit,
                "snapshot_create_seconds": round(snapshot_create_seconds, 6),
                "snapshot_diff_seconds": round(snapshot_diff_seconds, 6),
                "snapshot_restore_seconds": round(snapshot_restore_seconds, 6),
                "storage": db_storage(db.path)}


@case("performance.mount-contract-small-files", "performance",
      "挂载合同原子创建、macOS xattr、内容发布和同步写放大")
def performance_mount_contract_small_files(ctx: Context) -> dict[str, Any]:
    executable = ctx.build_dir / "vexfs_runtime_smoke"
    if not executable.exists():
        raise EvalSkip(f"缺少挂载合同 benchmark: {executable}")
    file_count = 250 if ctx.mode.name == "quick" else 1_000
    rss_before = child_max_rss_bytes()
    result = run_process([str(executable), "--benchmark", str(file_count)], timeout=60)
    metrics = json.loads(result.stdout)
    rss_after = child_max_rss_bytes()
    ctx.equal(metrics["files"], file_count, "挂载合同 benchmark 文件数")
    ctx.equal(metrics["commit_rows"], file_count, "每个新文件一个历史 commit")
    ctx.equal(metrics["version_rows"], file_count, "每个新文件一个内容版本")
    ctx.check(metrics["request_rows"] <= file_count * 2,
              "本地挂载每个文件最多保留 create/close 两条请求")
    ctx.check(metrics["durability_barriers"] >= 1, "synchronize 执行真实 FULL 屏障")
    ctx.check(rss_after < 1024 * MIB, "挂载合同 benchmark 峰值内存低于 1 GiB")
    ctx.budget("mount_contract_create_seconds", metrics["create_seconds"], 30.0)
    ctx.budget("mount_contract_sync_seconds", metrics["sync_seconds"], 5.0)
    metrics["child_max_rss_before_bytes"] = rss_before
    metrics["child_max_rss_after_bytes"] = rss_after
    return metrics


@case("performance.scale-tree", "performance",
      "1 千/1 万/10 万文件目录树的创建、重开、遍历、抽样读取和完整性")
def performance_scale_tree(ctx: Context) -> dict[str, Any]:
    file_count = {"quick": 1_000, "full": 10_000, "stress": 100_000}[ctx.mode.name]
    directory_count = min(1_000, max(10, file_count // 100))
    plain_payload = b"vexfs-scale-payload marker=plain\n"
    needle_payload = b"vexfs-scale-payload marker=needle\n"
    memory_budget = {"quick": 1 * 1024 * MIB, "full": 1536 * MIB,
                     "stress": 2 * 1024 * MIB}[ctx.mode.name]
    rss_before = self_max_rss_bytes()
    with tempfile.TemporaryDirectory(prefix="vexfs-scale-tree-") as directory:
        database_path = Path(directory) / "scale.sqlite3"
        db = Database(ctx, database_path)
        db.scalar("SELECT vexfs_mkdir('default','/scale')")
        for index in range(directory_count):
            db.scalar("SELECT vexfs_mkdir('default',?)", (f"/scale/d{index:04d}",))

        started = time.perf_counter()
        for index in range(file_count):
            bucket = index % directory_count
            payload = needle_payload if index % 1_000 == 0 else plain_payload
            db.scalar("SELECT vexfs_write('default',?,?)",
                      (f"/scale/d{bucket:04d}/f{index:06d}.txt", payload))
            if index % 1_000 == 0 and self_max_rss_bytes() > memory_budget:
                raise EvalFailure(
                    f"规模测试峰值内存超过保护线: {self_max_rss_bytes() / MIB:.1f} MiB")
        create_seconds = time.perf_counter() - started
        db.close()

        reopened = Database(ctx, database_path)
        # 大规模目录和 FTS 构建优先使用临时文件，并把 SQLite 页缓存限制在
        # 64 MiB，避免测试为了追求速度挤占整机内存。
        reopened.connection.execute("PRAGMA temp_store=FILE")
        reopened.connection.execute("PRAGMA cache_size=-65536")
        started = time.perf_counter()
        stored_files = reopened.scalar(
            "SELECT count(*) FROM _vexfs_dentries d "
            "JOIN _vexfs_inodes i ON i.id=d.inode_id "
            "JOIN _vexfs_workspaces w ON w.id=d.workspace_id "
            "WHERE w.name='default' AND i.kind='file' AND i.deleted_at IS NULL")
        count_seconds = time.perf_counter() - started
        ctx.equal(stored_files, file_count, "规模目录树文件数")

        sample_count = min(200, file_count)
        started = time.perf_counter()
        for sample in range(sample_count):
            index = sample * max(1, file_count // sample_count)
            if index >= file_count:
                index = file_count - 1
            bucket = index % directory_count
            path = f"/scale/d{bucket:04d}/f{index:06d}.txt"
            payload = needle_payload if index % 1_000 == 0 else plain_payload
            ctx.equal(reopened.scalar("SELECT vexfs_read('default',?)", (path,)),
                      payload, "规模目录树抽样读取")
        sample_seconds = time.perf_counter() - started
        started = time.perf_counter()
        grep = reopened.json(
            "SELECT vexfs_grep('default','/scale','needle',2,10240)")
        grep_seconds = time.perf_counter() - started
        expected_matches = (file_count + 999) // 1_000
        ctx.equal(grep["match_count"], expected_matches, "数据库批量 grep 命中数")
        ctx.equal(grep["files_scanned"], file_count, "数据库批量 grep 扫描文件数")
        ctx.equal(grep["binary_files_skipped"], 0, "数据库批量 grep 文本识别")
        ctx.equal(grep["index_used"], False, "数据库批量 grep 默认不增加索引写放大")
        started = time.perf_counter()
        index_status = reopened.json("SELECT vexfs_grep_index('enable')")
        index_build_seconds = time.perf_counter() - started
        ctx.equal(index_status["available"], True, "trigram 索引可用")
        ctx.equal(index_status["indexed_files"], file_count, "trigram 索引文件数")
        started = time.perf_counter()
        indexed_grep = reopened.json(
            "SELECT vexfs_grep('default','/scale','needle',2,10240)")
        indexed_grep_seconds = time.perf_counter() - started
        ctx.equal(indexed_grep["index_used"], True, "数据库 grep 使用 trigram 索引")
        ctx.equal(indexed_grep["match_count"], expected_matches, "trigram grep 命中数")
        ctx.equal(indexed_grep["files_scanned"], expected_matches,
                  "trigram grep 只读取候选文件")
        rss_after_index = self_max_rss_bytes()
        ctx.check(rss_after_index <= memory_budget,
                  f"规模测试峰值内存不超过 {memory_budget / MIB:.0f} MiB")
        ctx.equal(reopened.scalar("PRAGMA integrity_check"), "ok", "规模目录树完整性")
        storage = db_storage(database_path)
        reopened.close()

        create_budget = {"quick": 120.0, "full": 900.0, "stress": 7_200.0}[
            ctx.mode.name]
        ctx.budget("scale_tree_create_seconds", create_seconds, create_budget)
        ctx.budget("scale_tree_count_seconds", count_seconds, 60.0)
        ctx.budget("scale_tree_grep_seconds", grep_seconds,
                   {"quick": 5.0, "full": 30.0, "stress": 300.0}[ctx.mode.name])
        ctx.budget("scale_tree_index_build_seconds", index_build_seconds,
                   {"quick": 10.0, "full": 60.0, "stress": 600.0}[ctx.mode.name])
        ctx.budget("scale_tree_indexed_grep_seconds", indexed_grep_seconds,
                   {"quick": 1.0, "full": 5.0, "stress": 30.0}[ctx.mode.name])
        return {
            "files": file_count,
            "directories": directory_count,
            "create_seconds": round(create_seconds, 6),
            "create_files_per_second": round(file_count / max(create_seconds, 1e-9), 3),
            "count_seconds": round(count_seconds, 6),
            "sample_reads": sample_count,
            "sample_seconds": round(sample_seconds, 6),
            "database_grep_seconds": round(grep_seconds, 6),
            "database_grep_matches": grep["match_count"],
            "database_grep_files_per_second": round(
                file_count / max(grep_seconds, 1e-9), 3),
            "index_build_seconds": round(index_build_seconds, 6),
            "indexed_grep_seconds": round(indexed_grep_seconds, 6),
            "indexed_grep_matches": indexed_grep["match_count"],
            "indexed_grep_candidates": indexed_grep["files_scanned"],
            "max_rss_before_bytes": rss_before,
            "max_rss_after_index_bytes": rss_after_index,
            "memory_budget_bytes": memory_budget,
            "storage": storage,
        }


@case("stability.mixed-reopen-soak", "stability",
      "持续混合读写、元数据、快照、checkpoint 和数据库重开")
def stability_mixed_reopen_soak(ctx: Context) -> dict[str, Any]:
    default_seconds = {"quick": 5, "full": 60, "stress": 900}[ctx.mode.name]
    duration_seconds = max(1, int(os.environ.get(
        "VEXFS_EVAL_SOAK_SECONDS", str(default_seconds))))
    rng = random.Random(ctx.seed ^ 0x50A4)
    with tempfile.TemporaryDirectory(prefix="vexfs-soak-") as directory:
        database_path = Path(directory) / "soak.sqlite3"
        db = Database(ctx, database_path)
        db.scalar("SELECT vexfs_mkdir('default','/soak')")
        expected: dict[int, bytes] = {}
        for index in range(128):
            value = f"seed-{index}".encode()
            db.scalar("SELECT vexfs_write('default',?,?)",
                      (f"/soak/f{index:03d}.txt", value))
            expected[index] = value

        operations = 0
        reopen_count = 0
        checkpoint_count = 0
        snapshot_count = 0
        started = time.perf_counter()
        deadline = started + duration_seconds
        while time.perf_counter() < deadline:
            index = rng.randrange(128)
            path = f"/soak/f{index:03d}.txt"
            operation = operations % 6
            if operation in (0, 1):
                value = hashlib.sha256(f"{ctx.seed}:{operations}:{index}".encode()).digest()
                db.scalar("SELECT vexfs_write('default',?,?)", (path, value))
                expected[index] = value
            elif operation == 2:
                ctx.equal(db.scalar("SELECT vexfs_read('default',?)", (path,)),
                          expected[index], "soak 混合读取")
            elif operation == 3:
                inode = db.json("SELECT vexfs_stat('default',?)", (path,))["inode"]
                mode = 0o600 | (operations & 0o77)
                db.scalar("SELECT vexfs_set_mode('default',?,?)", (inode, mode))
            elif operation == 4:
                inode = db.json("SELECT vexfs_stat('default',?)", (path,))["inode"]
                db.scalar("SELECT vexfs_xattr_set('default',?,'user.soak',?,0)",
                          (inode, str(operations).encode()))
            else:
                temporary = f"/soak/.tmp-{index:03d}.txt"
                db.scalar("SELECT vexfs_rename('default',?,?,0)", (path, temporary))
                db.scalar("SELECT vexfs_rename('default',?,?,0)", (temporary, path))
            operations += 1

            if operations % 200 == 0:
                db.close()
                db = Database(ctx, database_path)
                reopen_count += 1
                sample = rng.randrange(128)
                ctx.equal(db.scalar("SELECT vexfs_read('default',?)",
                                    (f"/soak/f{sample:03d}.txt",)), expected[sample],
                          "soak 重开抽样")
            if operations % 2_000 == 0:
                name = f"soak-{snapshot_count:06d}"
                db.scalar("SELECT vexfs_snapshot_create('default',?)", (name,))
                ctx.equal(db.json(
                    "SELECT vexfs_snapshot_diff('default',?,'HEAD')", (name,))["changes"],
                    [], "soak 快照创建后无差异")
                db.scalar("SELECT vexfs_snapshot_drop('default',?)", (name,))
                snapshot_count += 1
            if operations % 5_000 == 0:
                db.connection.execute("PRAGMA wal_checkpoint(PASSIVE)").fetchone()
                ctx.equal(db.scalar("PRAGMA integrity_check"), "ok", "soak 中途完整性")
                checkpoint_count += 1

        elapsed = time.perf_counter() - started
        for index in range(128):
            ctx.equal(db.scalar("SELECT vexfs_read('default',?)",
                                (f"/soak/f{index:03d}.txt",)), expected[index],
                      "soak 最终内容")
        ctx.equal(db.scalar("PRAGMA integrity_check"), "ok", "soak 最终完整性")
        pending = db.scalar("SELECT count(*) FROM _vexfs_staging")
        ctx.equal(pending, 0, "soak 没有未发布 staging")
        storage = db_storage(database_path)
        db.close()
        ctx.check(elapsed >= duration_seconds * 0.95, "soak 达到目标时长")
        return {
            "target_seconds": duration_seconds,
            "elapsed_seconds": round(elapsed, 6),
            "operations": operations,
            "operations_per_second": round(operations / max(elapsed, 1e-9), 3),
            "reopens": reopen_count,
            "snapshots": snapshot_count,
            "checkpoints": checkpoint_count,
            "pending_staging": pending,
            "storage": storage,
        }


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


@case("performance.metadata-hardlinks", "performance",
      "owner、ACL 和硬链接批量操作性能")
def performance_metadata_hardlinks(ctx: Context) -> dict[str, Any]:
    file_count = {"quick": 100, "full": 500, "stress": 1_000}[ctx.mode.name]
    with Database(ctx) as db:
        db.scalar("SELECT vexfs_mkdir('default','/metadata')")
        db.scalar("SELECT vexfs_mkdir('default','/metadata/links')")
        for index in range(file_count):
            db.scalar("SELECT vexfs_write('default',?,X'7061796C6F6164')",
                      (f"/metadata/f{index:05d}.txt",))

        started = time.perf_counter()
        for index in range(file_count):
            inode = db.json(
                "SELECT vexfs_stat('default',?)",
                (f"/metadata/f{index:05d}.txt",))["inode"]
            db.scalar("SELECT vexfs_chown('default',?,?,?)", (inode, 1000 + index, 2000))
        chown_seconds = time.perf_counter() - started

        started = time.perf_counter()
        for index in range(file_count):
            inode = db.json(
                "SELECT vexfs_stat('default',?)",
                (f"/metadata/f{index:05d}.txt",))["inode"]
            db.scalar("SELECT vexfs_acl_grant('default',?,?,?)",
                      (inode, "agent", "read,write"))
        acl_seconds = time.perf_counter() - started

        started = time.perf_counter()
        for index in range(file_count):
            db.scalar("SELECT vexfs_link('default',?,?)",
                      (f"/metadata/f{index:05d}.txt",
                       f"/metadata/links/f{index:05d}.txt"))
        hardlink_seconds = time.perf_counter() - started

        ctx.equal(db.scalar(
            "SELECT json_array_length(vexfs_list('default','/metadata/links'))"),
            file_count,
                  "批量硬链接数量")
        sample_path = "/metadata/f00000.txt"
        sample = db.json("SELECT vexfs_stat('default',?)", (sample_path,))
        ctx.equal((sample["link_count"], sample["uid"], sample["gid"]),
                  (2, 1000, 2000), "批量 metadata 结果")
        acl = db.json("SELECT vexfs_acl_get('default',?)", (sample["inode"],))
        ctx.equal(acl[0]["principal"], "agent", "批量 ACL 结果")

        started = time.perf_counter()
        snapshot_commit = db.scalar(
            "SELECT vexfs_snapshot_create('default','metadata-hardlinks')")
        snapshot_seconds = time.perf_counter() - started

        ctx.budget("metadata_chown_seconds", chown_seconds, 60.0)
        ctx.budget("metadata_acl_seconds", acl_seconds, 60.0)
        ctx.budget("metadata_hardlink_seconds", hardlink_seconds, 60.0)
        ctx.budget("metadata_snapshot_seconds", snapshot_seconds, 30.0)
        return {"files": file_count, "snapshot_commit": snapshot_commit,
                "chown_seconds": round(chown_seconds, 6),
                "acl_seconds": round(acl_seconds, 6),
                "hardlink_seconds": round(hardlink_seconds, 6),
                "snapshot_seconds": round(snapshot_seconds, 6),
                "chown_ops_per_second": round(file_count / max(chown_seconds, 1e-9), 3),
                "acl_ops_per_second": round(file_count / max(acl_seconds, 1e-9), 3),
                "hardlink_ops_per_second": round(file_count / max(hardlink_seconds, 1e-9), 3)}


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
        inode = db.json("SELECT vexfs_stat('default','/random.bin')")["inode"]
        base_manifest = db.scalar(
            "SELECT manifest_id FROM _vexfs_file_versions "
            "WHERE inode_id=? AND version_no=1", (inode,))
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
        publish_started = time.perf_counter()
        ctx.equal(db.scalar(
            "SELECT vexfs_handle_publish(?,?,'data','random-publish')",
            (handle, generation)), 2, "随机 patch 发布新版本")
        publish_seconds = time.perf_counter() - publish_started
        db.scalar("SELECT vexfs_handle_close(?,0,'random-close')", (handle,))
        patched_manifest = db.scalar(
            "SELECT manifest_id FROM _vexfs_file_versions "
            "WHERE inode_id=? AND version_no=2", (inode,))
        affected_chunks = {offset // (64 * 1024) for offset in expected}
        total_chunks = logical_size // (64 * 1024)
        reused_chunks = db.scalar(
            "SELECT count(*) FROM _vexfs_manifest_chunks before "
            "JOIN _vexfs_manifest_chunks after "
            "ON after.chunk_no=before.chunk_no AND after.chunk_id=before.chunk_id "
            "WHERE before.manifest_id=? AND after.manifest_id=?",
            (base_manifest, patched_manifest))
        ctx.equal(reused_chunks, total_chunks - len(affected_chunks),
                  "随机发布只替换受影响的 64 KiB 块")
        physical_chunks = db.scalar(
            "SELECT count(*) FROM _vexfs_chunks WHERE inode_id=?", (inode,))
        ctx.check(physical_chunks <= 1 + len(affected_chunks),
                  "重复零块和未变化块不会重复保存")
        for offset, data in list(expected.items())[-min(len(expected), 3):]:
            ctx.equal(db.scalar(
                "SELECT substr(vexfs_read('default','/random.bin'),?,?)",
                (offset + 1, patch_size)), data, "发布后随机 patch 内容")
        ctx.budget("random_patch_seconds", seconds, 120.0)
        ctx.budget("random_patch_publish_seconds", publish_seconds, 60.0)
        storage_before_checkpoint = db_storage(db.path)
        db.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        storage_after_checkpoint = db_storage(db.path)
        return {"writes": ctx.mode.random_writes, "unique_slots": len(expected),
                "logical_bytes": logical_size, "capacity_bytes": row[2],
                "generation": generation, "seconds": round(seconds, 6),
                "publish_seconds": round(publish_seconds, 6),
                "operations_per_second": round(ctx.mode.random_writes / max(seconds, 1e-9), 3),
                "affected_chunks": len(affected_chunks), "reused_chunks": reused_chunks,
                "physical_chunks": physical_chunks,
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


@case("performance.integrity-check", "performance",
      "完整性检查按 64 KiB 流式扫描，记录快速检查与 SHA-256 吞吐和内存上界")
def performance_integrity_check(ctx: Context) -> dict[str, Any]:
    file_count = {"quick": 4, "full": 32, "stress": 64}[ctx.mode.name]
    payload = bytes((index * 31 + 7) % 251 for index in range(MIB))
    logical_bytes = file_count * len(payload)
    with Database(ctx) as db:
        for index in range(file_count):
            db.scalar("SELECT vexfs_write('default',?,?)",
                      (f"/check-{index:04d}.bin", payload))
        before_rss = self_max_rss_bytes()
        quick_started = time.perf_counter()
        quick = db.json("SELECT vexfs_check('default',0)")
        quick_seconds = time.perf_counter() - quick_started
        deep_started = time.perf_counter()
        deep = db.json("SELECT vexfs_check('default',1)")
        deep_seconds = time.perf_counter() - deep_started
        after_rss = self_max_rss_bytes()
        ctx.check(quick["ok"] and deep["ok"], "性能数据集检查通过")
        ctx.equal(deep["checked"]["content_bytes"], logical_bytes,
                  "深度检查扫描所有 canonical manifest/chunk")
        ctx.equal(deep["checked"]["versions"], file_count, "版本计数")
        ctx.budget("integrity_check_seconds", deep_seconds,
                   {"quick": 10.0, "full": 45.0, "stress": 90.0}[ctx.mode.name])
        # ru_maxrss 是进程历史峰值；这里只约束检查阶段没有出现灾难性额外增长。
        rss_growth = max(0, after_rss - before_rss)
        ctx.check(rss_growth <= 64 * MIB,
                  f"流式检查阶段 RSS 峰值增长过大: {rss_growth} bytes")
        return {"files": file_count,
                "logical_bytes": logical_bytes,
                "quick_seconds": round(quick_seconds, 6),
                "deep_seconds": round(deep_seconds, 6),
                "deep_mib_per_second": round(
                    logical_bytes / MIB / max(deep_seconds, 1e-9), 3),
                "reported_elapsed_ms": deep["elapsed_ms"],
                "rss_growth_bytes": rss_growth}


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
                "SELECT sum(chunk.size) FROM _vexfs_file_versions version "
                "JOIN _vexfs_manifest_chunks entry ON entry.manifest_id=version.manifest_id "
                "JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id "
                "WHERE version.inode_id=(SELECT inode_id FROM _vexfs_dentries "
                "WHERE name='max.bin')"),
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
    build_env = dict(os.environ)
    build_env["CCACHE_DIR"] = str(ctx.output_dir / "ccache")
    started = time.perf_counter()
    app: Path | None = None
    try:
        result = run_process(
            ["xcodebuild", "-project", str(project), "-scheme", "VexFSApp",
             "-configuration", "Debug", "-derivedDataPath", str(derived),
             "ARCHS=arm64", "ONLY_ACTIVE_ARCH=YES", "CODE_SIGNING_ALLOWED=NO", "build"],
            timeout=300, cwd=ctx.root, env=build_env)
        seconds = time.perf_counter() - started
        output = result.stdout + result.stderr
        ctx.check(b"BUILD SUCCEEDED" in output, "Xcode 没有 BUILD SUCCEEDED")
        app = next(derived.glob("Build/Products/Debug/VexDB Lite.app"), None)
        ctx.check(app is not None, "缺少 VexDB Lite.app")
        return {"seconds": round(seconds, 6), "app": str(app)}
    finally:
        # xcodebuild 会把测试产物登记到 LaunchServices。若不撤销登记，未签名的
        # Debug extension 会和 /Applications 中的正式开发签名版本争用同一标识。
        if app is None:
            app = next(derived.glob("Build/Products/Debug/VexDB Lite.app"), None)
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
        installed_apps = [
            Path.home() / "Applications/VexDB Lite.app",
            Path("/Applications/VexDB Lite.app"),
            Path("/Applications/VexFS.app"),
        ]
        installed_app = next((candidate for candidate in installed_apps
                              if candidate.exists()), installed_apps[0])
        installed_extension = installed_app / "Contents/Extensions/VexFSAppEx.appex"
        if (lsregister.exists() and installed_extension.exists()):
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


def mount_cli(ctx: Context) -> Path:
    """FSKit 只向受信任调用方公开扩展；发行 Gate 必须显式绑定被测 CLI。"""
    if ctx.mount_cli_override is not None:
        return ctx.mount_cli_override
    installed = Path.home() / ".local/bin/vexfs"
    return installed if installed.exists() else ctx.cli


def filesystem_cli_prefix(cli: Path, database: Path, workspace: str) -> list[str]:
    """同时支持独立 vexfs 名称和统一 vexdb fs 入口。"""
    name = cli.stem.lower() if cli.suffix.lower() == ".exe" else cli.name.lower()
    command = [str(cli)] if name == "vexfs" else [str(cli), "fs"]
    return command + ["--db", str(database), "--workspace", workspace]


MOUNT_CONFORMANCE_VERSION = 1
MOUNT_CONFORMANCE_CAPABILITIES = (
    "create-exclusive",
    "range-io",
    "fsync",
    "append",
    "truncate",
    "rename-replace",
    "chmod-exec",
    "hardlink",
    "symlink",
    "xattr",
    "unicode-name",
    "stable-errno",
    "snapshot-restore",
    "remount",
)


@dataclass(frozen=True)
class RealMountAdapter:
    name: str
    cli: Path
    database: Path
    mount_point: Path
    prefix: list[str]
    doctor_before: dict[str, Any]


def prepare_real_mount_adapter(ctx: Context, base: Path,
                               workspace: str,
                               database: Path | None = None) -> RealMountAdapter:
    """为当前系统准备真实 mount；测试主体不包含平台分支。"""
    system = platform.system()
    if system == "Darwin":
        name = "fskit"
        cli = mount_cli(ctx)
    elif system == "Linux":
        name = "libfuse3"
        cli = ctx.cli
        helper = ctx.build_dir / "vexfs-fuse"
        if not helper.exists():
            raise EvalSkip("当前构建没有 vexfs-fuse helper")
        if not Path("/dev/fuse").exists() or not os.access(
                "/dev/fuse", os.R_OK | os.W_OK):
            raise EvalSkip("当前环境没有可读写的 /dev/fuse")
        if shutil.which("fusermount3") is None:
            raise EvalSkip("当前环境没有 fusermount3")
    else:
        raise EvalSkip("共享 mount 一致性 eval 当前支持 macOS 和 Linux")

    database = database or base / "mount.sqlite3"
    mount_point = base / "mnt"
    prefix = filesystem_cli_prefix(cli, database, workspace)
    run_process(prefix + ["setup"])
    doctor = run_process(prefix + ["--json", "doctor"], check=False)
    try:
        details = json.loads(doctor.stdout)
    except json.JSONDecodeError as error:
        raise EvalFailure(
            f"{name} doctor 没有返回 JSON: {doctor.stdout.decode(errors='replace')}") from error
    if system == "Darwin" and details.get("extension") != "enabled":
        raise EvalSkip(
            f"FSKit extension={details.get('extension', 'unknown')}，需安装并在系统设置中启用")
    if system == "Darwin":
        module_path = details.get("extension_path", "")
        installed_extensions = {
            str(path.resolve())
            for path in (
                Path.home() / "Applications/VexDB Lite.app/Contents/Extensions/VexFSAppEx.appex",
                Path("/Applications/VexDB Lite.app/Contents/Extensions/VexFSAppEx.appex"),
            )
            if path.exists()
        }
        ctx.check(bool(module_path), "doctor 必须返回 FSKit 真实 module URL")
        ctx.check(str(Path(module_path).resolve()) in installed_extensions,
                  "FSKit module URL 必须指向正式安装 App，不能指向 archive/DerivedData")
    if system == "Linux" and not details.get("mount_ready"):
        raise EvalSkip(f"libfuse3 adapter={details.get('extension', 'unknown')}")
    mount_point.mkdir()
    return RealMountAdapter(name, cli, database, mount_point, prefix, details)


def expect_mount_errno(ctx: Context, operation: Callable[[], Any], expected: int,
                       message: str) -> None:
    ctx.checks += 1
    try:
        operation()
    except OSError as error:
        if error.errno != expected:
            raise EvalFailure(
                f"{message}: expected errno={expected}, actual={error.errno} ({error})")
        return
    raise EvalFailure(f"{message}: 预期 errno={expected} 但操作成功")


def system_setxattr(path: Path, name: str, value: bytes) -> None:
    if hasattr(os, "setxattr"):
        os.setxattr(path, name, value)
        return
    libc = ctypes.CDLL(None, use_errno=True)
    function = libc.setxattr
    if platform.system() == "Darwin":
        function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                             ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
        arguments = (os.fsencode(path), os.fsencode(name),
                     ctypes.create_string_buffer(value), len(value), 0, 0)
    else:
        function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                             ctypes.c_size_t, ctypes.c_int]
        arguments = (os.fsencode(path), os.fsencode(name),
                     ctypes.create_string_buffer(value), len(value), 0)
    function.restype = ctypes.c_int
    if function(*arguments) != 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), os.fspath(path))


def system_getxattr(path: Path, name: str) -> bytes:
    if hasattr(os, "getxattr"):
        return os.getxattr(path, name)
    libc = ctypes.CDLL(None, use_errno=True)
    function = libc.getxattr
    common = (os.fsencode(path), os.fsencode(name))
    if platform.system() == "Darwin":
        function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                             ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
        suffix = (0, 0)
    else:
        function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                             ctypes.c_size_t]
        suffix = ()
    function.restype = ctypes.c_ssize_t
    size = function(*common, None, 0, *suffix)
    if size < 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), os.fspath(path))
    buffer = ctypes.create_string_buffer(max(size, 1))
    result = function(*common, buffer, size, *suffix)
    if result < 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), os.fspath(path))
    return buffer.raw[:result]


def system_listxattr(path: Path) -> list[str]:
    if hasattr(os, "listxattr"):
        return os.listxattr(path)
    libc = ctypes.CDLL(None, use_errno=True)
    function = libc.listxattr
    if platform.system() == "Darwin":
        function.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                             ctypes.c_int]
        suffix = (0,)
    else:
        function.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
        suffix = ()
    function.restype = ctypes.c_ssize_t
    encoded_path = os.fsencode(path)
    size = function(encoded_path, None, 0, *suffix)
    if size < 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), os.fspath(path))
    buffer = ctypes.create_string_buffer(max(size, 1))
    result = function(encoded_path, buffer, size, *suffix)
    if result < 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), os.fspath(path))
    return [os.fsdecode(item) for item in buffer.raw[:result].split(b"\0") if item]


def mount_adapter_mount(ctx: Context, adapter: RealMountAdapter) -> list[dict[str, Any]]:
    run_process(adapter.prefix + ["mount", str(adapter.mount_point)], timeout=60)
    status = run_process(
        adapter.prefix + ["--json", "mount", "status", str(adapter.mount_point)])
    mounts = json.loads(status.stdout)
    ctx.equal(len(mounts), 1, f"{adapter.name} 挂载状态数量")
    ctx.equal(mounts[0]["target"], os.path.realpath(adapter.mount_point),
              f"{adapter.name} 挂载目标")
    return mounts


def mount_adapter_unmount(ctx: Context, adapter: RealMountAdapter,
                          label: str) -> None:
    result = run_process(adapter.prefix + ["unmount", str(adapter.mount_point)],
                         check=False, timeout=60)
    ctx.equal(result.returncode, 0, f"{adapter.name} {label}卸载退出码")
    if adapter.name == "fskit":
        # FSKit 在 umount 返回后仍会异步释放 volume identity；重挂载前给系统收尾时间。
        time.sleep(2.0)


def run_shared_mount_operations(ctx: Context, adapter: RealMountAdapter) -> dict[str, Any]:
    """只使用跨平台系统调用；macOS/Linux 必须执行完全相同的断言。"""
    root = adapter.mount_point / "conformance"
    left = root / "left"
    right = root / "right"
    left.mkdir(parents=True)
    right.mkdir()

    data = left / "data.txt"
    descriptor = os.open(data, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        ctx.equal(os.write(descriptor, b"alpha\nbeta\n"), 11, "共享合同顺序写")
        ctx.equal(os.pwrite(descriptor, b"AGENT", 6), 5, "共享合同 range write")
        os.fsync(descriptor)
        ctx.equal(os.pread(descriptor, 11, 0), b"alpha\nAGENT", "共享合同 range read")
    finally:
        os.close(descriptor)
    data.chmod(0o640)
    expect_mount_errno(
        ctx, lambda: os.close(os.open(data, os.O_CREAT | os.O_EXCL | os.O_WRONLY)),
        errno.EEXIST, "O_EXCL 已存在文件")

    descriptor = os.open(data, os.O_WRONLY | os.O_APPEND)
    try:
        ctx.equal(os.write(descriptor, b"\nagent\n"), 7, "共享合同 append")
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    ctx.equal(data.read_bytes(), b"alpha\nAGENT\nagent\n", "共享合同 close-to-open")

    copied = left / "copied.txt"
    shutil.copyfile(data, copied)
    moved = right / "moved.txt"
    copied.rename(moved)
    replacement = left / "replacement.txt"
    replacement.write_bytes(b"replacement")
    os.replace(replacement, moved)
    ctx.equal(moved.read_bytes(), b"replacement", "共享合同 rename replace")
    os.truncate(moved, 4)
    ctx.equal(moved.read_bytes(), b"repl", "共享合同 truncate")

    hardlink = right / "data-hard.txt"
    os.link(data, hardlink)
    data_stat = data.stat()
    link_stat = hardlink.stat()
    ctx.equal(data_stat.st_ino, link_stat.st_ino, "共享合同 hardlink inode")
    ctx.equal(data_stat.st_nlink, 2, "共享合同 hardlink link count")
    with hardlink.open("ab") as stream:
        stream.write(b"hard\n")
        stream.flush()
        os.fsync(stream.fileno())
    ctx.check(data.read_bytes().endswith(b"hard\n"), "共享合同 hardlink 共享内容")

    symlink = right / "data-link.txt"
    symlink.symlink_to("../left/data.txt")
    dangling = right / "dangling.txt"
    dangling.symlink_to("missing.txt")
    ctx.equal(os.readlink(symlink), "../left/data.txt", "共享合同 symlink target")
    ctx.equal(symlink.read_bytes(), data.read_bytes(), "共享合同 symlink 读取")
    ctx.check(dangling.is_symlink() and not dangling.exists(), "共享合同 dangling symlink")

    unicode_file = left / "你好-agent-🚀.txt"
    unicode_file.write_text("跨平台\n", encoding="utf-8")
    ctx.equal(unicode_file.read_text(encoding="utf-8"), "跨平台\n", "共享合同 Unicode")

    xattr_name = "user.vexfs.conformance"
    xattr_value = b"stored-in-database\x00binary"
    system_setxattr(data, xattr_name, xattr_value)
    ctx.equal(system_getxattr(data, xattr_name), xattr_value, "共享合同 xattr 读取")
    ctx.check(xattr_name in system_listxattr(data), "共享合同 xattr 列表")

    script = root / "run.sh"
    script.write_text("#!/bin/sh\nprintf 'vexfs-conformance\\n'\n", encoding="utf-8")
    script.chmod(0o755)
    ctx.equal(script.stat().st_mode & 0o777, 0o755, "共享合同 executable mode")
    ctx.equal(run_process([str(script)], cwd=root).stdout, b"vexfs-conformance\n",
              "共享合同直接执行")

    ctx.equal(data.stat().st_uid, os.getuid(), "共享合同 uid")
    ctx.equal(data.stat().st_gid, os.getgid(), "共享合同 gid")
    ctx.check(os.statvfs(adapter.mount_point).f_bsize > 0, "共享合同 statvfs")
    expect_mount_errno(ctx, lambda: (root / "missing.txt").read_bytes(),
                       errno.ENOENT, "不存在路径")
    expect_mount_errno(ctx, lambda: left.mkdir(), errno.EEXIST, "重复目录")
    expect_mount_errno(ctx, lambda: left.rmdir(), errno.ENOTEMPTY, "非空目录删除")

    return {
        "data": data.read_bytes(),
        "mode": data.stat().st_mode & 0o777,
        "hardlink_inode_equal": data.stat().st_ino == hardlink.stat().st_ino,
        "hardlink_count": data.stat().st_nlink,
        "symlink_target": os.readlink(symlink),
        "xattr_name": xattr_name,
        "xattr_value": system_getxattr(data, xattr_name),
        "unicode": unicode_file.read_text(encoding="utf-8"),
        "script_output": run_process([str(script)], cwd=root).stdout,
        "entries": sorted(path.name for path in root.iterdir()),
    }


def verify_shared_mount_after_restore(ctx: Context, adapter: RealMountAdapter,
                                      expected: dict[str, Any]) -> dict[str, Any]:
    root = adapter.mount_point / "conformance"
    data = root / "left/data.txt"
    hardlink = root / "right/data-hard.txt"
    symlink = root / "right/data-link.txt"
    unicode_file = root / "left/你好-agent-🚀.txt"
    script = root / "run.sh"
    ctx.equal(data.read_bytes(), expected["data"], "恢复后内容")
    ctx.equal(data.stat().st_mode & 0o777, expected["mode"], "恢复后 mode")
    ctx.equal(data.stat().st_ino, hardlink.stat().st_ino, "恢复后 hardlink inode")
    ctx.equal(data.stat().st_nlink, expected["hardlink_count"], "恢复后 hardlink count")
    ctx.equal(os.readlink(symlink), expected["symlink_target"], "恢复后 symlink")
    ctx.equal(system_getxattr(data, expected["xattr_name"]), expected["xattr_value"],
              "恢复后 xattr")
    ctx.equal(unicode_file.read_text(encoding="utf-8"), expected["unicode"],
              "恢复后 Unicode")
    ctx.equal(run_process([str(script)], cwd=root).stdout, expected["script_output"],
              "恢复后可执行文件")
    ctx.equal(sorted(path.name for path in root.iterdir()), expected["entries"],
              "恢复后目录树")
    return {
        "bytes": len(expected["data"]),
        "mode": expected["mode"],
        "hardlink_count": data.stat().st_nlink,
        "symlink_target": os.readlink(symlink),
        "xattrs": system_listxattr(data),
        "unicode_name": unicode_file.name,
    }


@case("mount.cross-platform-conformance", "mount",
      "macOS FSKit 与 Linux FUSE 执行同一份挂载、元数据、快照和重挂载合同")
def cross_platform_mount_conformance(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-conformance-") as directory:
        base = Path(directory)
        adapter = prepare_real_mount_adapter(ctx, base, "conformance")
        mounts = mount_adapter_mount(ctx, adapter)
        expected: dict[str, Any]
        try:
            expected = run_shared_mount_operations(ctx, adapter)
            reverse = run_process(adapter.prefix + ["cat", "/conformance/left/data.txt"])
            ctx.equal(reverse.stdout, expected["data"], "挂载写入可由数据库 CLI 读取")
        finally:
            mount_adapter_unmount(ctx, adapter, "首次")

        snapshot = run_process(adapter.prefix + ["snapshot", "create", "baseline"])
        snapshot_commit = int(snapshot.stdout.strip())
        ctx.check(snapshot_commit > 0, "共享合同 snapshot commit")
        run_process(adapter.prefix + ["write", "/conformance/left/data.txt"],
                    input_data=b"mutated-after-snapshot")
        run_process(adapter.prefix + ["rm", "/conformance/left/你好-agent-🚀.txt"])
        diff = run_process(adapter.prefix + ["snapshot", "diff", "baseline"], check=False)
        ctx.equal(diff.returncode, 1, "共享合同 snapshot diff 有变化")
        changes = json.loads(diff.stdout)["changes"]
        changed_paths = {change["path"] for change in changes}
        ctx.check("/conformance/left/data.txt" in changed_paths, "snapshot diff 内容变化")
        ctx.check("/conformance/left/你好-agent-🚀.txt" in changed_paths,
                  "snapshot diff Unicode 删除")
        mount_adapter_mount(ctx, adapter)
        try:
            ctx.equal((adapter.mount_point / "conformance/left/data.txt").read_bytes(),
                      b"mutated-after-snapshot", "恢复前挂载视图包含修改内容")
            restore = json.loads(run_process(
                adapter.prefix + ["--json", "snapshot", "restore", "baseline"],
                timeout=120).stdout)
            ctx.equal(restore["remounted"], True, "挂载状态恢复自动重挂载")
            ctx.equal(restore["mount_point"], os.path.realpath(adapter.mount_point),
                      "快照恢复保持原挂载点")
            status = json.loads(run_process(
                adapter.prefix + ["--json", "mount", "status",
                                  str(adapter.mount_point)]).stdout)
            ctx.equal(len(status), 1, "快照恢复后挂载仍然存在")
            ctx.equal(status[0]["database"], os.path.realpath(adapter.database),
                      "挂载身份记录数据库")
            ctx.equal(status[0]["workspace"], "conformance", "挂载身份记录 workspace")
            clean_diff = run_process(adapter.prefix + ["snapshot", "diff", "baseline"])
            ctx.equal(json.loads(clean_diff.stdout)["changes"], [],
                      "snapshot restore 后无差异")
            restored = verify_shared_mount_after_restore(ctx, adapter, expected)
        finally:
            mount_adapter_unmount(ctx, adapter, "第二次")

        after = json.loads(run_process(adapter.prefix + ["--json", "doctor"]).stdout)
        ctx.equal(after["mount_count"], 0, "共享合同卸载后无挂载")
        ctx.equal(after["database"]["pending_handles"], 0, "共享合同无未发布句柄")
        ctx.equal(after["database"]["retained_handles"], 0, "共享合同无保留句柄")
        ctx.equal(after["database"]["staging_bytes"], 0, "共享合同无暂存字节")
        return {
            "contract_version": MOUNT_CONFORMANCE_VERSION,
            "adapter": adapter.name,
            "capabilities": list(MOUNT_CONFORMANCE_CAPABILITIES),
            "doctor_before": adapter.doctor_before,
            "doctor_after": after,
            "mounts": mounts,
            "snapshot_commit": snapshot_commit,
            "restored": restored,
        }


PORTABILITY_WORKSPACE = "cross-os"
PORTABILITY_ACL = [
    {"principal": "agent-portable", "effect": "allow",
     "permissions": "read,write", "inherit": 1},
]
PORTABILITY_XATTR = "user.vexfs.portable"
PORTABILITY_XATTR_VALUE = b"mac-linux-mac\x00metadata"


def verify_portable_tree(ctx: Context, adapter: RealMountAdapter,
                         plan_content: bytes, linux_file: bool) -> None:
    root = adapter.mount_point / "portable"
    unicode_dir = root / "你好-🚀"
    plan = unicode_dir / "plan.txt"
    hardlink = root / "plan-hard.txt"
    symlink = unicode_dir / "plan-link.txt"
    script = root / "build.sh"
    ctx.equal(plan.read_bytes(), plan_content, "跨系统 plan 内容")
    ctx.equal(plan.stat().st_ino, hardlink.stat().st_ino, "跨系统 hardlink inode")
    ctx.equal(plan.stat().st_nlink, 2, "跨系统 hardlink 数量")
    ctx.equal(os.readlink(symlink), "plan.txt", "跨系统 symlink target")
    ctx.equal(symlink.read_bytes(), plan_content, "跨系统 symlink 读取")
    ctx.equal(plan.stat().st_mode & 0o777, 0o640, "跨系统文件 mode")
    visible_owner = ((os.getuid(), os.getgid()) if adapter.name == "fskit"
                     else (4242, 4343))
    ctx.equal((plan.stat().st_uid, plan.stat().st_gid), visible_owner,
              "跨系统 owner 映射")
    ctx.equal(system_getxattr(plan, PORTABILITY_XATTR), PORTABILITY_XATTR_VALUE,
              "跨系统 xattr")
    ctx.equal(script.stat().st_mode & 0o777, 0o755, "跨系统可执行 mode")
    ctx.equal(run_process([str(script)], cwd=root).stdout, b"portable-script\n",
              "跨系统执行脚本")
    ctx.equal((root / "from-linux.txt").exists(), linux_file,
              "Linux 新文件可见性")
    if linux_file:
        ctx.equal((root / "from-linux.txt").read_text(encoding="utf-8"),
                  "created on linux\n", "Linux 新文件内容")


def verify_portable_acl(ctx: Context, adapter: RealMountAdapter) -> None:
    acl = json.loads(run_process(
        adapter.prefix + ["getfacl", "/portable/你好-🚀/plan.txt"]).stdout)
    ctx.equal(acl, PORTABILITY_ACL, "跨系统便携 ACL")


def verify_portable_database_owner(ctx: Context, adapter: RealMountAdapter) -> None:
    record = json.loads(run_process(
        adapter.prefix + ["stat", "/portable/你好-🚀/plan.txt"]).stdout)
    ctx.equal((record["uid"], record["gid"]), (4242, 4343),
              "数据库原样保存数字 owner")


@case("portability.cross-os-roundtrip", "portability",
      "同一 SQLite 工作区按 macOS 创建、Linux 修改、macOS 回读三个阶段往返")
def cross_os_portability_roundtrip(ctx: Context) -> dict[str, Any]:
    phase = os.environ.get("VEXFS_PORTABILITY_PHASE", "")
    database_value = os.environ.get("VEXFS_PORTABILITY_DB", "")
    if not phase or not database_value:
        raise EvalSkip(
            "需通过 run_cross_platform_portability.sh 设置跨系统往返阶段和共享数据库")
    if phase not in {"mac-create", "linux-roundtrip", "mac-verify"}:
        raise EvalFailure(f"未知跨系统往返阶段：{phase}")
    expected_system = "Linux" if phase == "linux-roundtrip" else "Darwin"
    if platform.system() != expected_system:
        raise EvalFailure(
            f"阶段 {phase} 必须在 {expected_system} 执行，当前为 {platform.system()}")

    database = Path(database_value).resolve()
    database.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"vexfs-portability-{phase}-") as directory:
        adapter = prepare_real_mount_adapter(
            ctx, Path(directory), PORTABILITY_WORKSPACE, database=database)

        if phase == "mac-create":
            mount_adapter_mount(ctx, adapter)
            try:
                root = adapter.mount_point / "portable"
                unicode_dir = root / "你好-🚀"
                unicode_dir.mkdir(parents=True)
                plan = unicode_dir / "plan.txt"
                plan.write_bytes(b"mac-v1\n")
                plan.chmod(0o640)
                os.link(plan, root / "plan-hard.txt")
                (unicode_dir / "plan-link.txt").symlink_to("plan.txt")
                system_setxattr(plan, PORTABILITY_XATTR, PORTABILITY_XATTR_VALUE)
                script = root / "build.sh"
                script.write_text(
                    "#!/bin/sh\nprintf 'portable-script\\n'\n", encoding="utf-8")
                script.chmod(0o755)
            finally:
                mount_adapter_unmount(ctx, adapter, "macOS 创建")

            run_process(adapter.prefix + [
                "chown", "4242:4343", "/portable/你好-🚀/plan.txt"])
            run_process(adapter.prefix + ["setfacl", "/portable/你好-🚀/plan.txt"],
                        input_data=json.dumps(PORTABILITY_ACL).encode())
            verify_portable_acl(ctx, adapter)
            verify_portable_database_owner(ctx, adapter)
            snapshot = int(run_process(
                adapter.prefix + ["snapshot", "create", "mac-baseline"]
            ).stdout.strip())
            ctx.check(snapshot > 0, "macOS baseline snapshot")
            return {"phase": phase, "adapter": adapter.name,
                    "database": str(database), "snapshot": snapshot}

        if phase == "linux-roundtrip":
            mount_adapter_mount(ctx, adapter)
            try:
                verify_portable_tree(ctx, adapter, b"mac-v1\n", linux_file=False)
                plan = adapter.mount_point / "portable/你好-🚀/plan.txt"
                plan.write_bytes(b"temporary-linux-change\n")
                (adapter.mount_point / "portable/你好-🚀/plan-link.txt").unlink()
            finally:
                mount_adapter_unmount(ctx, adapter, "Linux 首次")
            verify_portable_acl(ctx, adapter)
            verify_portable_database_owner(ctx, adapter)
            diff = run_process(
                adapter.prefix + ["snapshot", "diff", "mac-baseline"], check=False)
            ctx.equal(diff.returncode, 1, "Linux 识别 macOS snapshot 差异")
            changes = {row["path"] for row in json.loads(diff.stdout)["changes"]}
            ctx.check("/portable/你好-🚀/plan.txt" in changes, "Linux snapshot 内容差异")
            ctx.check("/portable/你好-🚀/plan-link.txt" in changes,
                      "Linux snapshot 链接差异")
            run_process(adapter.prefix + ["snapshot", "restore", "mac-baseline"])

            mount_adapter_mount(ctx, adapter)
            try:
                verify_portable_tree(ctx, adapter, b"mac-v1\n", linux_file=False)
                plan = adapter.mount_point / "portable/你好-🚀/plan.txt"
                plan.write_bytes(b"mac-v1\nlinux-v2\n")
                (adapter.mount_point / "portable/from-linux.txt").write_text(
                    "created on linux\n", encoding="utf-8")
            finally:
                mount_adapter_unmount(ctx, adapter, "Linux 第二次")
            verify_portable_acl(ctx, adapter)
            verify_portable_database_owner(ctx, adapter)
            snapshot = int(run_process(
                adapter.prefix + ["snapshot", "create", "linux-baseline"]
            ).stdout.strip())
            ctx.check(snapshot > 0, "Linux baseline snapshot")
            return {"phase": phase, "adapter": adapter.name,
                    "database": str(database), "snapshot": snapshot}

        mount_adapter_mount(ctx, adapter)
        try:
            verify_portable_tree(
                ctx, adapter, b"mac-v1\nlinux-v2\n", linux_file=True)
        finally:
            mount_adapter_unmount(ctx, adapter, "macOS 回读")
        verify_portable_acl(ctx, adapter)
        verify_portable_database_owner(ctx, adapter)

        history = json.loads(run_process(
            adapter.prefix + ["--json", "history", "/portable/你好-🚀/plan.txt"]
        ).stdout)["entries"]
        historical = [run_process(
            adapter.prefix + ["show", "/portable/你好-🚀/plan.txt",
                              "--version", str(entry["version"])]
        ).stdout for entry in history]
        ctx.check(b"mac-v1\n" in historical, "macOS 可读 Linux 保留的旧文件版本")
        snapshots = json.loads(run_process(
            adapter.prefix + ["--json", "snapshot", "list"]).stdout)
        snapshot_names = {entry["name"] for entry in snapshots}
        ctx.check({"mac-baseline", "linux-baseline"}.issubset(snapshot_names),
                  "macOS 可见两端 snapshot")
        run_process(adapter.prefix + ["write", "/portable/你好-🚀/plan.txt"],
                    input_data=b"mac-temporary\n")
        run_process(adapter.prefix + ["snapshot", "restore", "linux-baseline"])
        clean = run_process(adapter.prefix + ["snapshot", "diff", "linux-baseline"])
        ctx.equal(json.loads(clean.stdout)["changes"], [],
                  "macOS 恢复 Linux snapshot 后无差异")

        mount_adapter_mount(ctx, adapter)
        try:
            verify_portable_tree(
                ctx, adapter, b"mac-v1\nlinux-v2\n", linux_file=True)
        finally:
            mount_adapter_unmount(ctx, adapter, "macOS 恢复 Linux snapshot")
        unified_cli = adapter.cli.parent / "vexdb"
        ctx.check(unified_cli.exists(), "跨系统测试缺少统一 vexdb 入口")
        integrity = run_process(
            [str(unified_cli), str(database), "PRAGMA integrity_check;"]).stdout.strip()
        ctx.equal(integrity, b"ok", "跨系统往返后 SQLite 完整性")
        return {"phase": phase, "adapter": adapter.name,
                "database": str(database), "history_versions": len(history),
                "snapshots": sorted(snapshot_names)}


@case("mount.timestamps", "mount",
      "真实挂载的 utimens、写入和目录变更时间戳语义")
def mount_timestamps(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-timestamps-") as directory:
        adapter = prepare_real_mount_adapter(ctx, Path(directory), "timestamps")
        mount_adapter_mount(ctx, adapter)
        try:
            root = adapter.mount_point / "times"
            root.mkdir()
            file = root / "value.txt"
            file.write_bytes(b"before")
            requested_atime = 1_700_000_000_123_000_000
            requested_mtime = 1_700_000_100_456_000_000
            os.utime(file, ns=(requested_atime, requested_mtime))
            updated = file.stat()
            ctx.check(abs(updated.st_atime_ns - requested_atime) <= 1_000_000,
                      "utimens access time 必须达到毫秒精度: "
                      f"requested={requested_atime}, actual={updated.st_atime_ns}")
            ctx.check(abs(updated.st_mtime_ns - requested_mtime) <= 1_000_000,
                      "utimens modify time 必须达到毫秒精度: "
                      f"requested={requested_mtime}, actual={updated.st_mtime_ns}")

            before_write = file.stat()
            time.sleep(0.01)
            with file.open("ab") as stream:
                stream.write(b"-after")
                stream.flush()
                os.fsync(stream.fileno())
            after_write = file.stat()
            ctx.check(after_write.st_mtime_ns > before_write.st_mtime_ns,
                      "写入必须推进 modify time")
            ctx.check(after_write.st_ctime_ns >= before_write.st_ctime_ns,
                      "写入不能倒退 change time")

            before_child = root.stat()
            time.sleep(0.01)
            (root / "child.txt").write_text("child", encoding="utf-8")
            after_child = root.stat()
            ctx.check(after_child.st_mtime_ns > before_child.st_mtime_ns,
                      "目录项变化必须推进目录 modify time")
            return {
                "adapter": adapter.name,
                "requested_atime_ns": requested_atime,
                "actual_atime_ns": updated.st_atime_ns,
                "requested_mtime_ns": requested_mtime,
                "actual_mtime_ns": updated.st_mtime_ns,
                "write_mtime_delta_ns": after_write.st_mtime_ns - before_write.st_mtime_ns,
                "directory_mtime_delta_ns": after_child.st_mtime_ns - before_child.st_mtime_ns,
            }
        finally:
            mount_adapter_unmount(ctx, adapter, "时间戳")


@case("mount.concurrent-append", "mount",
      "多个真实进程通过 O_APPEND 写同一文件且不丢失、不覆盖")
def mount_concurrent_append(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-append-") as directory:
        adapter = prepare_real_mount_adapter(ctx, Path(directory), "append")
        mount_adapter_mount(ctx, adapter)
        try:
            target = adapter.mount_point / "append.log"
            target.write_bytes(b"")
            workers = 4
            records_per_worker = 100
            child = (
                "import os,sys\n"
                "path,worker,count=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])\n"
                "fd=os.open(path,os.O_WRONLY|os.O_APPEND)\n"
                "try:\n"
                "  for index in range(count):\n"
                "    value=f'{worker:02d}:{index:04d}\\n'.encode()\n"
                "    if os.write(fd,value)!=len(value): raise RuntimeError('short append')\n"
                "  os.fsync(fd)\n"
                "finally:\n"
                "  os.close(fd)\n"
            )
            started = time.perf_counter()
            processes = [subprocess.Popen(
                [sys.executable, "-c", child, str(target), str(worker),
                 str(records_per_worker)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                for worker in range(workers)]
            failures: list[str] = []
            for process in processes:
                stdout, stderr = process.communicate(timeout=60)
                if process.returncode != 0:
                    failures.append(
                        f"exit={process.returncode} stdout={stdout!r} stderr={stderr!r}")
            ctx.equal(failures, [], "并发 O_APPEND 子进程")
            elapsed = time.perf_counter() - started
            lines = target.read_text(encoding="utf-8").splitlines()
            expected = {f"{worker:02d}:{index:04d}"
                        for worker in range(workers)
                        for index in range(records_per_worker)}
            ctx.equal(len(lines), len(expected), "并发 append 行数")
            ctx.equal(set(lines), expected, "并发 append 不能丢失或重复记录")
            return {
                "adapter": adapter.name,
                "workers": workers,
                "records": len(lines),
                "elapsed_seconds": elapsed,
                "records_per_second": len(lines) / elapsed,
            }
        finally:
            mount_adapter_unmount(ctx, adapter, "append")


@case("mount.open-rename-unlink", "mount",
      "打开文件在 rename/unlink 后保持可读写，且同名新文件不复用旧 inode")
def mount_open_rename_unlink(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-open-life-") as directory:
        adapter = prepare_real_mount_adapter(ctx, Path(directory), "open-life")
        mount_adapter_mount(ctx, adapter)
        try:
            original = adapter.mount_point / "original.txt"
            renamed = adapter.mount_point / "renamed.txt"
            original.write_bytes(b"abcdef")
            descriptor = os.open(original, os.O_RDWR)
            try:
                original.rename(renamed)
                ctx.equal(os.pwrite(descriptor, b"XY", 2), 2, "rename 后旧 fd 写入")
                os.fsync(descriptor)
                ctx.equal(os.pread(descriptor, 6, 0), b"abXYef", "rename 后旧 fd 读取")
            finally:
                os.close(descriptor)
            ctx.equal(renamed.read_bytes(), b"abXYef", "rename 后路径内容")

            descriptor = os.open(renamed, os.O_RDWR)
            old_inode = os.fstat(descriptor).st_ino
            try:
                renamed.unlink()
                ctx.check(not renamed.exists(), "unlink 后目录项立即消失")
                ctx.equal(os.pwrite(descriptor, b"OLD", 0), 3, "unlink 后旧 fd 写入")
                os.fsync(descriptor)
                ctx.equal(os.pread(descriptor, 6, 0), b"OLDYef", "unlink 后旧 fd 读取")
                renamed.write_bytes(b"new-file")
                ctx.check(renamed.stat().st_ino != old_inode, "同名新文件必须使用新 inode")
                ctx.equal(renamed.read_bytes(), b"new-file", "同名新文件内容独立")
            finally:
                os.close(descriptor)
            ctx.equal(renamed.read_bytes(), b"new-file", "旧 fd 关闭不覆盖同名新文件")
            return {"adapter": adapter.name, "old_inode": old_inode,
                    "new_inode": renamed.stat().st_ino}
        finally:
            mount_adapter_unmount(ctx, adapter, "打开文件生命周期")


@case("mount.read-only-open-lifecycle", "mount",
      "小文件只读快速路径和大文件 handle 在 rename/unlink/并发改写后保持正确")
def mount_read_only_open_lifecycle(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-read-life-") as directory:
        adapter = prepare_real_mount_adapter(ctx, Path(directory), "read-life")
        mount_adapter_mount(ctx, adapter)
        try:
            root = adapter.mount_point

            small = root / "small.txt"
            renamed_small = root / "small-renamed.txt"
            small_payload = b"small-read-only-payload"
            small.write_bytes(small_payload)
            descriptor = os.open(small, os.O_RDONLY)
            old_inode = os.fstat(descriptor).st_ino
            try:
                small.rename(renamed_small)
                ctx.equal(os.pread(descriptor, len(small_payload), 0), small_payload,
                          "小文件只读 fd 在 rename 后读取")
                renamed_small.unlink()
                ctx.equal(os.pread(descriptor, len(small_payload), 0), small_payload,
                          "小文件只读 fd 在 unlink 后读取")
                renamed_small.write_bytes(b"replacement")
                ctx.check(renamed_small.stat().st_ino != old_inode,
                          "小文件同名重建不能复用已 unlink inode")
            finally:
                os.close(descriptor)
            ctx.equal(renamed_small.read_bytes(), b"replacement",
                      "小文件旧只读 fd 关闭不影响同名新文件")

            shared = root / "shared.txt"
            shared.write_bytes(b"before")
            reader = os.open(shared, os.O_RDONLY)
            writer = os.open(shared, os.O_WRONLY)
            try:
                ctx.equal(os.pwrite(writer, b"after!", 0), 6, "并发写描述符写入")
                os.fsync(writer)
                ctx.equal(os.pread(reader, 6, 0), b"after!",
                          "同 vnode 只读描述符看到已提交改写")
                os.truncate(shared, 3)
                ctx.equal(os.pread(reader, 8, 0), b"aft",
                          "同 vnode 只读描述符看到路径截断")
            finally:
                os.close(writer)
                os.close(reader)

            large = root / "large.bin"
            renamed_large = root / "large-renamed.bin"
            large_payload = b"L" * (1024 * 1024 + 257)
            large.write_bytes(large_payload)
            descriptor = os.open(large, os.O_RDONLY)
            large_inode = os.fstat(descriptor).st_ino
            try:
                large.rename(renamed_large)
                renamed_large.unlink()
                ctx.equal(os.pread(descriptor, 32, 0), large_payload[:32],
                          "大文件只读 handle 在 unlink 后读取开头")
                ctx.equal(os.pread(descriptor, 257, 1024 * 1024), large_payload[-257:],
                          "大文件只读 handle 在 unlink 后读取结尾")
                renamed_large.write_bytes(b"new-large")
                ctx.check(renamed_large.stat().st_ino != large_inode,
                          "大文件同名重建不能复用已 unlink inode")
            finally:
                os.close(descriptor)
            ctx.equal(renamed_large.read_bytes(), b"new-large",
                      "大文件旧只读 fd 关闭不影响同名新文件")
            return {
                "adapter": adapter.name,
                "small_bytes": len(small_payload),
                "large_bytes": len(large_payload),
            }
        finally:
            mount_adapter_unmount(ctx, adapter, "只读打开生命周期")


def run_lock_probe(path: Path, operation: str, start: int = 0,
                   length: int = 0) -> subprocess.CompletedProcess[bytes]:
    child = (
        "import fcntl,os,sys\n"
        "fd=os.open(sys.argv[1],os.O_RDWR)\n"
        "try:\n"
        "  try:\n"
        "    if sys.argv[2]=='flock': fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)\n"
        "    else: fcntl.lockf(fd,fcntl.LOCK_EX|fcntl.LOCK_NB,int(sys.argv[4]),"
        "int(sys.argv[3]),os.SEEK_SET)\n"
        "  except BlockingIOError: sys.exit(10)\n"
        "finally:\n"
        "  os.close(fd)\n"
    )
    return run_process([sys.executable, "-c", child, str(path), operation,
                        str(start), str(length)], check=False)


@case("mount.process-locks", "mount",
      "真实进程间 flock 与 fcntl byte-range 最小锁语义")
def mount_process_locks(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-locks-") as directory:
        adapter = prepare_real_mount_adapter(ctx, Path(directory), "locks")
        mount_adapter_mount(ctx, adapter)
        try:
            target = adapter.mount_point / "lock.bin"
            target.write_bytes(b"0" * 64)
            descriptor = os.open(target, os.O_RDWR)
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                ctx.equal(run_lock_probe(target, "flock").returncode, 10,
                          "flock 冲突必须返回 would-block")
                fcntl.flock(descriptor, fcntl.LOCK_UN)
                ctx.equal(run_lock_probe(target, "flock").returncode, 0,
                          "flock 释放后可再次获取")

                fcntl.lockf(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB, 16, 0, os.SEEK_SET)
                ctx.equal(run_lock_probe(target, "lockf", 0, 16).returncode, 10,
                          "重叠 byte-range lock 必须冲突")
                ctx.equal(run_lock_probe(target, "lockf", 32, 16).returncode, 0,
                          "不重叠 byte-range lock 可以并存")
                fcntl.lockf(descriptor, fcntl.LOCK_UN, 16, 0, os.SEEK_SET)
            finally:
                os.close(descriptor)
            return {"adapter": adapter.name, "flock": True, "fcntl_range": True}
        finally:
            mount_adapter_unmount(ctx, adapter, "进程锁")


@case("mount.force-unmount", "mount",
      "强制卸载后挂载状态消失，重新挂载仍可读取已提交内容")
def mount_force_unmount(ctx: Context) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-force-") as directory:
        adapter = prepare_real_mount_adapter(ctx, Path(directory), "force-unmount")
        mount_adapter_mount(ctx, adapter)
        target = adapter.mount_point / "durable.txt"
        target.write_bytes(b"force-durable")
        with target.open("rb") as stream:
            os.fsync(stream.fileno())
        forced = run_process(
            adapter.prefix + ["unmount", "--force", str(adapter.mount_point)],
            check=False, timeout=60)
        ctx.equal(forced.returncode, 0, f"{adapter.name} 强制卸载退出码")
        status_result = run_process(
            adapter.prefix + ["--json", "mount", "status", str(adapter.mount_point)],
            check=False)
        ctx.equal(status_result.returncode, 1, "强制卸载后指定路径状态退出码")
        status = json.loads(status_result.stdout)
        ctx.equal(status, [], "强制卸载后状态必须消失")
        if adapter.name == "fskit":
            time.sleep(2.0)
        mount_adapter_mount(ctx, adapter)
        try:
            ctx.equal(target.read_bytes(), b"force-durable", "强制卸载后已提交内容")
            return {"adapter": adapter.name}
        finally:
            mount_adapter_unmount(ctx, adapter, "强制卸载重挂载")


@case("mount.helper-crash-recovery", "mount",
      "Linux FUSE helper 异常退出后保留 staging，并在新 session 自动发布")
def mount_helper_crash_recovery(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Linux":
        raise EvalSkip("helper 异常退出恢复当前只在 Linux libfuse3 执行")
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-helper-crash-") as directory:
        adapter = prepare_real_mount_adapter(ctx, Path(directory), "helper-crash")
        mount_adapter_mount(ctx, adapter)
        target = adapter.mount_point / "staged.txt"
        target.write_bytes(b"baseline")
        child_code = (
            "import os,sys,time\n"
            "fd=os.open(sys.argv[1],os.O_RDWR)\n"
            "os.pwrite(fd,b'CRASH',0)\n"
            "print('staged',flush=True)\n"
            "time.sleep(120)\n"
        )
        child = subprocess.Popen([sys.executable, "-c", child_code, str(target)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        remounted = False
        try:
            ready = child.stdout.readline().decode(errors="replace").strip()
            ctx.equal(ready, "staged", "子进程已写入但未关闭 fd")
            process_table = run_process(["ps", "-eo", "pid=,args="]).stdout.decode()
            helper_pids = []
            for line in process_table.splitlines():
                fields = line.strip().split(maxsplit=1)
                if len(fields) != 2:
                    continue
                if ("vexfs-fuse" in fields[1] and str(adapter.database) in fields[1]
                        and str(adapter.mount_point) in fields[1]):
                    helper_pids.append(int(fields[0]))
            ctx.equal(len(helper_pids), 1, "定位唯一 FUSE helper")
            os.kill(helper_pids[0], signal.SIGKILL)
            child.terminate()
            child.wait(timeout=10)

            forced = run_process(
                adapter.prefix + ["unmount", "--force", str(adapter.mount_point)],
                check=False, timeout=60)
            ctx.equal(forced.returncode, 0, "helper 崩溃后强制卸载")
            with sqlite3.connect(adapter.database) as connection:
                dirty = connection.execute(
                    "SELECT count(*) FROM _vexfs_handles "
                    "WHERE dirty_generation>published_generation").fetchone()[0]
                ctx.equal(dirty, 1, "helper 崩溃后 staging 仍在数据库")
                connection.execute("UPDATE _vexfs_mount_sessions SET lease_until=0")

            mount_adapter_mount(ctx, adapter)
            remounted = True
            ctx.equal(target.read_bytes(), b"CRASHine", "新 session 自动发布崩溃前 staging")
            with sqlite3.connect(adapter.database) as connection:
                dirty_after = connection.execute(
                    "SELECT count(*) FROM _vexfs_handles "
                    "WHERE dirty_generation>published_generation").fetchone()[0]
            ctx.equal(dirty_after, 0, "恢复后没有未发布 staging")
            return {"adapter": adapter.name, "helper_pid": helper_pids[0]}
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=10)
            if remounted:
                mount_adapter_unmount(ctx, adapter, "helper 崩溃恢复")


@case("mount.real-bash", "mount", "真实 FSKit mount 后用 bash 常用文件命令操作")
def real_mount_bash(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise EvalSkip("真实 FSKit mount 只在 macOS 执行")
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-eval-") as directory:
        base = Path(directory)
        database = base / "mount.sqlite3"
        mount_point = base / "mnt"
        cli = mount_cli(ctx)
        prefix = filesystem_cli_prefix(cli, database, "eval")
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
/bin/ls -la "$1/project"
find "$1/project" -type f | sort
rm "$1/project/moved.txt"
test "$(/bin/cat "$1/project/sub/result.txt")" = agent
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
        return {"cli": str(cli), "doctor_before": details, "doctor_after": after,
                "mounts": mounts,
                "descriptor": descriptor,
                "commands": ["mkdir", "printf", "grep", "cp", "mv", "cmp", "ls",
                             "find", "rm", "cat", "vexfs cat"]}


@case("mount.real-linux-bash-git", "mount",
      "真实 libfuse3 mount 后运行 Bash、可执行脚本、链接和 Git 工作区")
def real_linux_mount_bash_git(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Linux":
        raise EvalSkip("真实 libfuse3 mount 只在 Linux 执行")
    helper = ctx.build_dir / "vexfs-fuse"
    if not helper.exists():
        raise EvalSkip("当前构建没有 vexfs-fuse helper")
    if not Path("/dev/fuse").exists() or not os.access("/dev/fuse", os.R_OK | os.W_OK):
        raise EvalSkip("当前环境没有可读写的 /dev/fuse")
    if shutil.which("fusermount3") is None:
        raise EvalSkip("当前环境没有 fusermount3")
    if shutil.which("git") is None:
        raise EvalSkip("当前环境没有 git")

    with tempfile.TemporaryDirectory(prefix="vexfs-linux-mount-eval-") as directory:
        base = Path(directory)
        database = base / "mount.sqlite3"
        mount_point = base / "mnt"
        prefix = [str(ctx.cli), "--db", str(database), "--workspace", "linux-eval"]
        run_process(prefix + ["setup"])
        details = json.loads(run_process(prefix + ["--json", "doctor"], check=False).stdout)
        if not details.get("mount_ready"):
            raise EvalSkip(f"libfuse3 adapter={details.get('extension', 'unknown')}")
        mount_point.mkdir()
        run_process(prefix + ["mount", str(mount_point)], timeout=60)
        first_unmount = None
        try:
            shell = r'''
set -e
mkdir -p "$1/project/src"
printf '#!/bin/sh\nprintf agent-linux\n' > "$1/project/run.sh"
chmod 755 "$1/project/run.sh"
"$1/project/run.sh" > "$1/project/output.txt"
printf 'alpha\nagent\nomega\n' > "$1/project/src/input.txt"
grep agent "$1/project/src/input.txt" > "$1/project/src/result.txt"
cp "$1/project/src/input.txt" "$1/project/src/copy.txt"
mv "$1/project/src/copy.txt" "$1/project/src/moved.txt"
ln "$1/project/run.sh" "$1/project/run-hard.sh"
ln -s run.sh "$1/project/run-link.sh"
git -C "$1/project" init -q
git -C "$1/project" config user.name VexFS-Eval
git -C "$1/project" config user.email vexfs-eval@example.invalid
git -C "$1/project" add .
git -C "$1/project" commit -qm initial
test -z "$(git -C "$1/project" status --porcelain)"
test "$(cat "$1/project/output.txt")" = agent-linux
'''
            run_process(["/bin/sh", "-c", shell, "vexfs-linux-eval", str(mount_point)],
                        timeout=180)
            script = mount_point / "project/run.sh"
            hardlink = mount_point / "project/run-hard.sh"
            symlink = mount_point / "project/run-link.sh"
            ctx.equal(script.stat().st_ino, hardlink.stat().st_ino, "Linux hardlink inode")
            ctx.equal(script.stat().st_mode & 0o777, 0o755, "Linux executable mode")
            ctx.equal(script.stat().st_uid, os.getuid(), "Linux created-file uid")
            ctx.equal(script.stat().st_gid, os.getgid(), "Linux created-file gid")
            ctx.equal(os.readlink(symlink), "run.sh", "Linux symbolic link target")
            ctx.equal((mount_point / "project/src/result.txt").read_text(), "agent\n",
                      "Linux grep result")
            reverse = run_process(prefix + ["cat", "/project/output.txt"])
            ctx.equal(reverse.stdout, b"agent-linux", "Linux mount 写入可由数据库 CLI 读取")
        finally:
            first_unmount = run_process(prefix + ["unmount", str(mount_point)],
                                        check=False, timeout=60)
        ctx.equal(first_unmount.returncode, 0, "Linux 首次卸载")

        run_process(prefix + ["mount", str(mount_point)], timeout=60)
        second_unmount = None
        try:
            status = run_process(
                ["git", "-C", str(mount_point / "project"), "status", "--porcelain"],
                timeout=120)
            ctx.equal(status.stdout, b"", "Linux 重挂载后 Git workspace 完整")
            ctx.equal((mount_point / "project/output.txt").read_bytes(), b"agent-linux",
                      "Linux 重挂载内容")
        finally:
            second_unmount = run_process(prefix + ["unmount", str(mount_point)],
                                         check=False, timeout=60)
        ctx.equal(second_unmount.returncode, 0, "Linux 第二次卸载")
        after = json.loads(run_process(prefix + ["--json", "doctor"]).stdout)
        ctx.equal(after["mount_count"], 0, "Linux 卸载后无挂载")
        ctx.equal(after["database"]["pending_handles"], 0, "Linux 卸载后无未发布句柄")
        return {"doctor_before": details, "doctor_after": after,
                "commands": ["mkdir", "printf", "chmod", "exec", "grep", "cp", "mv",
                             "hardlink", "symlink", "git init", "git add", "git commit",
                             "git status", "unmount", "remount"]}


@case("mount.posix-metadata", "mount",
      "真实 FSKit mount 的可执行权限、符号链接、重命名和重挂载")
def real_mount_posix_metadata(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise EvalSkip("真实 FSKit mount 只在 macOS 执行")
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-posix-") as directory:
        base = Path(directory)
        database = base / "mount.sqlite3"
        mount_point = base / "mnt"
        cli = mount_cli(ctx)
        prefix = filesystem_cli_prefix(cli, database, "posix")
        run_process(prefix + ["setup"])
        details = json.loads(run_process(
            prefix + ["--json", "doctor"], check=False).stdout)
        if details.get("extension") != "enabled":
            raise EvalSkip(
                f"FSKit extension={details.get('extension', 'unknown')}，需安装并在系统设置中启用")

        mount_point.mkdir()
        run_process(prefix + ["mount", str(mount_point)], timeout=60)
        first_unmount = None
        script = mount_point / "project/run.sh"
        link = mount_point / "project/run-link"
        dangling = mount_point / "project/dangling"
        try:
            script.parent.mkdir()
            script.write_text("#!/bin/sh\nprintf 'vexfs-exec\\n'\n", encoding="utf-8")
            script.chmod(0o755)
            ctx.equal(script.stat().st_mode & 0o777, 0o755, "chmod 后可执行 mode")
            executed = run_process([str(script)], cwd=script.parent, timeout=30)
            ctx.equal(executed.stdout, b"vexfs-exec\n", "直接执行挂载盘脚本")

            link.symlink_to("run.sh")
            dangling.symlink_to("missing.txt")
            ctx.check(link.is_symlink() and dangling.is_symlink(), "符号链接类型")
            ctx.equal(os.readlink(link), "run.sh", "相对符号链接 target")
            ctx.equal(link.read_text(encoding="utf-8"), script.read_text(encoding="utf-8"),
                      "通过符号链接读取目标")
            ctx.check(not dangling.exists() and dangling.is_symlink(), "悬空符号链接")

            temporary = mount_point / "project/temp-link"
            temporary.symlink_to("run.sh")
            renamed = mount_point / "project/renamed-link"
            temporary.rename(renamed)
            ctx.equal(os.readlink(renamed), "run.sh", "重命名链接不改 target")
            renamed.unlink()
            ctx.check(script.exists(), "删除链接不删除目标")
        finally:
            first_unmount = run_process(prefix + ["unmount", str(mount_point)],
                                        check=False, timeout=60)
        ctx.equal(first_unmount.returncode, 0, "POSIX 首次卸载退出码")

        # FSKit 的 unload 在 umount 返回后仍可能异步收尾；等待旧 volume 释放同一
        # container identity，避免把系统生命周期竞态误判成持久化失败。
        time.sleep(2.0)
        run_process(prefix + ["mount", str(mount_point)], timeout=60)
        second_unmount = None
        try:
            ctx.equal(script.stat().st_mode & 0o777, 0o755, "重挂载后 mode 持久化")
            ctx.equal(os.readlink(link), "run.sh", "重挂载后链接 target 持久化")
            ctx.equal(run_process([str(link)], cwd=script.parent).stdout,
                      b"vexfs-exec\n", "重挂载后通过链接执行")
        finally:
            # mount 返回后 FSKit 仍会完成少量异步激活工作；这个回归用例操作很少，
            # 需要给 activate/deactivate 留出正常交接时间。
            time.sleep(2.0)
            second_unmount = run_process(prefix + ["unmount", str(mount_point)],
                                         check=False, timeout=60)
            if second_unmount.returncode != 0:
                run_process(["/sbin/umount", "-f", str(mount_point)],
                            check=False, timeout=60)
        ctx.equal(second_unmount.returncode, 0, "POSIX 第二次卸载退出码")

        connection = sqlite3.connect(database, isolation_level=None)
        connection.enable_load_extension(True)
        connection.load_extension(str(ctx.extension))
        try:
            row = connection.execute(
                "SELECT i.id,i.kind,i.mode,i.size FROM _vexfs_dentries d "
                "JOIN _vexfs_inodes i ON i.id=d.inode_id "
                "JOIN _vexfs_workspaces w ON w.id=d.workspace_id "
                "WHERE w.name='posix' AND d.name='run-link' AND i.deleted_at IS NULL"
            ).fetchone()
            ctx.check(row is not None, "SQLite 中存在符号链接 inode")
            assert row is not None
            ctx.equal((row[1], row[2], row[3]), ("symlink", 0o777, len("run.sh")),
                      "SQLite 中的链接元数据")
            target = connection.execute(
                "SELECT vexfs_readlink('posix',?)", (row[0],)).fetchone()[0]
            ctx.equal(target, b"run.sh", "SQLite 中保存链接 target")
            stored_mode = connection.execute(
                "SELECT i.mode FROM _vexfs_dentries d JOIN _vexfs_inodes i ON i.id=d.inode_id "
                "JOIN _vexfs_workspaces w ON w.id=d.workspace_id "
                "WHERE w.name='posix' AND d.name='run.sh' AND i.deleted_at IS NULL"
            ).fetchone()[0]
            ctx.equal(stored_mode, 0o755, "SQLite 中保存可执行权限")
        finally:
            connection.close()
        return {"cli": str(cli), "mode": 0o755, "target": "run.sh",
                "remounted": True, "database_verified": True}


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
        cli = mount_cli(ctx)
        prefix = filesystem_cli_prefix(cli, database, "perf")
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
            "cli": str(cli),
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


@case("mount.git-workspace", "mount", "真实 FSKit mount 中运行完整 Git 工作区流程")
def real_mount_git_workspace(ctx: Context) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise EvalSkip("真实 FSKit mount 只在 macOS 执行")
    git = shutil.which("git")
    if git is None:
        raise EvalSkip("系统没有 git")
    xattr = shutil.which("xattr")
    if xattr is None:
        raise EvalSkip("系统没有 xattr")
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-git-") as directory:
        base = Path(directory)
        database = base / "mount.sqlite3"
        mount_point = base / "mnt"
        cli = mount_cli(ctx)
        prefix = filesystem_cli_prefix(cli, database, "git-eval")
        run_process(prefix + ["setup"])
        details = json.loads(run_process(
            prefix + ["--json", "doctor"], check=False).stdout)
        if details.get("extension") != "enabled":
            raise EvalSkip(
                f"FSKit extension={details.get('extension', 'unknown')}，需安装并在系统设置中启用")
        mount_point.mkdir()
        run_process(prefix + ["mount", str(mount_point)], timeout=60)
        project = mount_point / "project"
        unmount = None
        try:
            project.mkdir()
            run_process([xattr, "-w", "com.vexfs.eval", "stored-in-sqlite", str(project)])
            xattr_value = run_process(
                [xattr, "-p", "com.vexfs.eval", str(project)], timeout=120)
            ctx.equal(xattr_value.stdout.strip(), b"stored-in-sqlite",
                      "挂载盘扩展属性读写")
            environment = os.environ.copy()
            environment.update({
                "GIT_CONFIG_NOSYSTEM": "1",
                "HOME": str(base / "home"),
            })
            (base / "home").mkdir()
            commands = [
                [git, "init", "-b", "main"],
                [git, "config", "user.name", "VexFS Eval"],
                [git, "config", "user.email", "vexfs-eval@example.invalid"],
                [git, "config", "core.ignorecase", "false"],
            ]
            for command in commands:
                run_process(command, cwd=project, env=environment, timeout=120)

            filemode = run_process(
                [git, "config", "--type=bool", "--default=true", "core.filemode"],
                cwd=project, env=environment)
            symlinks = run_process(
                [git, "config", "--type=bool", "--default=true", "core.symlinks"],
                cwd=project, env=environment)
            ctx.equal(filemode.stdout.strip(), b"true", "Git 识别可执行权限")
            ctx.equal(symlinks.stdout.strip(), b"true", "Git 识别符号链接")

            (project / "README.md").write_text("main\n", encoding="utf-8")
            executable = project / "run.sh"
            executable.write_text("#!/bin/sh\nprintf 'main-exec\\n'\n", encoding="utf-8")
            executable.chmod(0o755)
            current = project / "current"
            current.symlink_to("run.sh")
            run_process([git, "add", "."], cwd=project, env=environment)
            run_process([git, "commit", "-m", "initial"], cwd=project,
                        env=environment, timeout=120)
            run_process([git, "checkout", "-b", "feature"], cwd=project,
                        env=environment, timeout=120)
            (project / "README.md").write_text("feature\n", encoding="utf-8")
            executable.chmod(0o644)
            current.unlink()
            current.symlink_to("README.md")
            run_process([git, "add", "-A"], cwd=project, env=environment)
            run_process([git, "commit", "-m", "feature"], cwd=project,
                        env=environment, timeout=120)
            run_process([git, "checkout", "main"], cwd=project,
                        env=environment, timeout=120)
            status = run_process([git, "status", "--porcelain"], cwd=project,
                                 env=environment, timeout=120)
            ctx.equal(status.stdout, b"", "Git checkout 后工作区状态")
            apple_double = sorted(
                str(path.relative_to(project)) for path in project.rglob("._*")
            )
            ctx.equal(apple_double, [], "Git 工作区没有 AppleDouble 垃圾文件")
            ctx.equal((project / "README.md").read_text(encoding="utf-8"),
                      "main\n", "Git checkout 恢复主分支内容")
            ctx.equal(executable.stat().st_mode & 0o777, 0o755,
                      "Git checkout 恢复可执行权限")
            ctx.equal(os.readlink(current), "run.sh", "Git checkout 恢复符号链接")
            ctx.equal(run_process([str(current)], cwd=project, env=environment).stdout,
                      b"main-exec\n", "Git checkout 后通过链接执行")
            commits = run_process([git, "rev-list", "--all", "--count"], cwd=project,
                                  env=environment, timeout=120)
            ctx.equal(commits.stdout.strip(), b"2", "Git 提交数量")
            run_process([git, "gc"], cwd=project, env=environment, timeout=120)
            ctx.equal(run_process(prefix + ["cat", "/project/README.md"]).stdout,
                      b"main\n", "数据库 CLI 反向读取 Git 工作区")
        finally:
            unmount = run_process(prefix + ["unmount", str(mount_point)],
                                  check=False, timeout=60)
        ctx.equal(unmount.returncode, 0, "Git 测试卸载退出码")
        after = json.loads(run_process(prefix + ["--json", "doctor"]).stdout)
        ctx.equal(after["mount_count"], 0, "Git 测试卸载后挂载数")
        ctx.equal(after["database"]["pending_handles"], 0, "Git 测试未发布句柄")
        ctx.equal(after["database"]["retained_handles"], 0, "Git 测试保留句柄")
        ctx.equal(after["database"]["staging_bytes"], 0, "Git 测试暂存字节")
        connection = sqlite3.connect(database)
        try:
            stored_xattr = connection.execute(
                "SELECT value FROM _vexfs_xattrs WHERE name='com.vexfs.eval'"
            ).fetchone()
        finally:
            connection.close()
        ctx.equal(None if stored_xattr is None else stored_xattr[0], b"stored-in-sqlite",
                  "扩展属性持久化到 SQLite")
        return {
            "cli": str(cli),
            "git": run_process([git, "--version"]).stdout.decode().strip(),
            "filemode": True,
            "symlinks": True,
            "commands": ["xattr", "init", "config", "chmod", "symlink", "add",
                         "commit", "checkout", "status", "gc"],
            "doctor_after": after,
        }


@case("mount.real-toolchain-projects", "mount",
      "真实挂载目录中运行 Python、Node.js、Go、Rust 和 Git 项目并重挂载复测")
def real_mount_toolchain_projects(ctx: Context) -> dict[str, Any]:
    tool_paths = {name: shutil.which(name) for name in
                  ("python3", "node", "go", "cargo", "git")}
    missing = [name for name, path in tool_paths.items() if path is None]
    if missing:
        raise EvalSkip(f"当前环境缺少真实项目工具链: {', '.join(missing)}")

    with tempfile.TemporaryDirectory(prefix="vexfs-toolchain-projects-") as directory:
        base = Path(directory)
        adapter = prepare_real_mount_adapter(ctx, base, "toolchains")
        project = adapter.mount_point / "workspace"
        home = base / "home"
        cache = base / "cache"
        home.mkdir()
        cache.mkdir()
        host_home = Path.home()
        environment = os.environ.copy()
        environment.update({
            "HOME": str(home),
            "GIT_CONFIG_NOSYSTEM": "1",
            "GOCACHE": str(cache / "go-build"),
            "GOMODCACHE": str(cache / "go-mod"),
            "GOPATH": str(cache / "gopath"),
            # cargo/rustc 可能是 rustup shim；HOME 隔离后仍要指向已经安装好的
            # toolchain。项目 target 继续写在 VexFS 工作区，真实覆盖编译器文件操作。
            "CARGO_HOME": os.environ.get("CARGO_HOME", str(host_home / ".cargo")),
            "RUSTUP_HOME": os.environ.get("RUSTUP_HOME", str(host_home / ".rustup")),
            "CARGO_NET_OFFLINE": "true",
        })

        def run_toolchains() -> dict[str, float]:
            timings: dict[str, float] = {}
            commands = [
                ("python", [tool_paths["python3"], "-m", "unittest", "-v"],
                 project / "python"),
                ("node", [tool_paths["node"], "--test"], project / "node"),
                ("go", [tool_paths["go"], "test", "./..."], project / "go"),
                ("rust", [tool_paths["cargo"], "test", "--offline"], project / "rust"),
            ]
            for label, command, cwd in commands:
                started = time.perf_counter()
                run_process([str(part) for part in command], cwd=cwd,
                            env=environment, timeout=600)
                timings[f"{label}_seconds"] = round(time.perf_counter() - started, 6)
            return timings

        mount_adapter_mount(ctx, adapter)
        first_timings: dict[str, float] = {}
        first_unmounted = False
        try:
            project.mkdir()
            (project / ".gitignore").write_text(
                "__pycache__/\n*.pyc\ntarget/\n", encoding="utf-8")

            python_project = project / "python"
            python_project.mkdir()
            (python_project / "calc.py").write_text(
                "def add(left, right):\n    return left + right\n", encoding="utf-8")
            (python_project / "test_calc.py").write_text(
                "import unittest\nfrom calc import add\n\n"
                "class CalcTest(unittest.TestCase):\n"
                "    def test_add(self):\n        self.assertEqual(add(20, 22), 42)\n\n"
                "if __name__ == '__main__':\n    unittest.main()\n", encoding="utf-8")

            node_project = project / "node"
            node_project.mkdir()
            (node_project / "package.json").write_text(
                '{"name":"vexfs-node-eval","version":"1.0.0","type":"commonjs"}\n',
                encoding="utf-8")
            (node_project / "calc.js").write_text(
                "exports.add = (left, right) => left + right;\n", encoding="utf-8")
            (node_project / "calc.test.js").write_text(
                "const test = require('node:test');\n"
                "const assert = require('node:assert/strict');\n"
                "const { add } = require('./calc');\n"
                "test('add', () => assert.equal(add(20, 22), 42));\n", encoding="utf-8")

            go_project = project / "go"
            go_project.mkdir()
            (go_project / "go.mod").write_text(
                "module example.invalid/vexfs-eval\n\ngo 1.20\n", encoding="utf-8")
            (go_project / "calc.go").write_text(
                "package calc\n\nfunc Add(left, right int) int { return left + right }\n",
                encoding="utf-8")
            (go_project / "calc_test.go").write_text(
                "package calc\n\nimport \"testing\"\n\n"
                "func TestAdd(t *testing.T) { if Add(20, 22) != 42 { t.Fatal(\"bad sum\") } }\n",
                encoding="utf-8")

            rust_project = project / "rust"
            (rust_project / "src").mkdir(parents=True)
            (rust_project / "tests").mkdir()
            (rust_project / "Cargo.toml").write_text(
                "[package]\nname = \"vexfs-rust-eval\"\nversion = \"0.1.0\"\n"
                "edition = \"2021\"\n\n[dependencies]\n", encoding="utf-8")
            (rust_project / "src/lib.rs").write_text(
                "pub fn add(left: i32, right: i32) -> i32 { left + right }\n",
                encoding="utf-8")
            (rust_project / "tests/integration.rs").write_text(
                "use vexfs_rust_eval::add;\n\n#[test]\nfn adds() { assert_eq!(add(20, 22), 42); }\n",
                encoding="utf-8")
            environment["CARGO_TARGET_DIR"] = str(rust_project / "target")

            first_timings = run_toolchains()
            git = str(tool_paths["git"])
            run_process([git, "init", "-b", "main"], cwd=project, env=environment)
            run_process([git, "config", "user.name", "VexFS Toolchain Eval"],
                        cwd=project, env=environment)
            run_process([git, "config", "user.email", "eval@example.invalid"],
                        cwd=project, env=environment)
            run_process([git, "add", "."], cwd=project, env=environment, timeout=300)
            run_process([git, "commit", "-m", "toolchain projects"], cwd=project,
                        env=environment, timeout=300)
            ctx.equal(run_process([git, "status", "--porcelain"], cwd=project,
                                  env=environment).stdout, b"", "工具链项目 Git 状态")
        finally:
            mount_adapter_unmount(ctx, adapter, "工具链首次")
            first_unmounted = True

        connection = sqlite3.connect(adapter.database)
        try:
            ctx.equal(connection.execute("PRAGMA integrity_check").fetchone()[0], "ok",
                      "工具链项目数据库完整性")
            stored_files = connection.execute(
                "SELECT count(*) FROM _vexfs_inodes WHERE kind='file' AND deleted_at IS NULL"
            ).fetchone()[0]
        finally:
            connection.close()
        ctx.check(stored_files >= 15, "工具链项目产物进入数据库")

        mount_adapter_mount(ctx, adapter)
        second_timings: dict[str, float] = {}
        try:
            second_timings = run_toolchains()
            git = str(tool_paths["git"])
            ctx.equal(run_process([git, "status", "--porcelain"], cwd=project,
                                  env=environment).stdout, b"", "重挂载后 Git 状态")
        finally:
            mount_adapter_unmount(ctx, adapter, "工具链第二次")
        ctx.check(first_unmounted, "工具链首次卸载完成")
        after = json.loads(run_process(adapter.prefix + ["--json", "doctor"]).stdout)
        ctx.equal(after["database"]["pending_handles"], 0, "工具链无未发布句柄")
        ctx.equal(after["database"]["retained_handles"], 0, "工具链无保留句柄")
        return {
            "adapter": adapter.name,
            "tools": {
                "python": run_process([str(tool_paths["python3"]), "--version"]).stdout.decode().strip(),
                "node": run_process([str(tool_paths["node"]), "--version"]).stdout.decode().strip(),
                "go": run_process([str(tool_paths["go"]), "version"]).stdout.decode().strip(),
                "cargo": run_process([str(tool_paths["cargo"]), "--version"]).stdout.decode().strip(),
                "git": run_process([str(tool_paths["git"]), "--version"]).stdout.decode().strip(),
            },
            "first_run": first_timings,
            "after_remount": second_timings,
            "stored_files": stored_files,
            "doctor_after": after,
        }


@case("mount.real-opencode-project", "mount",
      "OpenCode 在真实挂载项目中自主查看、修改、测试，并在重挂载后保留结果")
def real_mount_opencode_project(ctx: Context) -> dict[str, Any]:
    if os.environ.get("VEXFS_EVAL_OPENCODE") != "1":
        raise EvalSkip("设置 VEXFS_EVAL_OPENCODE=1 后执行真实模型调用")
    opencode = shutil.which("opencode")
    python = shutil.which("python3")
    git = shutil.which("git")
    if opencode is None or python is None or git is None:
        raise EvalSkip("真实 OpenCode 用例需要 opencode、python3 和 git")
    model = os.environ.get("VEXFS_EVAL_OPENCODE_MODEL", "openai/gpt-5.4-mini")

    with tempfile.TemporaryDirectory(prefix="vexfs-opencode-project-") as directory:
        base = Path(directory)
        adapter = prepare_real_mount_adapter(ctx, base, "opencode")
        project = adapter.mount_point / "agent-project"
        environment = os.environ.copy()
        environment["GIT_CONFIG_NOSYSTEM"] = "1"

        mount_adapter_mount(ctx, adapter)
        agent_seconds = 0.0
        changed: list[str] = []
        try:
            project.mkdir()
            (project / "calc.py").write_text(
                "def total_even(values):\n"
                "    # TODO: return the sum of only the even integers.\n"
                "    raise NotImplementedError\n",
                encoding="utf-8")
            (project / "test_calc.py").write_text(
                "import unittest\n"
                "from calc import total_even\n\n"
                "class CalcTest(unittest.TestCase):\n"
                "    def test_mixed_values(self):\n"
                "        self.assertEqual(total_even([1, 2, 3, 4, -6]), 0)\n\n"
                "    def test_empty(self):\n"
                "        self.assertEqual(total_even([]), 0)\n\n"
                "if __name__ == '__main__':\n"
                "    unittest.main()\n",
                encoding="utf-8")
            (project / "README.md").write_text(
                "Complete the TODO in calc.py and keep the public function name.\n",
                encoding="utf-8")
            run_process([git, "init", "-b", "main"], cwd=project, env=environment)
            run_process([git, "config", "user.name", "VexFS Agent Eval"],
                        cwd=project, env=environment)
            run_process([git, "config", "user.email", "eval@example.invalid"],
                        cwd=project, env=environment)
            run_process([git, "add", "."], cwd=project, env=environment)
            run_process([git, "commit", "-m", "agent task fixture"],
                        cwd=project, env=environment)

            prompt = (
                "Inspect this repository and complete the TODO in calc.py. "
                "Edit only calc.py. Run `python3 -m unittest -v` and do not stop "
                "until the tests pass. Do not ask questions and do not commit."
            )
            started = time.perf_counter()
            result = run_process(
                [opencode, "run", "--auto", "--pure", "--format", "json",
                 "--model", model, "--dir", str(project), prompt],
                cwd=project, env=environment, timeout=900)
            agent_seconds = time.perf_counter() - started
            ctx.check(len(result.stdout) > 0, "OpenCode 必须返回执行事件")
            run_process([python, "-m", "unittest", "-v"], cwd=project,
                        env=environment, timeout=120)
            changed = run_process([git, "diff", "--name-only"], cwd=project,
                                  env=environment).stdout.decode().splitlines()
            ctx.equal(changed, ["calc.py"], "OpenCode 只修改任务要求的文件")
            implementation = (project / "calc.py").read_text(encoding="utf-8")
            ctx.check("NotImplementedError" not in implementation,
                      "OpenCode 已完成 TODO")
        finally:
            mount_adapter_unmount(ctx, adapter, "OpenCode 首次")

        stored = run_process(adapter.prefix + ["cat", "/agent-project/calc.py"])
        ctx.check(b"NotImplementedError" not in stored.stdout,
                  "OpenCode 修改已经进入数据库权威内容")
        connection = sqlite3.connect(adapter.database)
        try:
            ctx.equal(connection.execute("PRAGMA integrity_check").fetchone()[0], "ok",
                      "OpenCode 项目数据库完整性")
        finally:
            connection.close()

        mount_adapter_mount(ctx, adapter)
        try:
            run_process([python, "-m", "unittest", "-v"], cwd=project,
                        env=environment, timeout=120)
            ctx.equal(run_process([git, "diff", "--name-only"], cwd=project,
                                  env=environment).stdout.decode().splitlines(),
                      ["calc.py"], "重挂载后保留 OpenCode 工作区差异")
        finally:
            mount_adapter_unmount(ctx, adapter, "OpenCode 第二次")
        return {
            "adapter": adapter.name,
            "opencode": run_process([opencode, "--version"]).stdout.decode().strip(),
            "model": model,
            "agent_seconds": round(agent_seconds, 6),
            "changed_files": changed,
        }


@case("mount.scale-tree", "mount",
      "真实挂载目录创建并遍历 1 千/1 万/10 万文件，执行文本搜索和快照恢复")
def real_mount_scale_tree(ctx: Context) -> dict[str, Any]:
    file_count = {"quick": 1_000, "full": 10_000, "stress": 100_000}[ctx.mode.name]
    directory_count = min(1_000, max(20, file_count // 100))
    with tempfile.TemporaryDirectory(prefix="vexfs-mount-scale-") as directory:
        base = Path(directory)
        adapter = prepare_real_mount_adapter(ctx, base, "mount-scale")
        root = adapter.mount_point / "scale"
        search = shutil.which("rg") or shutil.which("grep")
        if search is None:
            raise EvalSkip("系统没有 rg 或 grep")

        def create_tree(target: Path) -> None:
            target.mkdir()
            for directory_index in range(directory_count):
                (target / f"d{directory_index:04d}").mkdir()
            for file_index in range(file_count):
                bucket = file_index % directory_count
                marker = "needle" if file_index % 1_000 == 0 else "plain"
                (target / f"d{bucket:04d}/f{file_index:06d}.txt").write_text(
                    f"file={file_index}; marker={marker}\n", encoding="utf-8")

        def search_tree(target: Path) -> tuple[float, int, bool]:
            started = time.perf_counter()
            try:
                if Path(search).name == "rg":
                    result = run_process(
                        [search, "-l", "needle", str(target)], timeout=3_600)
                else:
                    result = run_process(
                        [search, "-R", "-l", "needle", str(target)], timeout=3_600)
            except subprocess.TimeoutExpired:
                return time.perf_counter() - started, 0, True
            return time.perf_counter() - started, len(
                [line for line in result.stdout.splitlines() if line]), False

        native_root = base / "native-scale"
        native_started = time.perf_counter()
        create_tree(native_root)
        native_create_seconds = time.perf_counter() - native_started
        native_walk_started = time.perf_counter()
        native_files = sum(len(files) for _, _, files in os.walk(native_root))
        native_walk_seconds = time.perf_counter() - native_walk_started
        native_search_seconds, native_matches, native_search_timed_out = search_tree(native_root)
        ctx.check(not native_search_timed_out, "原生目录对照搜索不能超时")
        ctx.equal(native_files, file_count, "原生目录对照文件数")
        expected_matches = (file_count + 999) // 1_000
        ctx.equal(native_matches, expected_matches, "原生目录对照搜索命中数")

        mount_adapter_mount(ctx, adapter)
        create_seconds = 0.0
        walk_seconds = 0.0
        search_seconds = 0.0
        search_timed_out = False
        matches = 0
        try:
            started = time.perf_counter()
            create_tree(root)
            create_seconds = time.perf_counter() - started

            started = time.perf_counter()
            walked_files = sum(len(files) for _, _, files in os.walk(root))
            walk_seconds = time.perf_counter() - started
            ctx.equal(walked_files, file_count, "真实规模目录遍历文件数")
            for index in (0, file_count // 2, file_count - 1):
                bucket = index % directory_count
                text = (root / f"d{bucket:04d}/f{index:06d}.txt").read_text()
                ctx.check(f"file={index};" in text, "真实规模目录抽样内容")

            search_seconds, matches, search_timed_out = search_tree(root)
            if not search_timed_out:
                ctx.equal(matches, expected_matches, "真实规模目录文本搜索命中数")
        finally:
            mount_adapter_unmount(ctx, adapter, "规模首次")

        database_search_started = time.perf_counter()
        database_search = json.loads(run_process(
            [str(ctx.cli), "--db", str(adapter.database), "--workspace", "mount-scale",
             "--json", "grep", "-l", "needle", "/scale", "--max-results", "10240"],
            timeout=3_600).stdout)
        database_search_seconds = time.perf_counter() - database_search_started
        ctx.equal(database_search["match_count"], expected_matches,
                  "数据库快路径搜索命中数")
        ctx.equal(database_search["files_scanned"], file_count,
                  "数据库快路径搜索扫描完整目录树")
        index_build_started = time.perf_counter()
        index_status = json.loads(run_process(
            [str(ctx.cli), "--db", str(adapter.database), "--workspace", "mount-scale",
             "index", "enable"], timeout=3_600).stdout)
        index_build_seconds = time.perf_counter() - index_build_started
        ctx.check(index_status["indexed_files"] >= file_count,
                  "数据库 trigram 索引覆盖业务文件和 FSKit 系统文件")
        indexed_search_started = time.perf_counter()
        indexed_search = json.loads(run_process(
            [str(ctx.cli), "--db", str(adapter.database), "--workspace", "mount-scale",
             "--json", "grep", "-l", "needle", "/scale", "--max-results", "10240"],
            timeout=3_600).stdout)
        indexed_search_seconds = time.perf_counter() - indexed_search_started
        ctx.equal(indexed_search["index_used"], True, "真实挂载数据使用 trigram 索引")
        ctx.equal(indexed_search["match_count"], expected_matches,
                  "trigram 索引搜索命中数")
        # 当前真实 FSKit helper 可能来自已安装的上一预览包。性能数据取完后关闭
        # 新索引，避免旧 helper 后续修改临时数据库时无法维护这个新合同。
        run_process(
            [str(ctx.cli), "--db", str(adapter.database), "--workspace", "mount-scale",
             "index", "disable"], timeout=300)

        snapshot_started = time.perf_counter()
        run_process(adapter.prefix + ["snapshot", "create", "scale-baseline"],
                    timeout=3_600)
        snapshot_seconds = time.perf_counter() - snapshot_started
        original = b"file=0; marker=needle\n"
        run_process(adapter.prefix + ["write", "/scale/d0000/f000000.txt"],
                    input_data=b"changed\n", timeout=300)
        diff = run_process(adapter.prefix + ["snapshot", "diff", "scale-baseline"],
                           check=False, timeout=3_600)
        ctx.equal(diff.returncode, 1, "真实规模目录快照检测变化")
        restore_started = time.perf_counter()
        run_process(adapter.prefix + ["snapshot", "restore", "scale-baseline"],
                    timeout=3_600)
        restore_seconds = time.perf_counter() - restore_started

        mount_adapter_mount(ctx, adapter)
        try:
            ctx.equal((root / "d0000/f000000.txt").read_bytes(), original,
                      "真实规模目录快照恢复内容")
            remounted_files = sum(len(files) for _, _, files in os.walk(root))
            ctx.equal(remounted_files, file_count, "真实规模目录重挂载文件数")
        finally:
            mount_adapter_unmount(ctx, adapter, "规模第二次")

        connection = sqlite3.connect(adapter.database)
        storage_rows: dict[str, int] = {}
        try:
            ctx.equal(connection.execute("PRAGMA integrity_check").fetchone()[0], "ok",
                      "真实规模目录数据库完整性")
            storage_rows = {
                label: int(connection.execute(f"SELECT count(*) FROM {table}").fetchone()[0])
                for label, table in (
                    ("inode_rows", "_vexfs_inodes"),
                    ("commit_rows", "_vexfs_commits"),
                    ("file_version_rows", "_vexfs_file_versions"),
                    ("request_rows", "_vexfs_requests"),
                    ("inode_history_rows", "_vexfs_inode_states"),
                    ("dentry_history_rows", "_vexfs_dentry_states"),
                    ("acl_rows", "_vexfs_acl_entries"),
                    ("xattr_rows", "_vexfs_xattrs"),
                )
            }
        finally:
            connection.close()
        create_budget = {"quick": 600.0, "full": 3_600.0, "stress": 14_400.0}[
            ctx.mode.name]
        ctx.check(create_seconds >= 0, "mount_scale_create_seconds 不能为负数")
        deferred_failures: list[str] = []
        if ctx.enforce_performance:
            ctx.checks += 1
            if create_seconds > create_budget:
                deferred_failures.append(
                    "mount_scale_create_seconds 超过预算: "
                    f"{create_seconds:.3f} > {create_budget:.3f}")
        if search_timed_out:
            ctx.checks += 1
            deferred_failures.append("真实规模目录文本搜索在 3600 秒内未完成")
        metrics = {
            "adapter": adapter.name,
            "files": file_count,
            "directories": directory_count,
            "create_seconds": round(create_seconds, 6),
            "create_files_per_second": round(file_count / max(create_seconds, 1e-9), 3),
            "walk_seconds": round(walk_seconds, 6),
            "search_seconds": round(search_seconds, 6),
            "search_timed_out": search_timed_out,
            "search_matches": matches,
            "database_search_seconds": round(database_search_seconds, 6),
            "database_search_matches": database_search["match_count"],
            "database_search_files_per_second": round(
                file_count / max(database_search_seconds, 1e-9), 3),
            "index_build_seconds": round(index_build_seconds, 6),
            "indexed_search_seconds": round(indexed_search_seconds, 6),
            "indexed_search_matches": indexed_search["match_count"],
            "indexed_search_candidates": indexed_search["files_scanned"],
            "native_create_seconds": round(native_create_seconds, 6),
            "native_create_files_per_second": round(
                file_count / max(native_create_seconds, 1e-9), 3),
            "native_walk_seconds": round(native_walk_seconds, 6),
            "native_search_seconds": round(native_search_seconds, 6),
            "create_slowdown_vs_native": round(
                create_seconds / max(native_create_seconds, 1e-9), 3),
            "search_slowdown_vs_native": round(
                search_seconds / max(native_search_seconds, 1e-9), 3),
            "snapshot_seconds": round(snapshot_seconds, 6),
            "restore_seconds": round(restore_seconds, 6),
            "database_bytes": adapter.database.stat().st_size,
            "database_bytes_per_file": round(
                adapter.database.stat().st_size / max(file_count, 1), 3),
            **storage_rows,
            "commits_per_file": round(storage_rows["commit_rows"] / file_count, 3),
            "versions_per_file": round(storage_rows["file_version_rows"] / file_count, 3),
            "requests_per_file": round(storage_rows["request_rows"] / file_count, 3),
        }
        if deferred_failures:
            raise EvalFailureWithMetrics("; ".join(deferred_failures), metrics)
        return metrics


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
    if operation == "snapshot-restore-race":
        path, extension, expected_head_text, ready_path, barrier_path = arguments[1:]
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
            commit = connection.execute(
                "SELECT vexfs_snapshot_restore('default','race-point',?)",
                (int(expected_head_text),)).fetchone()[0]
            print(commit)
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
    # 保留 vexfs 符号链接本身，统一二进制通过 argv[0] 进入兼容文件命令。
    cli = build_dir / "vexfs"
    if not cli.exists():
        raise SystemExit(f"找不到 VexFS CLI，请先构建: {cli}")
    mode = MODES[options.mode]
    seed = options.seed
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + f"-{mode.name}-{seed}"
    output_dir = Path(options.output_dir).resolve() / run_id
    output_dir.mkdir(parents=True, exist_ok=False)
    mount_cli_override = Path(options.mount_cli).expanduser().absolute() if options.mount_cli else None
    if mount_cli_override is not None and not mount_cli_override.exists():
        raise SystemExit(f"指定的 mount CLI 不存在: {mount_cli_override}")
    ctx = Context(root, build_dir, ext, cli, output_dir, mode, seed,
                  options.enforce_performance, options.fail_on_skip,
                  mount_cli_override)
    selected = [item for item in CASES
                if mode.name in item.modes and
                (item.category != "package" or options.include_package)]
    if options.filter:
        selected = [item for item in selected if options.filter in item.case_id or
                    options.filter in item.category]
    if not selected:
        raise SystemExit(
            f"没有匹配的 eval 用例: filter={options.filter!r}, "
            f"include_package={options.include_package}")
    if (options.fail_on_skip and platform.system() == "Darwin" and
            any(item.category == "mount" for item in selected) and
            mount_cli_override is None):
        raise SystemExit("macOS 发行 mount Gate 必须用 --mount-cli 显式绑定签名 CLI")
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
        except EvalFailureWithMetrics as exception:
            status = "FAIL"
            metrics = exception.metrics
            error = str(exception)
            trace = traceback.format_exc()
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
    gate_failed = failed > 0 or (options.fail_on_skip and skipped > 0)
    if gate_failed:
        overall_status = "FAIL"
    elif skipped > 0:
        overall_status = "PASS_WITH_SKIPS"
    else:
        overall_status = "PASS"
    selected_mount_cli = mount_cli(ctx)
    mount_cli_sha256 = hashlib.sha256(selected_mount_cli.read_bytes()).hexdigest()
    report = {
        "schema_version": 1,
        "run_id": run_id,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "mode": mode.name,
        "seed": seed,
        "enforce_performance": options.enforce_performance,
        "fail_on_skip": options.fail_on_skip,
        "environment": system_details(root, build_dir, ext),
        "extension": str(ext),
        "mount_cli": str(selected_mount_cli),
        "mount_cli_sha256": mount_cli_sha256,
        "cases": results,
        "summary": {
            "status": overall_status,
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
    return 1 if gate_failed else 0


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
    parser.add_argument("--include-package", action="store_true",
                        help="额外运行需要预先重打包和签名的交付包测试")
    parser.add_argument("--fail-on-skip", action="store_true",
                        help="任何 SKIP 都视为 Gate 失败；发行和真机 wrapper 必须开启")
    parser.add_argument("--mount-cli", default=os.environ.get("VEXFS_EVAL_MOUNT_CLI", ""),
                        help="真实 macOS Gate 使用的签名 vexdb/vexfs；不再猜测 dist 最新包")
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
