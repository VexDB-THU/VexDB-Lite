#!/usr/bin/env bash
# release.sh — 远程跨架构打包 + GitHub Release 发布
#
# 用法:
#   bash scripts/release.sh build [target] [arch]    # 远程 build + 拉回 dist/ (默认动作)
#   bash scripts/release.sh package                  # 把 dist/<arch>/ 打成 tarball + SHA256SUMS
#   bash scripts/release.sh test [target] [arch]     # 原生 Linux 验证（target: duck|sqlite|all）
#   bash scripts/release.sh upload <tag>             # 上传 tarballs 到 GitHub Release
#
#   build target: duck | pg | sqlite | all (default: all)
#   build arch:   x86 | arm | all (default: all)
#
#   示例:
#     bash scripts/release.sh                          # build 全量 (4 个产物)
#     bash scripts/release.sh build duck arm           # 只 build ARM DuckDB
#     bash scripts/release.sh package                  # 4 个 tarball 进 dist/release/
#     bash scripts/release.sh test sqlite all          # 两个架构安装并验证 SQLite + VexFS 包
#     bash scripts/release.sh upload v0.1.0            # 推送到 GitHub Release
#
# 凭证: 通过 SERVERS_FILE 环境变量指定凭证文件，或直接覆盖以下 *_HOST/*_USER/*_PASS。
#
# 多版本矩阵 (留空各自回落单版本，向后兼容):
#   DUCKDB_VERSIONS="v1.5.0 v1.5.1 v1.5.2 v1.5.3"   # DuckDB 扩展按版本各编一份
#   PG_VERSIONS="pg16:...:... pg17:...:..."          # PG .so 按版本各编一份
#   先对每个 DuckDB 版本跑: DUCKDB_VERSION=vX.Y.Z bash build_duck.sh setup
#   产物落 dist/<arch>-linux/<dver>/，包名 vexdb-lite-duckdb-<dver>-linux-<arch>.tar.gz

set -euo pipefail
trap 'echo "[release] FAILED at line $LINENO" >&2' ERR

# ============================================================
# 配置
# ============================================================

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# PG source lives inside this repo. Separate PG_VEXDB checkout retired 2026-05-13;
# nested duckdb/ wrapper retired 2026-05-13 — repo root is now flat.
PG_VEXDB_ROOT="${PG_VEXDB_ROOT:-$REPO_ROOT}"
DIST_DIR="$REPO_ROOT/dist"
VEXDB_LITE_RELEASE_VERSION="${VEXDB_LITE_RELEASE_VERSION:-0.1.0-dev}"
SOURCE_COMMIT="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
SOURCE_DIRTY=false
[[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null || true)" ]] && SOURCE_DIRTY=true
SOURCE_REVISION="$SOURCE_COMMIT"
[[ "$SOURCE_DIRTY" == true ]] && SOURCE_REVISION="${SOURCE_REVISION}-dirty"
VEXFS_RUNTIME_HEADER="$REPO_ROOT/agent_files/mount/common/include/vexfs_runtime_types.h"
VEXFS_RUNTIME_ABI="$(sed -n 's/^#define VEXFS_RUNTIME_ABI_VERSION \([0-9][0-9]*\)u$/\1/p' "$VEXFS_RUNTIME_HEADER")"
[[ -n "$VEXFS_RUNTIME_ABI" ]] || { echo "无法读取 VexFS runtime ABI" >&2; exit 1; }

# 远程 GitHub 不可达，cmake / DuckDB src 必须本地下载后 scp
CMAKE_VER="3.28.6"
CMAKE_LINUX_X86="cmake-${CMAKE_VER}-linux-x86_64.tar.gz"
CMAKE_TARBALL="/tmp/${CMAKE_LINUX_X86}"
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VER}/${CMAKE_LINUX_X86}"
REMOTE_DIR="pkgbuild"

# 多 DuckDB 版本矩阵：每条一个 git tag，空格分隔。留空 → 单版本(DUCKDB_VERSION
# 默认 v1.5.2)，完全向后兼容。每版本的本地源码树由 build_duck.sh 放在
# build/duck/<ver>/(与 build_duck.sh 的 DUCK_BASE/<ver> 对齐)。
DUCKDB_VERSION="${DUCKDB_VERSION:-v1.5.2}"
DUCKDB_VERSIONS="${DUCKDB_VERSIONS:-}"
[[ -z "$DUCKDB_VERSIONS" ]] && DUCKDB_VERSIONS="$DUCKDB_VERSION"
# 某个 DuckDB 版本的本地源码树路径。
_duck_src_local() { echo "$REPO_ROOT/build/duck/$1/duckdb_src"; }

# 远程机器配置（Mac bash 3.2 无 assoc 数组，用 prefix 变量）
# x86：CentOS 8 + gcc 8.5（太老）→ 借同事的 gcc 10.3
X86_HOST="${X86_HOST:-}"
X86_USER="${X86_USER:-}"
X86_PASS="${X86_PASS:-}"
X86_PG_CONFIG="${X86_PG_CONFIG:-/opt/pgsql/bin/pg_config}"
X86_BOOST="${X86_BOOST:-/opt/boost_1_90_0}"
X86_GCC_DIR="${X86_GCC_DIR:-/opt/gcc-toolchain/gcc}"

# ARM：Kylin V10 SP1 + gcc 9.3 + cmake 3.16（系统自带够用）
ARM_HOST="${ARM_HOST:-}"
ARM_USER="${ARM_USER:-}"
ARM_PASS="${ARM_PASS:-}"
ARM_PG_CONFIG="${ARM_PG_CONFIG:-/path/to/pg_config}"
ARM_BOOST="${ARM_BOOST:-/path/to/boost}"

# 第一个参数兼容旧用法（直接传 target 隐式为 build）
CMD="${1:-build}"
case "$CMD" in
    build|package|upload|test) shift ;;
    duck|pg|sqlite|all)        CMD="build" ;;
    *)                         CMD="build" ;;
esac
TARGET="${1:-all}"
ARCH="${2:-all}"
RELEASE_DIR="$DIST_DIR/release"
PG_VERSION="${PG_VERSION:-pg19}"

# 多版本 PG 打包：每条 "版本:x86_pg_config:arm_pg_config"，空格分隔多条。
# 留空 → 用上面的单版本(PG_VERSION + X86/ARM_PG_CONFIG)合成一条，完全向后兼容。
PG_VERSIONS="${PG_VERSIONS:-}"
[[ -z "$PG_VERSIONS" ]] && PG_VERSIONS="${PG_VERSION}:${X86_PG_CONFIG}:${ARM_PG_CONFIG}"

# PG smoke(validate_pg)：端口默认 5532；SKIP_PG_SMOKE=1 跳过 in-PG CREATE EXTENSION
# 那段(符号/ELF 检查仍跑)。多版本构建建议设 SKIP_PG_SMOKE=1，除非每版本各有一个
# 在跑的 PG 实例并用 PG_SMOKE_PORT 指定端口。
PG_SMOKE_PORT="${PG_SMOKE_PORT:-5532}"
SKIP_PG_SMOKE="${SKIP_PG_SMOKE:-}"

# ============================================================
# Util
# ============================================================

YEL=$'\033[1;33m'; GRN=$'\033[0;32m'; RED=$'\033[0;31m'; BLU=$'\033[0;34m'; NC=$'\033[0m'
info()  { printf '%s[release]%s %s\n' "$YEL" "$NC" "$*"; }
ok()    { printf '%s[release]%s %s\n' "$GRN" "$NC" "$*"; }
fail()  { printf '%s[release]%s %s\n' "$RED" "$NC" "$*" >&2; exit 1; }
section() { printf '\n%s━━━ %s ━━━%s\n' "$BLU" "$*" "$NC"; }

require_clean_release_source() {
    if [[ "$SOURCE_DIRTY" == true && "${VEXDB_LITE_ALLOW_DIRTY_RELEASE:-0}" != 1 ]]; then
        fail "拒绝从脏工作树生成发行物；仅本地验证可设置 VEXDB_LITE_ALLOW_DIRTY_RELEASE=1"
    fi
}

load_credentials() {
    local arch="${1:-all}"   # 只加载用到的架构凭证
    local f="${SERVERS_FILE:-}"
    if [[ -n "$f" && -f "$f" ]]; then
        [[ -z "$X86_PASS" ]] && X86_PASS="$(awk "/$X86_HOST/,/^---/{if(\$1==\"|\"&&\$2==\"Password\"){print \$4; exit}}" "$f" 2>/dev/null || true)"
        [[ -z "$ARM_PASS" ]] && ARM_PASS="$(awk "/$ARM_HOST/,/^---/{if(\$1==\"|\"&&\$2==\"Password\"){print \$4; exit}}" "$f" 2>/dev/null || true)"
    fi
    [[ "$arch" == "arm" ]] && { [[ -n "$ARM_PASS" ]] || fail "ARM_PASS 未设置；请通过环境变量或 SERVERS_FILE 提供"; }
    [[ "$arch" == "x86" ]] && { [[ -n "$X86_PASS" ]] || fail "X86_PASS 未设置；请通过环境变量或 SERVERS_FILE 提供"; }
    if [[ "$arch" == "all" ]]; then
        [[ -n "$X86_PASS" ]] || fail "X86_PASS 未设置；请通过环境变量或 SERVERS_FILE 提供"
        [[ -n "$ARM_PASS" ]] || fail "ARM_PASS 未设置；请通过环境变量或 SERVERS_FILE 提供"
    fi
}

# 通用 SSH/SCP helper — 第一个参数 x86/arm，复用 ControlMaster 大幅减少建连开销
SSH_OPTS=(-o StrictHostKeyChecking=accept-new
          -o ControlMaster=auto
          -o ControlPersist=600
          -o ControlPath=/tmp/ssh-vexdb-release-%r@%h:%p)

# 给一个 arch 取对应的 host/user/pass（mac bash 3.2 兼容，eval 间接展开）
_host() { eval "echo \$$(echo "$1" | tr a-z A-Z)_HOST"; }
_user() { eval "echo \$$(echo "$1" | tr a-z A-Z)_USER"; }
_pass() { eval "echo \$$(echo "$1" | tr a-z A-Z)_PASS"; }

rssh() {
    local arch=$1; shift
    sshpass -p "$(_pass "$arch")" ssh "${SSH_OPTS[@]}" \
        "$(_user "$arch")@$(_host "$arch")" "$@"
}
rscp_up() {
    local arch=$1; shift
    sshpass -p "$(_pass "$arch")" scp -q "${SSH_OPTS[@]}" "$@" \
        "$(_user "$arch")@$(_host "$arch"):~/"
}
rscp_down() {
    local arch=$1 src=$2 dst=$3
    sshpass -p "$(_pass "$arch")" scp -q "${SSH_OPTS[@]}" \
        "$(_user "$arch")@$(_host "$arch"):$src" "$dst"
}
# rrsync_up: 增量同步本地目录到远程目录,跳过未变文件。SSH_OPTS 拼成
# rsync 的 -e 参数。--delete 让远程严格镜像本地(避免上一轮残留)。
# 用法: rrsync_up <arch> <src> <dst> [extra rsync args, e.g. --exclude=...]
rrsync_up() {
    local arch=$1 src=$2 dst=$3
    shift 3
    local ssh_cmd
    printf -v ssh_cmd "sshpass -p %q ssh" "$(_pass "$arch")"
    for opt in "${SSH_OPTS[@]}"; do ssh_cmd+=" $opt"; done
    rsync -az --delete -e "$ssh_cmd" "$@" "$src/" \
        "$(_user "$arch")@$(_host "$arch"):$dst/"
}

# ============================================================
# Stage
# ============================================================

prepare_local() {
    section "本地准备"
    for cmd in sshpass scp curl tar; do
        command -v "$cmd" >/dev/null || fail "缺少命令: ${cmd}（brew install ${cmd}）"
    done

    [[ -d "$REPO_ROOT" ]] || fail "找不到 vexdb_lite 仓库: $REPO_ROOT"

    load_credentials "$ARCH"
    mkdir -p "$DIST_DIR/x86_64-linux" "$DIST_DIR/aarch64-linux"

    [[ -f "$CMAKE_TARBALL" ]] || {
        info "下载 cmake $CMAKE_VER → $CMAKE_TARBALL"
        curl -sSL --max-time 120 "$CMAKE_URL" -o "$CMAKE_TARBALL" || fail "cmake 下载失败"
    }

    # rsync 增量上传源码:本地 → 远程,跳过未变文件。不再 tar+scp(每次重传几十/几百 MB)。
    command -v rsync >/dev/null || fail "缺少 rsync(brew install rsync)"

    # 每个待构建的 DuckDB 版本都要有本地源码树（build_duck.sh setup 各出一份）。
    if [[ "$TARGET" == "all" || "$TARGET" == "duck" ]]; then
        for dver in $DUCKDB_VERSIONS; do
            local src; src="$(_duck_src_local "$dver")"
            [[ -d "$src" ]] || fail "本地 DuckDB src 不存在: $src (先跑: DUCKDB_VERSION=$dver bash build_duck.sh setup)"
        done
    fi

    ok "本地准备完成"
}

# 这些 rsync exclude 与之前的 tar exclude 等价。--delete 让远程跟本地严格一致。
VEXDB_RSYNC_EXCLUDES=(
    --exclude='build/'      --exclude='.git/'        --exclude='dist/'
    --exclude='.claude/'    --exclude='.specstory/'  --exclude='.local/'
    --exclude='.env'        --exclude='.env.*'       --exclude='credentials.json'
    --exclude='*.duckdb_extension'  --exclude='*.unstripped'
    --exclude='__pycache__' --exclude='*.o' --exclude='*.so' --exclude='*.so.debug'
    # spec test 在 docker 容器以 root 跑, 生成的 .db 文件 root-owned, host UID
    # rsync --delete 删不掉 → 整个 stage 失败. 这些是测试临时文件, 不需同步.
    --exclude='duckdb_unittest_tempdir/'
)
DUCKDB_RSYNC_EXCLUDES=(
    --exclude='.git/' --exclude='.github/'
    # 容器内 spec test 以 root 在 duckdb_src 下生成的临时目录, host UID rsync --delete
    # 删不掉 → 整个 stage 失败 (exit 23). 与 VEXDB_RSYNC_EXCLUDES 同理排除.
    --exclude='duckdb_unittest_tempdir/'
)

# arch 参数化：替代之前的 stage_x86 + stage_arm 两个几乎一样的函数
# 走 rsync 增量同步,首次约 5min,后续 commit 改几行只传 diff(秒级)。
stage() {
    local arch=$1
    section "$arch 同步源码(rsync 增量)"
    rssh "$arch" "mkdir -p ~/$REMOTE_DIR/vexdb_lite"
    rrsync_up "$arch" "$REPO_ROOT" "~/$REMOTE_DIR/vexdb_lite" "${VEXDB_RSYNC_EXCLUDES[@]}"

    if [[ "$arch" == "x86" ]] && ! rssh x86 "[ -x ~/opt/cmake-${CMAKE_VER}-linux-x86_64/bin/cmake ]"; then
        info "x86 部署 portable cmake $CMAKE_VER"
        rscp_up x86 "$CMAKE_TARBALL"
        rssh x86 "mkdir -p ~/opt && cd ~/opt && tar xzf ~/$CMAKE_LINUX_X86 && rm ~/$CMAKE_LINUX_X86"
    fi

    # 每个 DuckDB 版本一棵独立源码树 build/duck/<ver>/duckdb_src。build_duck.sh
    # 用 CMakeLists.txt 存在性判断是否 clone(rsync 已带来→走增量,不重 clone),
    # 版本戳 .vexdb_target_version 随 rsync 同步(不在 exclude)→远程防呆校验放行。
    if [[ "$TARGET" == "all" || "$TARGET" == "duck" ]]; then
        for dver in $DUCKDB_VERSIONS; do
            info "$arch 同步 DuckDB src $dver(rsync 增量)"
            rssh "$arch" "mkdir -p ~/$REMOTE_DIR/vexdb_lite/build/duck/$dver/duckdb_src"
            rrsync_up "$arch" "$(_duck_src_local "$dver")" "~/$REMOTE_DIR/vexdb_lite/build/duck/$dver/duckdb_src" \
                "${DUCKDB_RSYNC_EXCLUDES[@]}"
        done
    fi
    ok "$arch stage 完成"
}

# ============================================================
# Build
# ============================================================

# manylinux_2_28 ABI 标准:产物 libstdc++ GLIBCXX 符号引用 <= GLIBCXX_3.4.22。
# 超过即对外发版不兼容(客户机器旧 GLIBC 跑不起,"libc 版本不支持"报错)。
GLIBCXX_MAX="3.4.22"

# 远程校验 .duckdb_extension 的 GLIBCXX 上限——在 validate_duck 用,产物已在远程。
# 用法: _assert_glibcxx_remote <arch> <remote_ext_path>
_assert_glibcxx_remote() {
    local arch=$1 ext=$2
    info "  $arch GLIBCXX 校验(对外发版兼容性,manylinux_2_28 上限 GLIBCXX_$GLIBCXX_MAX)"
    rssh "$arch" "
        found=\$(strings '$ext' 2>/dev/null | grep -oE 'GLIBCXX_[0-9.]+' | sort -t. -k1n -k2n -k3n | tail -1)
        if [ -z \"\$found\" ]; then
            echo '    无 GLIBCXX_ 符号引用,通过'
            exit 0
        fi
        found_ver=\${found#GLIBCXX_}
        highest=\$(printf '%s\\n%s\\n' '$GLIBCXX_MAX' \"\$found_ver\" | sort -t. -k1n -k2n -k3n | tail -1)
        if [ \"\$highest\" = '$GLIBCXX_MAX' ]; then
            echo \"    产物 \$found <= GLIBCXX_$GLIBCXX_MAX,通过\"
        else
            echo \"    产物 \$found > GLIBCXX_$GLIBCXX_MAX,manylinux_2_28 不兼容\" >&2
            exit 1
        fi
    " || fail "$arch GLIBCXX 校验失败:产物 GLIBCXX 上限超 manylinux_2_28(检查 MANYLINUX=1 是否生效;host gcc 10.3 路径会带 GLIBCXX_3.4.26)"
    ok "  $arch GLIBCXX 校验通过"
}

# 本地校验 .duckdb_extension 的 GLIBCXX 上限——在 cmd_package 用,扫 dist。
# 返回:0 通过,1 不兼容。打印 OK/FAIL 行供调用方汇总。
_assert_glibcxx_local() {
    local f=$1
    local found
    found=$(strings "$f" 2>/dev/null | grep -oE 'GLIBCXX_[0-9.]+' | sort -t. -k1n -k2n -k3n | tail -1)
    if [[ -z "$found" ]]; then
        info "  $(basename "$f"):无 GLIBCXX_ 符号,通过"
        return 0
    fi
    local found_ver="${found#GLIBCXX_}"
    local highest
    highest=$(printf '%s\n%s\n' "$GLIBCXX_MAX" "$found_ver" | sort -t. -k1n -k2n -k3n | tail -1)
    if [[ "$highest" == "$GLIBCXX_MAX" ]]; then
        ok "  $(basename "$f"):$found <= GLIBCXX_$GLIBCXX_MAX"
        return 0
    fi
    echo "[FAIL] $(basename "$f"):$found > GLIBCXX_$GLIBCXX_MAX,manylinux_2_28 不兼容" >&2
    return 1
}

# build_duck <arch> <dver>：构建并拉回某个 DuckDB 版本的扩展。产物按版本落
# dist/<arch>-linux/<dver>/(与 PG 的 $archdir/<pgver>/ 对齐)。
build_duck() {
    local arch=$1 dver=$2 outdir
    [[ "$arch" == "x86" ]] && outdir="$DIST_DIR/x86_64-linux/$dver" || outdir="$DIST_DIR/aarch64-linux/$dver"
    mkdir -p "$outdir"
    local rbuild="build/duck/$dver/build"

    if [[ "${MANYLINUX:-1}" == "1" ]]; then
        section "$arch build DuckDB ext $dver (manylinux container)"
        _build_duck_manylinux "$arch" "$dver" || fail "$arch DuckDB $dver manylinux build 失败"
    else
        section "$arch build DuckDB ext $dver (host gcc)"
        _build_duck_host "$arch" "$dver" || fail "$arch DuckDB $dver host build 失败"
    fi

    validate_duck "$arch" "$dver" || fail "$arch DuckDB $dver smoke 失败"

    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/$rbuild/extension/vexdb_lite/vexdb_lite.duckdb_extension" "$outdir/"
    # Pull the unstripped copy too — kept in dist/<arch>/<dver>/ for our gdb work,
    # excluded from packaging by .gitignore + cmd_package's explicit allowlist.
    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/$rbuild/extension/vexdb_lite/vexdb_lite.duckdb_extension.unstripped" "$outdir/" \
        2>/dev/null || info "  (unstripped 未拉到本地，本机调试时再 scp)"
    ok "$arch DuckDB ext $dver: $(ls -lh "$outdir/vexdb_lite.duckdb_extension" | awk '{print $5}')"
}

# host gcc 10.3 路径——快但产物有 GLIBCXX_3.4.26 依赖，仅 dev / 内部测试用。
# 对外发版必须用 _build_duck_manylinux（MANYLINUX=1 触发）。
_build_duck_host() {
    local arch=$1 dver=$2 env_setup=""
    local rbuild="build/duck/$dver/build"
    if [[ "$arch" == "x86" ]]; then
        env_setup="GCC=$X86_GCC_DIR && \
            export PATH=\$GCC/bin:\$HOME/opt/cmake-${CMAKE_VER}-linux-x86_64/bin:\$PATH && \
            export LD_LIBRARY_PATH=\$GCC/lib64:\${LD_LIBRARY_PATH:-} && \
            export CC=\$GCC/bin/gcc CXX=\$GCC/bin/g++ &&"
    else
        env_setup="export BOOST_INCLUDEDIR=$ARM_BOOST BOOST_ROOT=$ARM_BOOST &&"
    fi

    # TMPDIR redirect: x86 .51 / 卷 40G 不够编 AVX-512 模板 + 链 DuckDB 静态库
    # 的中间产物,挂 "No space left on device"。统一切 /home。详见 build_pg() 注释。
    #
    # 增量 build (CLEAN_DUCK_BUILD 默认 0): 保留 build/duck/<ver>/build, CMake 自动
    # 检测改了的 .cpp 只重编那些, vex extension 改源码后 ~30s 重 link 即可, 不用全量
    # 重编 DuckDB 本体 (~5-8min). 切 toolchain (host gcc <-> manylinux 容器) 时
    # ABI 不兼容会 build fail, 需 CLEAN_DUCK_BUILD=1 强制清.
    # DUCKDB_VERSION=$dver 让 build_duck.sh 走 build/duck/$dver/ 这棵树。
    local clean_step=""
    [[ "${CLEAN_DUCK_BUILD:-0}" == "1" ]] && clean_step="rm -rf $rbuild && \\"$'\n        '
    rssh "$arch" "mkdir -p \$HOME/tmpdir && \
        $env_setup \
        cd ~/$REMOTE_DIR/vexdb_lite && \
        ${clean_step}DUCKDB_VERSION=$dver TMPDIR=\$HOME/tmpdir bash build_duck.sh build" || return 1

    info "$arch strip + reseal footer ($dver)"
    rssh "$arch" "$env_setup \
        cd ~/$REMOTE_DIR/vexdb_lite && \
        DUCKDB_VERSION=$dver TMPDIR=\$HOME/tmpdir UNSTRIPPED_OUT=$rbuild/extension/vexdb_lite/vexdb_lite.duckdb_extension.unstripped \
        bash build_duck.sh strip" || return 1
}

# manylinux_2_28 容器路径——产物 GLIBCXX≤3.4.22，对外发版必走。
#
# Docker rootful on both ends; x86 vmuser 不在 docker 组，命令前缀套
# `echo PASS | sudo -S`。镜像必须在远程预先 docker load 过；ARM 端有
# x86_64 + aarch64 两份，x86 端通常需从 ARM save → mac 中转 → x86 load
# （详见 [[reference_pkg_build]] 镜像就位状态段）。
#
# 容器内步骤（跟历史 ad-hoc 命令对齐 + 合入 build_duck.sh strip）:
#   1. pip 装 cmake==3.29.6（镜像内 cmake 4.x 不兼容 DuckDB v1.5.2
#      link_threads 检查；aliyun 镜像 pypi 拉得快）
#   2. sed 给 build_duck.sh 的 cmake 命令注入 BOOST_ROOT
#   3. rm -rf build/duck/build（host gcc 编出的 .o 跟容器 ABI 不兼容）
#   4. build_duck.sh build → 出含 footer 的 .duckdb_extension
#   5. build_duck.sh strip → strip + 重写 footer（commit f5d62cd31c 后必须
#      走这个，裸 strip 会吃掉 footer）
_build_duck_manylinux() {
    local arch=$1 dver=$2 image boost_src docker_pfx
    local rbuild="build/duck/$dver/build" rsrc="build/duck/$dver/duckdb_src"
    if [[ "$arch" == "x86" ]]; then
        image="quay.io/pypa/manylinux_2_28_x86_64"
        boost_src="$X86_BOOST"
        # x86 docker rootful + vmuser 不在 docker 组 → sudo 喂密码
        docker_pfx="echo '$X86_PASS' | sudo -S docker"
    else
        image="quay.io/pypa/manylinux_2_28_aarch64"
        boost_src="$ARM_BOOST"
        # ARM vexdb 在 docker 组，免 sudo
        docker_pfx="docker"
    fi

    # 镜像存在性预检——避免后面 docker run 失败时 build_duck.sh 报错不清晰
    rssh "$arch" "$docker_pfx image inspect $image >/dev/null 2>&1" \
        || fail "$arch manylinux 镜像 $image 不在；先 docker load 一份"

    # 增量 build (CLEAN_DUCK_BUILD 默认 0): 容器内 manylinux toolchain ABI 稳定,
    # 可复用 build/duck/build/ 的 .o + libduckdb_static.a, 改 vex 源码后 ~30s
    # 重 link 即可. 强制 clean: CLEAN_DUCK_BUILD=1 release.sh build.
    local clean_step_in_container=""
    [[ "${CLEAN_DUCK_BUILD:-0}" == "1" ]] && clean_step_in_container="rm -rf $rbuild"

    # 容器以 root 跑（manylinux 镜像里 pip 走默认 PATH 需要 root；--user 改成
    # host UID 会让 pip 在 $PATH 里失踪）。产物会是 root-owned，容器内最后用
    # chown 改回 host UID/GID，省得 host 上 sshd 用户后续 rscp_down 拿不到。
    local host_uid host_gid
    host_uid=$(rssh "$arch" 'id -u')
    host_gid=$(rssh "$arch" 'id -g')

    rssh "$arch" "$docker_pfx run --rm \
        -v \$HOME/$REMOTE_DIR/vexdb_lite:/work \
        -v $boost_src:/opt/boost:ro \
        -w /work \
        $image \
        bash -c 'set -e
            # manylinux_2_28 镜像把每个 Python ABI 装在 /opt/python/cp*-cp*/bin/
            # (独立解释器,跟 /usr/bin/python3 没共享 site-packages),pip 也在那里。
            # /usr/bin/python3 没装 pip 模块,默认 PATH 也不含 /opt/python/...,
            # 直接 \`pip\` 或 \`python3 -m pip\` 都找不到。把 cp310 加进 PATH 即可。
            export PATH=/opt/python/cp310-cp310/bin:\$PATH
            pip install --quiet -i https://mirrors.aliyun.com/pypi/simple cmake==3.29.6 2>/dev/null || \
              pip install --quiet cmake==3.29.6
            sed -i \"s|-DDUCKDB_EXTENSION_CONFIGS=|-DBOOST_ROOT=/opt/boost -DBoost_NO_BOOST_CMAKE=ON -DDUCKDB_EXTENSION_CONFIGS=|\" build_duck.sh
            # regenerate extension_config_local.cmake with container paths (/work/...)
            # — host (macOS) 上 build_duck.sh setup 生成的版本写死了本地绝对路径
            # (/Users/Four/...), rsync 同步到容器后 cmake add_subdirectory 找不到.
            cat > $rsrc/extension/extension_config_local.cmake <<CFGEOF
duckdb_extension_load(vexdb_lite
    SOURCE_DIR /work/vexdb_duckdb
    INCLUDE_DIR /work/vexdb_duckdb/include
    LOAD_TESTS
)
CFGEOF
            ${clean_step_in_container}
            DUCKDB_VERSION=$dver bash build_duck.sh build
            DUCKDB_VERSION=$dver UNSTRIPPED_OUT=$rbuild/extension/vexdb_lite/vexdb_lite.duckdb_extension.unstripped \
              bash build_duck.sh strip
            chown -R $host_uid:$host_gid $rbuild build_duck.sh
        '" || return 1
}

# DuckDB ext smoke：ELF + 入口符号 + footer 存在 + 真 LOAD 自检。
#
# Footer 检查是 v0.0.3 翻车后加的硬关卡：当时 strip 把 append_metadata.cmake
# 写入的 footer 一起截掉了，结果发出去的 .duckdb_extension 在客户机上 LOAD
# 报 "metadata at the end of the file is invalid"。validate_duck 旧版只看
# ELF + 大小，放过了缺 footer 的产物。现在直接 grep "duckdb_signature"
# magic（append_metadata.cmake 写入的 custom-section 名字），并跑一次真 LOAD。
validate_duck() {
    local arch=$1 dver=$2
    info "$arch DuckDB smoke 验证 ${dver}（ELF + 入口符号 + footer + LOAD）"
    local ext_path="~/$REMOTE_DIR/vexdb_lite/build/duck/$dver/build/extension/vexdb_lite/vexdb_lite.duckdb_extension"
    rssh "$arch" "set -e
        test -f $ext_path
        file $ext_path | grep -q ELF
        nm -D $ext_path 2>/dev/null | grep -qE 'vexdb_lite.*init'
        test \$(stat -c %s $ext_path) -gt 1048576
        # footer canary — append_metadata.cmake 写入的 custom-section 名字
        grep -aq duckdb_signature $ext_path || { echo 'missing footer'; exit 1; }" \
        || { echo "smoke 失败"; return 1; }
    info "  ELF + 入口符号 + footer 通过"

    # 真 LOAD：CLI 必须与 $dver 版本一致——否则 DuckDB 原生 footer 版本校验会拒绝加载
    # (报 "built specifically for DuckDB version ...")，正好反向验证 footer 版本戳正确。
    # 候选(全须是 $dver 版):
    #   1. build/duck/$dver/build/duckdb       ─ BUILD_SHELL=ON 时 build_duck.sh 自编
    #   2. ~/duckdb_cli_<下划线版本> (v1.5.2→1_5_2) ─ 远程预置的官方对应版本 CLI
    #   都没有 → soft skip（footer canary 已是 hard gate，LOAD 是 defense-in-depth）。
    #
    # LD_LIBRARY_PATH 只在 host gcc 路径下需要（产物链 GLIBCXX_3.4.26，要 GCC 10.3 的
    # libstdc++）；manylinux 路径产物本身只要 GLIBCXX≤3.4.22，系统 libstdc++ 就够。
    local ld_setup=""
    if [[ "${MANYLINUX:-1}" != "1" && "$arch" == "x86" ]]; then
        ld_setup="export LD_LIBRARY_PATH=$X86_GCC_DIR/lib64:\${LD_LIBRARY_PATH:-};"
    fi
    local cli_us; cli_us=$(echo "$dver" | sed 's/^v//; s/\./_/g')   # v1.5.2 → 1_5_2 (兼容旧命名)
    rssh "$arch" "set -e
        $ld_setup
        EXT=\$HOME/$REMOTE_DIR/vexdb_lite/build/duck/$dver/build/extension/vexdb_lite/vexdb_lite.duckdb_extension
        CLI=
        for cand in \$HOME/$REMOTE_DIR/vexdb_lite/build/duck/$dver/build/duckdb \$HOME/duckdb_cli_$cli_us; do
            [ -x \"\$cand\" ] && { CLI=\$cand; break; }
        done
        if [ -n \"\$CLI\" ]; then
            echo \"  using CLI: \$CLI\"
            \"\$CLI\" -unsigned -c \"LOAD '\$EXT'; SELECT vexdb_version();\" 2>&1 | tail -3
        else
            echo '  (skip LOAD smoke: 找不到 $dver 版 duckdb CLI)'
        fi" \
        || { echo "LOAD smoke 失败"; return 1; }
    info "  LOAD smoke 通过"

    # GLIBCXX 上限校验:对外发版必须 <= manylinux_2_28 标准(GLIBCXX_3.4.22)。
    # 即使 MANYLINUX=1 走 manylinux 容器,这里再校验一道,挡住未来配置漂移
    # (容器镜像换 / build_duck.sh 路径变化等)。host gcc 10.3 路径产物 GLIBCXX_3.4.26
    # 在这一步会被直接拦截。
    _assert_glibcxx_remote "$arch" "$ext_path" || return 1
}

build_pg() {
    local arch=$1 outdir pg_config boost env_setup=""
    [[ "$arch" == "x86" ]] && { outdir="$DIST_DIR/x86_64-linux/$PG_VERSION"; pg_config="$X86_PG_CONFIG"; boost="$X86_BOOST"; } \
                          || { outdir="$DIST_DIR/aarch64-linux/$PG_VERSION"; pg_config="$ARM_PG_CONFIG"; boost="$ARM_BOOST"; }
    # x86 用 portable cmake + 指定 gcc10.3(同 build_duck);ARM 用 Kylin 系统 cmake(3.16+ 够)。
    if [[ "$arch" == "x86" ]]; then
        env_setup="GCC=$X86_GCC_DIR && \
            export PATH=\$GCC/bin:\$HOME/opt/cmake-${CMAKE_VER}-linux-x86_64/bin:\$PATH && \
            export LD_LIBRARY_PATH=\$GCC/lib64:\${LD_LIBRARY_PATH:-} && \
            export CC=\$GCC/bin/gcc CXX=\$GCC/bin/g++ &&"
    fi
    # 构建产物 vexdb_lite.so 落仓库根,供后续 split-debug / 下载。
    # MANYLINUX=1(默认):在 manylinux_2_28 派生镜像(scripts/docker/Dockerfile.pg-manylinux,
    #   内含 PG 16-19)里编 → 产物 GLIBC_2.17 可移植。MANYLINUX=0:host build(绑构建机 glibc)。
    if [[ "${MANYLINUX:-1}" == "1" ]]; then
        local image pgc docker_pfx host_uid host_gid
        [[ "$arch" == "x86" ]] && { image="vexdb-pg-manylinux:x86_64"; docker_pfx="echo '$X86_PASS' | sudo -S docker"; } \
                              || { image="vexdb-pg-manylinux:aarch64"; docker_pfx="docker"; }
        case "$PG_VERSION" in
            pg16) pgc=/usr/pgsql-16/bin/pg_config ;;
            pg17) pgc=/usr/pgsql-17/bin/pg_config ;;
            pg18) pgc=/usr/pgsql-18/bin/pg_config ;;
            pg19) pgc=/opt/pg19/bin/pg_config ;;
            *)    fail "$PG_VERSION 无对应 manylinux 容器内 pg_config（仅 pg16-19）" ;;
        esac
        section "$arch build vexdb_lite ($PG_VERSION, manylinux 容器)"
        rssh "$arch" "$docker_pfx image inspect $image >/dev/null 2>&1" \
            || fail "$arch manylinux 镜像 $image 不在；先 docker build -f scripts/docker/Dockerfile.pg-manylinux"
        host_uid=$(rssh "$arch" 'id -u'); host_gid=$(rssh "$arch" 'id -g')
        # 并行度按本机算: max(4, min(75% 核数, 可用内存GB/4))。瓶颈是内存——AVX-512
        # dispatcher 等重模板(x86 专属, ARM 无)每个 cc1plus 峰值约 4G, 纯按核数(16 核
        # ×75%=12)在 x86 会撞 OOM(实测 -j8 ~30G 可用就 cc1plus 被 kill)。故 min 内存封顶;
        # 又因 ARM 可用内存紧(~9G)会被封到 -j2 而它本无重文件, 用 -j4 下限兜住(已知安全)。
        local pg_jobs
        pg_jobs=$(rssh "$arch" 'C=$(nproc); MG=$(free -g | sed -n "2p" | tr -s " " | cut -d" " -f7); J=$((C*3/4)); MJ=$((MG/4)); if [ "$MJ" -ge 1 ] && [ "$MJ" -lt "$J" ]; then J=$MJ; fi; if [ "$J" -lt 4 ]; then J=4; fi; echo "$J"')
        info "$arch $PG_VERSION 并行度 -j$pg_jobs(min 75%核 / 可用内存GB/4)"
        rssh "$arch" "$docker_pfx run --rm \
            -v \$HOME/$REMOTE_DIR/vexdb_lite:/work \
            -v $boost:/opt/boost:ro \
            -v \$HOME/.ccache_vexdb:/ccache \
            -e CCACHE_DIR=/ccache \
            -w /work \
            $image \
            bash -c 'set -e
                export PATH=/opt/python/cp310-cp310/bin:\$PATH
                # ccache: 跨 --rm 持久缓存在 host \$HOME/.ccache_vexdb(挂到 /ccache)。
                # 镜像若没自带就装(幂等, 已装则秒过); vexdb_pg/CMakeLists 的
                # find_program(ccache) 据此设 CMAKE_CXX_COMPILER_LAUNCHER。CLEAN_BUILD
                # 清的是 build 目录, ccache 按内容哈希命中, 全量重编也能秒级复用。
                command -v ccache >/dev/null 2>&1 || yum install -y ccache >/dev/null 2>&1 || true
                ${CLEAN_BUILD:+rm -rf build/pg-ml-$PG_VERSION;} mkdir -p build/pg-ml-$PG_VERSION && cd build/pg-ml-$PG_VERSION
                PG_CONFIG=$pgc cmake ../../vexdb_pg -DCMAKE_BUILD_TYPE=Release -DBOOST_FALLBACK_INC=/opt/boost
                # 并行度 \$pg_jobs 由外层按本机核数+可用内存算(见 build_pg 注释), 避免
                # AVX-512/annkmeans 重模板并行编时峰值内存撞 OOM。
                cmake --build . -j$pg_jobs
                cp vexdb_lite.so /work/vexdb_lite.so
                chown -R $host_uid:$host_gid /work/build/pg-ml-$PG_VERSION /work/vexdb_lite.so
            '" || fail "$arch $PG_VERSION manylinux build 失败"
    else
        section "$arch build vexdb_lite ($PG_VERSION, cmake host)"
        rssh "$arch" "mkdir -p \$HOME/tmpdir && \
            $env_setup \
            cd ~/$REMOTE_DIR/vexdb_lite && \
            ${CLEAN_BUILD:+rm -rf build/pg-$PG_VERSION &&} mkdir -p build/pg-$PG_VERSION && cd build/pg-$PG_VERSION && \
            TMPDIR=\$HOME/tmpdir PG_CONFIG=$pg_config cmake ../../vexdb_pg -DCMAKE_BUILD_TYPE=Release -DBOOST_FALLBACK_INC=$boost && \
            TMPDIR=\$HOME/tmpdir cmake --build . -j8 && \
            cp vexdb_lite.so ../../vexdb_lite.so" \
            || fail "$arch PG build 失败"
    fi

    # Split debug: standard GNU binutils workflow (Fedora/Debian also use this
    # for their PG -dbgsym packages). Produces vexdb_lite.so (stripped, light) and
    # vexdb_lite.so.debug (heavy companion). The .debug carries .debug_info etc.
    # and is linked back via .gnu_debuglink so gdb auto-loads it whenever both
    # files sit next to each other (or .debug is placed under
    # /usr/lib/debug/.build-id/ matching the .so's .note.gnu.build-id).
    # 旧 Makefile 的 split-debug target 随 Makefile 删除,这里直接走 binutils 流程。
    info "$arch split-debug"
    rssh "$arch" "cd ~/$REMOTE_DIR/vexdb_lite && \
        objcopy --only-keep-debug vexdb_lite.so vexdb_lite.so.debug && \
        strip --strip-unneeded vexdb_lite.so && \
        objcopy --add-gnu-debuglink=vexdb_lite.so.debug vexdb_lite.so && \
        chmod -x vexdb_lite.so.debug" \
        || fail "$arch split-debug 失败"

    validate_pg "$arch" || fail "$arch PG smoke 失败"

    mkdir -p "$outdir"
    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/vexdb_lite.so" "$outdir/"
    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/vexdb_lite.so.debug" "$outdir/"
    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/vexdb_pg/vexdb_lite.control" "$outdir/"
    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/vexdb_pg/sql/vexdb_lite--1.0.sql" "$outdir/"
}

# PG smoke：替换远程现成 PG 的 .so + CREATE EXTENSION + 最小 ANN 查询。
# 远程 PG 已部署 vexdb_lite，备份原 .so 后用新 .so 覆盖，跑完恢复（避免污染共用环境）。
validate_pg() {
    local arch=$1 pg_config
    [[ "$arch" == "x86" ]] && pg_config="$X86_PG_CONFIG" || pg_config="$ARM_PG_CONFIG"
    info "$arch PG smoke 验证（CREATE EXTENSION + 简单 ANN + build-id + debuglink）"

    rssh "$arch" "set -e
        PG_LIB=\$($pg_config --pkglibdir)
        PG_SHARE=\$($pg_config --sharedir)/extension
        SO=~/$REMOTE_DIR/vexdb_lite/vexdb_lite.so
        SO_DBG=~/$REMOTE_DIR/vexdb_lite/vexdb_lite.so.debug
        # 1. 符号检查：PG 扩展必须有 Pg_magic_func
        nm -D \$SO 2>/dev/null | grep -q Pg_magic_func || { echo 'no Pg_magic_func symbol'; exit 1; }
        # 2. split-debug 卫生检查：build-id + debuglink section + .debug 文件
        readelf -n \$SO 2>/dev/null | grep -q 'Build ID:' || { echo 'no build-id (split-debug 无法定位 .debug)'; exit 1; }
        readelf -S \$SO 2>/dev/null | grep -q '.gnu_debuglink' || { echo 'no .gnu_debuglink section'; exit 1; }
        test -f \$SO_DBG || { echo 'companion .debug missing'; exit 1; }
        readelf -n \$SO_DBG 2>/dev/null | grep -q 'Build ID:' || { echo '.debug missing build-id'; exit 1; }
        # 2. 覆盖部署 + 跑 smoke + 恢复（如果有权限）
        if [ -z "$SKIP_PG_SMOKE" ] && [ -w \"\$PG_LIB/vexdb_lite.so\" ]; then
            cp \"\$PG_LIB/vexdb_lite.so\" \"\$PG_LIB/vexdb_lite.so.bak\"
            cp \$SO \"\$PG_LIB/vexdb_lite.so\"
            trap 'cp \"\$PG_LIB/vexdb_lite.so.bak\" \"\$PG_LIB/vexdb_lite.so\" && rm \"\$PG_LIB/vexdb_lite.so.bak\"' EXIT
            psql -p ${PG_SMOKE_PORT:-5532} -d spec_test -At -c \"DROP TABLE IF EXISTS vex_smoke;
                CREATE TABLE vex_smoke(id INT, v floatvector(3));
                INSERT INTO vex_smoke VALUES (1,'[1,0,0]'),(2,'[0,1,0]'),(3,'[0,0,1]');
                CREATE INDEX ON vex_smoke USING vexdb_graph (v floatvector_l2_ops);
                SELECT id FROM vex_smoke ORDER BY v <-> '[1,0,0]'::floatvector LIMIT 1;
                DROP TABLE vex_smoke;\" 2>&1 | tail -5
        else
            echo '(skip in-PG smoke: no write perm to \$PG_LIB; ABI 符号检查通过)'
        fi"
    ok "$arch vexdb_lite.so: $(ls -lh "$outdir/vexdb_lite.so" | awk '{print $5}')"
}

# SQLite + VexFS Linux 发行物。和 DuckDB/PG 一样在 manylinux_2_28 容器中构建，
# 避免绑定远程构建机的 glibc/libstdc++。fuse3-devel 只用于编译；用户机器运行时
# 需要系统提供 libfuse3.so.3、fusermount3 和 /dev/fuse。
build_sqlite_files() {
    local arch=$1 image docker_pfx boost outarch outdir host_uid host_gid remote_home
    if [[ "$arch" == "x86" ]]; then
        image="quay.io/pypa/manylinux_2_28_x86_64"
        # 不把 sudo 密码拼进远程命令。构建账号必须能直接运行 Docker，或只对
        # Docker 配置免交互 sudo；否则这里明确失败。
        docker_pfx="sudo -n docker"
        boost="$X86_BOOST"
        outarch=x86_64
    else
        image="quay.io/pypa/manylinux_2_28_aarch64"
        docker_pfx="docker"
        boost="$ARM_BOOST"
        outarch=aarch64
    fi
    outdir="$DIST_DIR/${outarch}-linux/sqlite-files"
    mkdir -p "$outdir/bin" "$outdir/lib"

    section "$arch build SQLite + VexFS Files (manylinux container)"
    rssh "$arch" "$docker_pfx image inspect $image >/dev/null 2>&1" \
        || fail "$arch manylinux 镜像 $image 不在"
    host_uid=$(rssh "$arch" 'id -u')
    host_gid=$(rssh "$arch" 'id -g')
    remote_home=$(rssh "$arch" 'printf %s "$HOME"')

    rssh "$arch" "$docker_pfx run --rm \
        -v \$HOME/$REMOTE_DIR/vexdb_lite:/work \
        -v $boost:/opt/boost:ro \
        -w /work \
        $image \
        bash -c 'set -e
            export PATH=/opt/python/cp310-cp310/bin:\$PATH
            dnf install -y fuse3-devel pkgconf-pkg-config >/dev/null
            pip install --quiet -i https://mirrors.aliyun.com/pypi/simple cmake==3.29.6 2>/dev/null || \
              pip install --quiet cmake==3.29.6
            ${CLEAN_SQLITE_BUILD:+rm -rf build/sqlite-files-ml;}
            cmake -S vexdb_sqlite -B build/sqlite-files-ml \
              -DCMAKE_BUILD_TYPE=Release -DVEXDB_SQLITE_BUILD_TESTS=OFF \
              -DVEXDB_BOOST_INC=/opt/boost \
              -DVEXDB_LITE_VERSION=$VEXDB_LITE_RELEASE_VERSION \
              -DVEXDB_SQLITE_GIT_HASH=$SOURCE_REVISION \
              -DCMAKE_EXE_LINKER_FLAGS=\"-static-libstdc++ -static-libgcc\" \
              -DCMAKE_SHARED_LINKER_FLAGS=\"-static-libstdc++ -static-libgcc\"
            cmake --build build/sqlite-files-ml \
              --target vexdb_cli vexfs_fuse vexdb_lite_loadable -j\$(nproc)
            build/sqlite-files-ml/vexdb --version
            DB=\$(mktemp /tmp/vexfs-release-smoke.XXXXXX.sqlite3)
            build/sqlite-files-ml/vexfs-fuse --db \$DB --workspace release --self-test
            rm -f \$DB \$DB-wal \$DB-shm
            chown -R $host_uid:$host_gid build/sqlite-files-ml
        '" || fail "$arch SQLite + VexFS manylinux build 失败"

    local remote_build="$remote_home/$REMOTE_DIR/vexdb_lite/build/sqlite-files-ml"
    for binary in vexdb vexfs-fuse vexdb_lite.so; do
        rssh "$arch" "test -f '$remote_build/$binary'" \
            || fail "$arch SQLite + VexFS 缺少产物 $binary"
        _assert_glibcxx_remote "$arch" "$remote_build/$binary"
    done
    rssh "$arch" "set -e
        file '$remote_build/vexdb' | grep -q ELF
        file '$remote_build/vexfs-fuse' | grep -q ELF
        file '$remote_build/vexdb_lite.so' | grep -q ELF
        ldd '$remote_build/vexfs-fuse' | grep -q 'libfuse3.so.3'
        ! readelf -d '$remote_build/vexdb' | grep -E '(RPATH|RUNPATH)' >/dev/null
        ! readelf -d '$remote_build/vexfs-fuse' | grep -E '(RPATH|RUNPATH)' >/dev/null
    " || fail "$arch SQLite + VexFS ELF/依赖校验失败"

    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/build/sqlite-files-ml/vexdb" "$outdir/bin/vexdb"
    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/build/sqlite-files-ml/vexfs-fuse" "$outdir/bin/vexfs-fuse"
    rscp_down "$arch" "~/$REMOTE_DIR/vexdb_lite/build/sqlite-files-ml/vexdb_lite.so" "$outdir/lib/vexdb_lite.so"
    chmod 0755 "$outdir/bin/vexdb" "$outdir/bin/vexfs-fuse"
    ln -sfn vexdb "$outdir/bin/vexfs"
    install -m 0755 "$REPO_ROOT/agent_files/linux/package/install.sh" "$outdir/install.sh"
    install -m 0755 "$REPO_ROOT/agent_files/linux/package/uninstall.sh" "$outdir/uninstall.sh"
    install -m 0644 "$REPO_ROOT/agent_files/linux/package/使用说明.md" "$outdir/使用说明.md"
    {
        echo "product=vexdb-lite"
        echo "component=sqlite-files"
        echo "version=$VEXDB_LITE_RELEASE_VERSION"
        echo "architecture=$outarch"
        echo "mount_abi=$VEXFS_RUNTIME_ABI"
        echo "source_commit=$SOURCE_COMMIT"
        echo "source_dirty=$SOURCE_DIRTY"
        echo "build_environment=manylinux_2_28"
        echo "runtime_dependency=fuse3"
    } > "$outdir/MANIFEST.txt"
    (
        cd "$outdir"
        shasum -a 256 bin/vexdb bin/vexfs-fuse lib/vexdb_lite.so > SHA256SUMS.txt
    )
    ok "$arch SQLite + VexFS Files 已拉回 $outdir"
}

# ============================================================
# Main
# ============================================================

ARCHES=()
[[ "$ARCH" == "x86" || "$ARCH" == "all" ]] && ARCHES+=(x86)
[[ "$ARCH" == "arm" || "$ARCH" == "all" ]] && ARCHES+=(arm)

cmd_build() {
    info "build 目标=$TARGET 架构=${ARCHES[*]}"
    [[ "$TARGET" =~ ^(duck|pg|sqlite|all)$ ]] || fail "target 必须为 duck/pg/sqlite/all"
    [[ "$ARCH"   =~ ^(x86|arm|all)$ ]] || fail "arch 必须为 x86/arm/all"

    prepare_local

    for arch in "${ARCHES[@]}"; do stage "$arch"; done

    # DuckDB：按 DUCKDB_VERSIONS 逐版本构建（C++ 扩展版本锁定，每版各编一份）。
    if [[ "$TARGET" == "duck" || "$TARGET" == "all" ]]; then
        for dver in $DUCKDB_VERSIONS; do
            section "DuckDB 版本 $dver"
            for arch in "${ARCHES[@]}"; do build_duck "$arch" "$dver"; done
        done
    fi

    # PG：按 PG_VERSIONS 逐版本构建（每条覆盖 PG_VERSION + 该版本的 x86/arm pg_config）。
    if [[ "$TARGET" == "pg" || "$TARGET" == "all" ]]; then
        for entry in $PG_VERSIONS; do
            IFS=: read -r PG_VERSION X86_PG_CONFIG ARM_PG_CONFIG <<<"$entry"
            section "PG 版本 $PG_VERSION (x86:$X86_PG_CONFIG arm:$ARM_PG_CONFIG)"
            for arch in "${ARCHES[@]}"; do build_pg "$arch"; done
        done
    fi

    # SQLite + VexFS：统一 CLI、SQLite loadable extension 和 libfuse3 helper。
    if [[ "$TARGET" == "sqlite" || "$TARGET" == "all" ]]; then
        section "SQLite + VexFS Files"
        for arch in "${ARCHES[@]}"; do build_sqlite_files "$arch"; done
    fi

    section "build 完成"
    find "$DIST_DIR" -maxdepth 4 -type f \( -name "*.duckdb_extension" -o -name "vexdb_lite.so" \
        -o -name "vexdb_lite" -o -name "vexdb" -o -name "vexfs-fuse" \
        -o -name "*.control" -o -name "*.sql" \) -exec ls -lh {} \;
}

# ============================================================
# Package — 打 tarball + SHA256
# ============================================================

cmd_package() {
    section "Package"
    require_clean_release_source

    # Pre-flight: 扫 dist 下所有 .duckdb_extension,校验 GLIBCXX 上限。
    # 即使上游 build 漏了 MANYLINUX=1 / 产物从别处拉来 / validate 被跳过,
    # 这里也挡住——绝不让高 GLIBCXX 产物进 tarball。manylinux_2_28 上限 GLIBCXX_3.4.22。
    info "Package 前置:扫 dist 下 .duckdb_extension 校验 GLIBCXX 上限"
    local pkg_fail=0
    while IFS= read -r ext_file; do
        _assert_glibcxx_local "$ext_file" || pkg_fail=$((pkg_fail+1))
    done < <(find "$DIST_DIR" -maxdepth 3 -name "*.duckdb_extension" -type f 2>/dev/null | grep -v unstripped)
    [[ $pkg_fail -gt 0 ]] && fail "Package 终止:$pkg_fail 个 .duckdb_extension 产物 GLIBCXX 上限 > GLIBCXX_$GLIBCXX_MAX(对外不兼容,用 MANYLINUX=1 重 build)"
    ok "Package 前置:.duckdb_extension 产物 GLIBCXX <= GLIBCXX_$GLIBCXX_MAX"

    mkdir -p "$RELEASE_DIR"
    rm -f "$RELEASE_DIR"/*.tar.gz "$RELEASE_DIR/SHA256SUMS.txt"

    # v0.0.6 踩过坑: macOS 上 BSD tar 默认把扩展属性 (xattr / quarantine /
    # provenance) 落成 `._<filename>` AppleDouble 文件混进 tarball,在
    # macOS 上 `tar -tzf` 又故意把 `._*` 当 xattr 隐藏不显示 → 发版前看
    # 不见,Linux 用户解包后 PG 扫 `*.control` glob 把 `._vexdb_lite.control`
    # 一起读 → "syntax error in file ... near token \"\""。
    # 修法:
    #   1. COPYFILE_DISABLE=1 关掉 macOS 写 AppleDouble 的源头
    #   2. --no-xattrs 强制 tar 不把 xattr 写进归档
    #   3. --exclude='._*' --exclude='.DS_Store' 双保险,挡住目录里残留
    # 防回归: 打完包后用 GNU tar (或 macOS 12+ 的 `tar --strict`)
    # `gtar -tzf` 显式扫一遍,看到 `._*` 直接 fail。
    local TAR_FLAGS=(-czf)
    if tar --no-xattrs --help >/dev/null 2>&1; then
        TAR_FLAGS=(--no-xattrs --exclude='._*' --exclude='.DS_Store' -czf)
    else
        # BSD tar (macOS 默认): 没有 --no-xattrs,靠 COPYFILE_DISABLE + --exclude
        TAR_FLAGS=(--exclude='._*' --exclude='.DS_Store' -czf)
    fi
    export COPYFILE_DISABLE=1

    pkg_tar() {
        local out=$1 archdir=$2; shift 2
        info "打包 $(basename "$out")"
        tar "${TAR_FLAGS[@]}" "$out" -C "$archdir" "$@"
    }

    for arch in x86_64 aarch64; do
        local archdir="$DIST_DIR/${arch}-linux"
        [[ -d "$archdir" ]] || { info "跳过 $arch: $archdir 不存在"; continue; }

        # 打包前清掉源目录里残留的 AppleDouble (防御性,正常情况下不该有)
        find "$archdir" \( -name '._*' -o -name '.DS_Store' \) -delete 2>/dev/null

        # DuckDB：每个版本从 $archdir/<dver> 子目录打包（build_duck 已按版本下载）。
        # C++ 扩展版本锁定，tarball 命名带 DuckDB 版本，客户按自己的 DuckDB 版本下对应包。
        # Duck debug symbols 单独 ship,跟 PG 对齐。Duck 走 unstripped 全量(没做
        # objcopy split-debug),比 PG 的 .so.debug 大但客户没出 crash 用不到。
        for dver in $DUCKDB_VERSIONS; do
            local ddir="$archdir/$dver"
            if [[ -f "$ddir/vexdb_lite.duckdb_extension" ]]; then
                pkg_tar "$RELEASE_DIR/vexdb-lite-duckdb-${dver}-linux-${arch}.tar.gz" "$ddir" \
                    vexdb_lite.duckdb_extension
            fi
            if [[ -f "$ddir/vexdb_lite.duckdb_extension.unstripped" ]]; then
                pkg_tar "$RELEASE_DIR/vexdb-lite-duckdb-${dver}-debugsymbols-linux-${arch}.tar.gz" "$ddir" \
                    vexdb_lite.duckdb_extension.unstripped
            fi
        done

        # PG：每个版本从 $archdir/<版本> 子目录打包（build_pg 已按版本下载到子目录）。
        # PG debug symbols ship as a separate asset (Fedora/Debian convention):
        # customers download the main tarball; on a crash they fetch the matching
        # debugsymbols asset and drop the .debug next to the .so (gdb auto-loads
        # via .gnu_debuglink CRC + .note.gnu.build-id match).
        for entry in $PG_VERSIONS; do
            local pgver="${entry%%:*}" pgdir="$archdir/${entry%%:*}"
            if [[ -f "$pgdir/vexdb_lite.so" ]]; then
                pkg_tar "$RELEASE_DIR/vexdb-lite-${pgver}-linux-${arch}.tar.gz" "$pgdir" \
                    vexdb_lite.so vexdb_lite.control vexdb_lite--1.0.sql
            fi
            if [[ -f "$pgdir/vexdb_lite.so.debug" ]]; then
                pkg_tar "$RELEASE_DIR/vexdb-lite-${pgver}-debugsymbols-linux-${arch}.tar.gz" "$pgdir" \
                    vexdb_lite.so.debug
            fi
        done

        # SQLite + VexFS：统一 CLI、libfuse3 helper、loadable extension 和普通用户安装说明。
        local filesdir="$archdir/sqlite-files"
        if [[ -x "$filesdir/bin/vexdb" && -x "$filesdir/bin/vexfs-fuse" &&
              -f "$filesdir/lib/vexdb_lite.so" ]]; then
            (
                cd "$filesdir"
                shasum -a 256 -c SHA256SUMS.txt >/dev/null
            ) || fail "$arch SQLite + VexFS stage 哈希校验失败"
            pkg_tar "$RELEASE_DIR/vexdb-lite-sqlite-files-linux-${arch}.tar.gz" \
                "$filesdir" bin lib install.sh uninstall.sh 使用说明.md MANIFEST.txt SHA256SUMS.txt
        fi
    done

    # 防回归: 每个 tarball 都扫一遍,有 AppleDouble 直接 fail。
    # macOS BSD tar `tar -tzf` 会隐藏 `._*` (作为 xattr resource fork),
    # 必须用 GNU tar (gtar / Linux tar) 才能看见。优先 gtar,fallback
    # python tarfile (能稳定列出所有 entry,不解释 xattr)。
    info "验证 tarball 不含 AppleDouble (._* / .DS_Store)"
    local verify_cmd=""
    if command -v gtar >/dev/null 2>&1; then
        verify_cmd="gtar -tzf"
    elif tar --version 2>/dev/null | grep -qi "gnu tar"; then
        verify_cmd="tar -tzf"
    fi
    for tb in "$RELEASE_DIR"/*.tar.gz; do
        local bad=""
        if [[ -n "$verify_cmd" ]]; then
            bad=$($verify_cmd "$tb" | grep -E '(^|/)\._|(^|/)\.DS_Store$' || true)
        else
            # Pure-python fallback (macOS 没装 gtar 时)
            bad=$(python3 -c "import tarfile,sys
with tarfile.open(sys.argv[1]) as t:
  for n in t.getnames():
    if '/._' in '/'+n or n.endswith('.DS_Store'):
      print(n)" "$tb")
        fi
        if [[ -n "$bad" ]]; then
            fail "AppleDouble 污染检出 in $(basename "$tb"):"
            printf '  %s\n' $bad >&2
            fail "发版中断: 检查打包目录 / TAR_FLAGS / COPYFILE_DISABLE"
            return 1
        fi
    done

    info "生成 SHA256SUMS"
    (cd "$RELEASE_DIR" && shasum -a 256 *.tar.gz > SHA256SUMS.txt)
    ok "Release assets (AppleDouble check passed):"
    ls -lh "$RELEASE_DIR"
}

# ============================================================
# Test — manylinux 容器内编 unittest + 跑 [spec_run]
# ============================================================
# 必须在 cmd_build(MANYLINUX=1)之后跑:容器内 vex 已编、libduckdb_static.a
# 已有,只需增量编 catch2 + test driver,然后跑 spec_run group。release.sh
# build 阶段的 LOAD smoke 只检 ELF 完整性,不验功能 — 跑完 spec 才能确认发版二
# 进制可用。

cmd_test() {
    # 兼容旧用法 `test x86`（默认跑 DuckDB），同时支持
    # `test sqlite x86` 和 `test all all`。
    local test_target="duck" arch_arg="all"
    if [[ "${1:-}" =~ ^(duck|sqlite|all)$ ]]; then
        test_target="$1"
        arch_arg="${2:-all}"
    else
        arch_arg="${1:-all}"
    fi
    [[ "$arch_arg" =~ ^(x86|arm|all)$ ]] || fail "test arch 必须为 x86/arm/all"

    load_credentials "$arch_arg"

    local arches=()
    [[ "$arch_arg" == "all" || "$arch_arg" == "x86" ]] && arches+=("x86")
    [[ "$arch_arg" == "all" || "$arch_arg" == "arm" ]] && arches+=("arm")

    if [[ "$test_target" == "duck" || "$test_target" == "all" ]]; then
        # 每个 DuckDB 版本各跑一遍 spec（用对应版本的 build/duck/<ver>/build 树）。
        for dver in $DUCKDB_VERSIONS; do
            for arch in "${arches[@]}"; do
                section "$arch spec test $dver (manylinux container)"
                _spec_test_manylinux "$arch" "$dver" || fail "$arch $dver spec test 失败"
            done
        done
        ok "DuckDB spec test 全过"
    fi

    if [[ "$test_target" == "sqlite" || "$test_target" == "all" ]]; then
        for arch in "${arches[@]}"; do
            section "$arch SQLite + VexFS 安装包测试（原生 Linux）"
            _sqlite_files_package_test "$arch" || fail "$arch SQLite + VexFS 安装包测试失败"
        done
        ok "SQLite + VexFS 安装包测试全过"
    fi
}

_sqlite_files_package_test() {
    local arch=$1 outarch pkg pkg_name
    [[ "$arch" == "x86" ]] && outarch="x86_64" || outarch="aarch64"
    pkg="$RELEASE_DIR/vexdb-lite-sqlite-files-linux-${outarch}.tar.gz"
    pkg_name="$(basename "$pkg")"
    [[ -f "$pkg" ]] || fail "找不到 $pkg；请先运行 package"

    # 上传到对应架构的真实 Linux 机器，在全新的 HOME 中走完整安装和卸载流程。
    rscp_up "$arch" "$pkg"
    rssh "$arch" "set -euo pipefail
        package=\"\$HOME/$pkg_name\"
        tmp=\$(mktemp -d \"\$HOME/vexdb-sqlite-files-package-test.XXXXXX\")
        cleanup() { rm -rf \"\$tmp\" \"\$package\"; }
        trap cleanup EXIT
        mkdir -p \"\$tmp/package\" \"\$tmp/home\" \"\$tmp/data\"
        tar xzf \"\$package\" -C \"\$tmp/package\"

        export HOME=\"\$tmp/home\"
        export XDG_DATA_HOME=\"\$tmp/data\"
        export VEXDB_LITE_BIN_DIR=\"\$HOME/.local/bin\"
        export VEXDB_LITE_LIB_DIR=\"\$HOME/.local/lib/vexdb-lite\"
        bash \"\$tmp/package/install.sh\"

        bin=\"\$VEXDB_LITE_BIN_DIR/vexdb\"
        helper=\"\$VEXDB_LITE_BIN_DIR/vexfs-fuse\"
        extension=\"\$VEXDB_LITE_LIB_DIR/vexdb_lite.so\"
        db=\"\$XDG_DATA_HOME/package-test.sqlite3\"
        \"\$bin\" --version
        \"\$bin\" \"\$db\" 'SELECT sqlite_version(), vexdb_version();'
        python3 -c 'import sqlite3,sys; db=sqlite3.connect(\":memory:\"); db.enable_load_extension(True); db.load_extension(sys.argv[1]); print(db.execute(\"SELECT vexdb_version()\").fetchone()[0])' \"\$extension\"
        \"\$helper\" --db \"\$db\" --workspace package-test --self-test
        \"\$bin\" fs --db \"\$db\" mkdir /package-test
        printf 'package-smoke\\n' | \"\$bin\" fs --db \"\$db\" write /package-test/hello.txt
        test \"\$(\"\$bin\" fs --db \"\$db\" cat /package-test/hello.txt)\" = package-smoke

        bash \"\$tmp/package/uninstall.sh\"
        test ! -e \"\$bin\"
        test ! -e \"\$helper\"
        test ! -e \"\$extension\"
        test -f \"\$db\"
    " || return 1
}

_spec_test_manylinux() {
    local arch=$1 dver=$2 image boost_src docker_pfx
    local rbuild="build/duck/$dver/build"
    if [[ "$arch" == "x86" ]]; then
        image="quay.io/pypa/manylinux_2_28_x86_64"
        boost_src="$X86_BOOST"
        docker_pfx="echo '$X86_PASS' | sudo -S docker"
    else
        image="quay.io/pypa/manylinux_2_28_aarch64"
        boost_src="$ARM_BOOST"
        docker_pfx="docker"
    fi

    rssh "$arch" "$docker_pfx image inspect $image >/dev/null 2>&1" \
        || fail "$arch manylinux 镜像 $image 不在；先 docker load 一份"

    # cmake 必须每次容器重装(临时层 pip,Makefile 记的绝对路径会失效)。
    # 清 macOS `._*` xattr shadow 文件,否则 catch2 会注册它们为 test。
    rssh "$arch" "$docker_pfx run --rm \
        -v \$HOME/$REMOTE_DIR/vexdb_lite:/work \
        -v $boost_src:/opt/boost:ro \
        -w /work \
        $image \
        bash -c 'set -e
            PY=\$(ls /opt/python/cp3*/bin/python | head -1)
            \$PY -m pip install --quiet -i https://mirrors.aliyun.com/pypi/simple cmake==3.29.6 2>/dev/null || \
              \$PY -m pip install --quiet cmake==3.29.6
            export PATH=\$(dirname \$PY):\$PATH
            find vexdb_duckdb/test -name \"._*\" -delete 2>/dev/null || true
            cmake --build $rbuild --target unittest -j 8 2>&1 | tail -5
            ./$rbuild/test/unittest \"[spec_run]\"
        '" || return 1
}

# ============================================================
# Upload — gh release create / upload
# ============================================================

cmd_upload() {
    local tag="${1:-}"
    [[ "$SOURCE_DIRTY" == false ]] || fail "拒绝从脏工作树上传 GitHub Release"
    [[ -n "$tag" ]] || fail "upload 必须提供 tag: bash scripts/release.sh upload v0.0.1"
    command -v gh >/dev/null || fail "缺少 gh CLI (brew install gh)"
    [[ -d "$RELEASE_DIR" ]] || fail "$RELEASE_DIR 不存在；先跑 package"
    local assets=("$RELEASE_DIR"/*.tar.gz "$RELEASE_DIR/SHA256SUMS.txt")
    [[ -f "${assets[0]}" ]] || fail "$RELEASE_DIR 里没找到 tarball"

    section "Upload to GitHub Release $tag"
    if gh release view "$tag" >/dev/null 2>&1; then
        # 若设 CLEAN_OLD_ASSETS=1，先删 release 上所有现有 asset 再上传新的
        if [[ "${CLEAN_OLD_ASSETS:-0}" == "1" ]]; then
            info "CLEAN_OLD_ASSETS=1：删 $tag 现有所有 assets"
            local existing
            existing=$(gh release view "$tag" --json assets --jq '.assets[].name')
            if [[ -n "$existing" ]]; then
                while IFS= read -r name; do
                    info "  删 $name"
                    gh release delete-asset "$tag" "$name" -y || true
                done <<< "$existing"
            fi
        else
            info "release $tag 已存在，追加 assets (--clobber 覆盖同名；设 CLEAN_OLD_ASSETS=1 清旧)"
        fi
        gh release upload "$tag" "${assets[@]}" --clobber
    else
        info "新建 release $tag"
        gh release create "$tag" "${assets[@]}" \
            --title "$tag" \
            --notes "Auto-generated release. See README for usage."
    fi
    ok "已上传 $tag"
}

case "$CMD" in
    build)   cmd_build ;;
    package) cmd_package ;;
    upload)  cmd_upload "$@" ;;
    test)    cmd_test "$@" ;;
    *)       fail "unknown command: ${CMD}（build|package|upload|test）" ;;
esac
