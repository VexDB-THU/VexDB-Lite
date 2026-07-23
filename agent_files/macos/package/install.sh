#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP_DIR_OVERRIDE="${VEXDB_LITE_APP_DIR:-${VEXFS_APP_DIR:-}}"
APP_DIR="${APP_DIR_OVERRIDE:-$HOME/Applications}"
BIN_DIR="${VEXDB_LITE_BIN_DIR:-${VEXFS_BIN_DIR:-$HOME/.local/bin}}"
LIB_DIR="${VEXDB_LITE_LIB_DIR:-${VEXFS_LIB_DIR:-$HOME/.local/lib/vexdb-lite}}"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
OFFICIAL_TEAM_ID="BB5VK42K87"

[ -d "$ROOT/VexDB Lite.app" ] || { echo "安装包缺少 VexDB Lite.app" >&2; exit 1; }
[ -x "$ROOT/bin/vexdb" ] || { echo "安装包缺少 bin/vexdb" >&2; exit 1; }
[ -x "$ROOT/bin/vexfs" ] || { echo "安装包缺少 bin/vexfs" >&2; exit 1; }
[ -f "$ROOT/lib/vexdb_lite.dylib" ] || { echo "安装包缺少 SQLite 扩展" >&2; exit 1; }
[ -f "$ROOT/MANIFEST.txt" ] || { echo "安装包缺少 MANIFEST.txt" >&2; exit 1; }
[ -f "$ROOT/SHA256SUMS.txt" ] || { echo "安装包缺少 SHA256SUMS.txt" >&2; exit 1; }

(cd "$ROOT" && /usr/bin/shasum -a 256 -c SHA256SUMS.txt >/dev/null) || {
    echo "安装包文件哈希校验失败" >&2
    exit 1
}

PACKAGE_SIGNATURE="$(sed -n 's/^signature=//p' "$ROOT/MANIFEST.txt")"
EXPECTED_TEAM="$(sed -n 's/^signing_team=//p' "$ROOT/MANIFEST.txt")"
EXPECTED_CONTRACT="$(sed -n 's/^contract_version=//p' "$ROOT/MANIFEST.txt")"
EXPECTED_RUNTIME_ABI="$(sed -n 's/^mount_abi_version=//p' "$ROOT/MANIFEST.txt")"
SOURCE_DIRTY="$(sed -n 's/^source_dirty=//p' "$ROOT/MANIFEST.txt")"
case "$PACKAGE_SIGNATURE" in
    developer-id)
        [ "$EXPECTED_TEAM" = "$OFFICIAL_TEAM_ID" ] || {
            echo "Developer ID 包发布者无效：${EXPECTED_TEAM:-missing}" >&2
            exit 1
        }
        if [ "$SOURCE_DIRTY" != false ]; then
            [ "${VEXDB_LITE_ALLOW_DIRTY_INSTALL:-0}" = 1 ] || {
                echo "拒绝安装来自脏工作树的 Developer ID 包" >&2
                exit 1
            }
            echo "警告：正在安装脏源码测试包；此开关不得用于正式发布。" >&2
        fi
        ;;
    ad-hoc)
        [ "${VEXDB_LITE_ALLOW_ADHOC_INSTALL:-0}" = 1 ] || {
            echo "ad-hoc 包仅供本地测试；如确需安装，请设置 VEXDB_LITE_ALLOW_ADHOC_INSTALL=1" >&2
            exit 1
        }
        ;;
    *) echo "安装包签名类型无效：${PACKAGE_SIGNATURE:-missing}" >&2; exit 1 ;;
esac

ACTUAL_RUNTIME_ABI="$("$ROOT/bin/vexdb" fs --json doctor 2>/dev/null \
    | sed -n 's/.*"runtime_abi":\([0-9][0-9]*\).*/\1/p' || true)"
[ -n "$ACTUAL_RUNTIME_ABI" ] && [ "$ACTUAL_RUNTIME_ABI" = "$EXPECTED_RUNTIME_ABI" ] || {
    echo "安装包 runtime ABI 不一致：实际 ${ACTUAL_RUNTIME_ABI:-missing}，清单 ${EXPECTED_RUNTIME_ABI:-missing}" >&2
    exit 1
}
ACTUAL_CONTRACT="$("$ROOT/bin/vexdb" :memory: "SELECT vexfs_contract_version();" 2>/dev/null || true)"
[ -n "$ACTUAL_CONTRACT" ] && [ "$ACTUAL_CONTRACT" = "$EXPECTED_CONTRACT" ] || {
    echo "安装包文件合同不一致：实际 ${ACTUAL_CONTRACT:-missing}，清单 ${EXPECTED_CONTRACT:-missing}" >&2
    exit 1
}

verify_component() {
    local item="$1"
    shift
    /usr/bin/codesign --verify --strict "$@" "$item" || {
        echo "安装包签名校验失败：$item" >&2
        exit 1
    }
    if [ "$PACKAGE_SIGNATURE" = developer-id ]; then
        local actual
        actual="$(/usr/bin/codesign -d --verbose=4 "$item" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
        [ "$actual" = "$EXPECTED_TEAM" ] || {
            echo "安装包发布者错误：$item（实际 ${actual:-none}，期望 $EXPECTED_TEAM）" >&2
            exit 1
        }
    fi
}

verify_component "$ROOT/VexDB Lite.app" --deep
verify_component "$ROOT/VexDB Lite.app/Contents/Extensions/VexFSAppEx.appex"
verify_component "$ROOT/bin/vexdb"
verify_component "$ROOT/lib/vexdb_lite.dylib"

mkdir -p "$APP_DIR" "$BIN_DIR" "$LIB_DIR"

# 旧 VexFS.app 与新 App 沿用同一个 extension ID。两者同时注册时 macOS 可能继续
# 调用旧扩展，表现为 mount "Loading resource: Input/output error"。正常安装时先
# 撤销旧注册，并在权限允许时把旧 App 改成不会被 LaunchServices 扫描的备份。
# 自定义 APP_DIR 用于测试或集成，不触碰系统中已有的 App 注册。
LEGACY_FOUND=false
LEGACY_INDEX=0
if [ -z "$APP_DIR_OVERRIDE" ]; then
    for LEGACY_APP in "$HOME/Applications/VexFS.app" "/Applications/VexFS.app"; do
        [ -d "$LEGACY_APP" ] || continue
        LEGACY_FOUND=true
        LEGACY_INDEX=$((LEGACY_INDEX + 1))
        if [ -x "$LSREGISTER" ]; then
            "$LSREGISTER" -u "$LEGACY_APP" >/dev/null 2>&1 || true
        fi
        LEGACY_BACKUP="$APP_DIR/VexFS Legacy $(date +%Y%m%d-%H%M%S)-$LEGACY_INDEX.app.disabled"
        if mv "$LEGACY_APP" "$LEGACY_BACKUP" 2>/dev/null; then
            echo "旧 VexFS App 已停用并保留：$LEGACY_BACKUP"
        else
            echo "警告：无法移动旧 App，请手动删除：$LEGACY_APP" >&2
        fi
    done
fi

INSTALL_ID="$(date +%Y%m%d-%H%M%S)-$$"
INSTALLED_APP="$APP_DIR/VexDB Lite.app"
STAGED_APP="$APP_DIR/.VexDB Lite $INSTALL_ID.app"
PREVIOUS_APP=""

# ditto 会合并已有目录，不能直接覆盖已安装 App。旧 bundle 中多出的文件（例如旧
# embedded.provisionprofile）会留在新签名封装内，导致 codesign 立即失效。先复制到
# 新目录并验签，再整体替换；失败时保留旧 App。
ditto "$ROOT/VexDB Lite.app" "$STAGED_APP"
if ! /usr/bin/codesign --verify --deep --strict "$STAGED_APP"; then
    FAILED_APP="$APP_DIR/VexDB Lite Failed $INSTALL_ID.app.disabled"
    mv "$STAGED_APP" "$FAILED_APP" 2>/dev/null || true
    echo "安装包 App 签名校验失败，未替换当前版本：$FAILED_APP" >&2
    exit 1
fi
if [ -d "$INSTALLED_APP" ]; then
    if [ -x "$LSREGISTER" ] && [ -z "$APP_DIR_OVERRIDE" ]; then
        "$LSREGISTER" -u "$INSTALLED_APP" >/dev/null 2>&1 || true
    fi
    PREVIOUS_APP="$APP_DIR/VexDB Lite Previous $INSTALL_ID.app.disabled"
    mv "$INSTALLED_APP" "$PREVIOUS_APP"
fi
if ! mv "$STAGED_APP" "$INSTALLED_APP"; then
    [ -n "$PREVIOUS_APP" ] && [ -d "$PREVIOUS_APP" ] && \
        mv "$PREVIOUS_APP" "$INSTALLED_APP" 2>/dev/null || true
    echo "无法替换 VexDB Lite.app，已经尝试恢复旧版本" >&2
    exit 1
fi
install -m 0755 "$ROOT/bin/vexdb" "$BIN_DIR/vexdb"
ln -sfn vexdb "$BIN_DIR/vexfs"
install -m 0644 "$ROOT/lib/vexdb_lite.dylib" "$LIB_DIR/vexdb_lite.dylib"

if [ -z "$APP_DIR_OVERRIDE" ]; then
    INSTALLED_EXTENSION="$INSTALLED_APP/Contents/Extensions/VexFSAppEx.appex"
    if [ -x "$LSREGISTER" ]; then
        "$LSREGISTER" -f "$INSTALLED_APP" >/dev/null 2>&1 || true
    fi
    if command -v pluginkit >/dev/null; then
        pluginkit -a "$INSTALLED_EXTENSION" >/dev/null 2>&1 || true
    fi
    # ExtensionKit may not publish a replaced FSKit module until its containing
    # App has been launched once. Launch it hidden so doctor does not keep
    # reporting `extension: missing` after a valid upgrade.
    /usr/bin/open -gj "$INSTALLED_APP" >/dev/null 2>&1 || true
    MOUNTS="$(/sbin/mount 2>/dev/null | tr '[:upper:]' '[:lower:]')"
    case "$MOUNTS" in
        *"(vexfs,"*|*"(exfat,"*|*"(msdos,"*|*" type vexfs"*|*" type exfat"*|*" type msdos"*) ;;
        *) pkill -KILL -x fskit_agent >/dev/null 2>&1 || true ;;
    esac

    # enabled 只表示同 bundle ID 的扩展已经获准使用，不保证 FSKit 当前解析到
    # 新安装的 App。用签名 CLI 读取 FSKit 的真实 module URL，避免安装成功但
    # mount 实际卡在 Xcode archive 或旧副本上。
    EXTENSION_CHECK=""
    for ATTEMPT in 1 2 3 4 5; do
        EXTENSION_CHECK="$(VEXFS_EXPECTED_EXTENSION_PATH="$INSTALLED_EXTENSION" \
            "$BIN_DIR/vexdb" fs --json doctor 2>/dev/null || true)"
        case "$EXTENSION_CHECK" in
            *'"extension_path_matches":true'*) break ;;
        esac
        "$LSREGISTER" -f "$INSTALLED_APP" >/dev/null 2>&1 || true
        pluginkit -a "$INSTALLED_EXTENSION" >/dev/null 2>&1 || true
        sleep 1
    done
    case "$EXTENSION_CHECK" in
        *'"extension_path_matches":true'*) ;;
        *'"extension_path":""'*)
            echo "警告：系统尚未返回 FSKit module 路径；启用扩展后请运行 vexdb fs doctor。" >&2
            ;;
        *)
            echo "FSKit 当前没有加载新安装的扩展，安装未完成：" >&2
            echo "$EXTENSION_CHECK" >&2
            echo "期望路径：$INSTALLED_EXTENSION" >&2
            exit 1
            ;;
    esac
fi

echo "VexDB-Lite 已安装："
echo "  App: $APP_DIR/VexDB Lite.app"
echo "  CLI: $BIN_DIR/vexdb"
echo "  文件快捷命令: $BIN_DIR/vexfs"
echo "  SQLite: $LIB_DIR/vexdb_lite.dylib"
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *)
        echo ""
        echo "请把下面一行加入 ~/.zshrc："
        echo "  export PATH=\"$BIN_DIR:\$PATH\""
        ;;
esac
echo ""
echo "下一步：$BIN_DIR/vexdb --version && $BIN_DIR/vexdb fs setup"
