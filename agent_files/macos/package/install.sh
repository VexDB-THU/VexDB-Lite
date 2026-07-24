#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP_DIR_OVERRIDE="${VEXDB_LITE_APP_DIR:-${VEXFS_APP_DIR:-}}"
APP_DIR="${APP_DIR_OVERRIDE:-$HOME/Applications}"
BIN_DIR="${VEXDB_LITE_BIN_DIR:-${VEXFS_BIN_DIR:-$HOME/.local/bin}}"
LIB_DIR="${VEXDB_LITE_LIB_DIR:-${VEXFS_LIB_DIR:-$HOME/.local/lib/vexdb-lite}}"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
OFFICIAL_TEAM_ID="BB5VK42K87"
PACKAGE_APP="$ROOT/.payload/VexDB Lite.app"

[ -d "$PACKAGE_APP" ] || { echo "安装包缺少 .payload/VexDB Lite.app" >&2; exit 1; }
[ -x "$ROOT/bin/vexdb" ] || { echo "安装包缺少 bin/vexdb" >&2; exit 1; }
[ -x "$ROOT/bin/vexfs" ] || { echo "安装包缺少 bin/vexfs" >&2; exit 1; }
[ -f "$ROOT/lib/vexdb_lite.dylib" ] || { echo "安装包缺少 SQLite 扩展" >&2; exit 1; }
[ -f "$ROOT/lib/runtime/libpq.5.dylib" ] || { echo "安装包缺少 PostgreSQL runtime" >&2; exit 1; }
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

DOCTOR_CONTRACT_OUTPUT=""
ACTUAL_RUNTIME_ABI=""
for ATTEMPT in 1 2 3; do
    DOCTOR_CONTRACT_OUTPUT="$("$ROOT/bin/vexdb" fs --json doctor 2>&1 || true)"
    ACTUAL_RUNTIME_ABI="$(printf '%s\n' "$DOCTOR_CONTRACT_OUTPUT" \
        | sed -n 's/.*"runtime_abi":\([0-9][0-9]*\).*/\1/p')"
    [ -z "$ACTUAL_RUNTIME_ABI" ] || break
    sleep 1
done
[ -n "$ACTUAL_RUNTIME_ABI" ] && [ "$ACTUAL_RUNTIME_ABI" = "$EXPECTED_RUNTIME_ABI" ] || {
    echo "安装包 runtime ABI 不一致：实际 ${ACTUAL_RUNTIME_ABI:-missing}，清单 ${EXPECTED_RUNTIME_ABI:-missing}" >&2
    [ -z "$DOCTOR_CONTRACT_OUTPUT" ] || printf '%s\n' "$DOCTOR_CONTRACT_OUTPUT" >&2
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

read_code_entitlement() {
    local item="$1" key="$2" entitlement_file value
    entitlement_file="$(mktemp "${TMPDIR:-/tmp}/vexdb-lite-entitlements.XXXXXX")" || return 1
    if ! /usr/bin/codesign -d --entitlements - --xml "$item" \
            >"$entitlement_file" 2>/dev/null; then
        rm -f "$entitlement_file"
        return 1
    fi
    value="$(/usr/libexec/PlistBuddy -c "Print :$key" \
        "$entitlement_file" 2>/dev/null || true)"
    rm -f "$entitlement_file"
    [ -n "$value" ] || return 1
    printf '%s\n' "$value"
}

verify_component "$PACKAGE_APP" --deep
PACKAGE_EXTENSION="$PACKAGE_APP/Contents/Extensions/VexFSAppEx.appex"
verify_component "$PACKAGE_EXTENSION"
verify_component "$ROOT/bin/vexdb"
verify_component "$ROOT/lib/vexdb_lite.dylib"
find "$ROOT/lib/runtime" \
    "$PACKAGE_APP/Contents/Extensions/VexFSAppEx.appex/Contents/Frameworks" \
    -type f -name '*.dylib' -print0 | while IFS= read -r -d '' library; do
        verify_component "$library"
    done

FSKIT_ENTITLEMENT="$(read_code_entitlement "$PACKAGE_EXTENSION" \
    com.apple.developer.fskit.fsmodule || true)"
[ "$FSKIT_ENTITLEMENT" = true ] || {
    echo "安装包 FSKit 扩展缺少 fsmodule entitlement" >&2
    exit 1
}
if [ "$PACKAGE_SIGNATURE" = developer-id ]; then
    EXTENSION_TEAM="$(read_code_entitlement "$PACKAGE_EXTENSION" \
        com.apple.developer.team-identifier || true)"
    EXTENSION_APPLICATION="$(read_code_entitlement "$PACKAGE_EXTENSION" \
        com.apple.application-identifier || true)"
    [ "$EXTENSION_TEAM" = "$EXPECTED_TEAM" ] || {
        echo "安装包 FSKit 扩展 team entitlement 错误：${EXTENSION_TEAM:-missing}" >&2
        exit 1
    }
    [ "$EXTENSION_APPLICATION" = "$EXPECTED_TEAM.io.vexdb.vexfs.extension" ] || {
        echo "安装包 FSKit 扩展 application identifier 错误：${EXTENSION_APPLICATION:-missing}" >&2
        exit 1
    }
fi

verify_installed_mount() {
    local probe_root probe_database probe_mount probe_error attempt actual
    probe_root="$(mktemp -d "${TMPDIR:-/tmp}/vexdb-lite-install-mount.XXXXXX")"
    probe_database="$probe_root/resource/probe.sqlite3"
    probe_mount="$probe_root/mount"
    probe_error="$probe_root/mount-error.log"
    mkdir -p "$probe_root/resource" "$probe_mount"

    cleanup_probe_mount() {
        if "$BIN_DIR/vexdb" fs --db "$probe_database" --workspace install-probe \
                unmount --force "$probe_mount" >/dev/null 2>&1; then
            rm -rf "$probe_root"
            return 0
        fi
        echo "临时 FSKit 验证目录未能卸载，已保留以避免删除挂载内容：$probe_root" >&2
        return 1
    }

    if ! "$BIN_DIR/vexdb" fs --db "$probe_database" \
            --workspace install-probe setup >/dev/null 2>"$probe_error"; then
        cat "$probe_error" >&2
        rm -rf "$probe_root"
        return 1
    fi

    # FSKit can briefly report the newly registered path before ExtensionKit has
    # finished replacing the old helper. A real disposable mount is the only
    # reliable readiness check; doctor intentionally reports registry state only.
    for attempt in 1 2; do
        if "$BIN_DIR/vexdb" fs --db "$probe_database" \
                --workspace install-probe mount "$probe_mount" \
                >/dev/null 2>"$probe_error"; then
            if printf '%s\n' ready-from-fskit >"$probe_mount/ready.txt" 2>>"$probe_error"; then
                actual="$(cat "$probe_mount/ready.txt" 2>>"$probe_error" || true)"
                if [ "$actual" = ready-from-fskit ]; then
                    if "$BIN_DIR/vexdb" fs --db "$probe_database" \
                            --workspace install-probe unmount "$probe_mount" \
                            >/dev/null 2>>"$probe_error"; then
                        rm -rf "$probe_root"
                        return 0
                    fi
                    cat "$probe_error" >&2
                    echo "临时 FSKit 验证目录未能正常卸载，已保留：$probe_root" >&2
                    return 1
                fi
            fi
        fi
        if [ "$attempt" -eq 2 ]; then
            cat "$probe_error" >&2
            cleanup_probe_mount || true
            return 1
        fi
        cleanup_probe_mount || return 1
        # The cleanup removed the disposable database. Recreate it for the next
        # attempt so no state from a failed helper launch can affect the result.
        probe_root="$(mktemp -d "${TMPDIR:-/tmp}/vexdb-lite-install-mount.XXXXXX")"
        probe_database="$probe_root/resource/probe.sqlite3"
        probe_mount="$probe_root/mount"
        probe_error="$probe_root/mount-error.log"
        mkdir -p "$probe_root/resource" "$probe_mount"
        "$BIN_DIR/vexdb" fs --db "$probe_database" --workspace install-probe \
            setup >/dev/null 2>"$probe_error" || {
                cat "$probe_error" >&2
                rm -rf "$probe_root"
                return 1
            }
        sleep $((attempt * 2))
    done

    return 1
}

fskit_mounts_active() {
    local active_mounts
    active_mounts="$(/sbin/mount 2>/dev/null | tr '[:upper:]' '[:lower:]')"
    case "$active_mounts" in
        *"(vexfs,"*|*"(exfat,"*|*"(msdos,"*|*"(ftp,"*|\
        *" type vexfs"*|*" type exfat"*|*" type msdos"*|*" type ftp"*)
            return 0
            ;;
    esac
    return 1
}

restart_current_user_fskit_agent() {
    local current_uid old_pids pid attempt still_running
    if fskit_mounts_active; then
        echo "检测到正在使用的 FSKit 文件系统；拒绝刷新 fskit_agent。" >&2
        return 1
    fi
    current_uid="$(id -u 2>/dev/null || true)"
    [ -n "$current_uid" ] || {
        echo "无法读取当前用户 ID，不能安全刷新 fskit_agent。" >&2
        return 1
    }
    old_pids="$(/usr/bin/pgrep -u "$current_uid" -x fskit_agent 2>/dev/null || true)"
    [ -n "$old_pids" ] || return 0

    # fskit_agent 会忽略 TERM。没有活动 FSKit 卷时用 KILL 结束当前用户的
    # 旧代理，让 launchd 按需拉起一个不含旧 extension UUID 的新进程。
    /usr/bin/pkill -KILL -u "$current_uid" -x fskit_agent >/dev/null 2>&1 || {
        echo "无法刷新当前用户的 fskit_agent。" >&2
        return 1
    }
    for attempt in 1 2 3 4 5; do
        still_running=false
        for pid in $old_pids; do
            if /bin/kill -0 "$pid" >/dev/null 2>&1; then
                still_running=true
                break
            fi
        done
        [ "$still_running" = false ] && return 0
        sleep 0.2
    done
    echo "旧 fskit_agent 没有退出，无法保证扩展身份已经刷新。" >&2
    return 1
}

mkdir -p "$APP_DIR" "$BIN_DIR" "$LIB_DIR"

# Replacing an FSKit module requires restarting the current user's discovery
# agents. Never do that while any known FSKit volume is mounted, because the
# volume may belong to VexFS or to another FSKit implementation such as exFAT.
if [ -z "$APP_DIR_OVERRIDE" ]; then
    if fskit_mounts_active; then
        echo "检测到正在使用的 FSKit 文件系统；请先卸载相关卷，再安装或升级 VexDB-Lite。" >&2
        exit 1
    fi
fi

# 旧 VexFS.app 与新 App 沿用同一个 extension ID。两者同时注册时 macOS 可能继续
# 调用旧扩展，表现为 mount "Loading resource: Input/output error"。正常安装时先
# 撤销旧注册，并在权限允许时把旧 App 放入本次安装事务目录。
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
        LEGACY_BACKUP="$APP_DIR/.VexFS Legacy $(date +%Y%m%d-%H%M%S)-$LEGACY_INDEX.bundle.disabled"
        if mv "$LEGACY_APP" "$LEGACY_BACKUP" 2>/dev/null; then
            echo "旧 VexFS App 已停用并保留：$LEGACY_BACKUP"
        else
            echo "警告：无法移动旧 App，请手动删除：$LEGACY_APP" >&2
        fi
    done
fi

INSTALL_ID="$(date +%Y%m%d-%H%M%S)-$$"
INSTALLED_APP="$APP_DIR/VexDB Lite.app"
TRANSACTION_DIR="$APP_DIR/.vexdb-lite-install-$INSTALL_ID"
STAGED_APP="$TRANSACTION_DIR/new.bundle"
PREVIOUS_APP=""
APP_REPLACED=false
CLI_BACKED_UP=false
LIBRARY_BACKED_UP=false
RUNTIME_BACKED_UP=false
INSTALL_COMMITTED=false
mkdir -p "$TRANSACTION_DIR"

rollback_install() {
    local status="$1"
    set +e
    if [ "$INSTALL_COMMITTED" != true ] && [ "$APP_REPLACED" = true ]; then
        if [ -x "$LSREGISTER" ] && [ -z "$APP_DIR_OVERRIDE" ]; then
            "$LSREGISTER" -u "$INSTALLED_APP" >/dev/null 2>&1 || true
        fi
        rm -rf "$INSTALLED_APP"
        if [ -n "$PREVIOUS_APP" ] && [ -d "$PREVIOUS_APP" ]; then
            mv "$PREVIOUS_APP" "$INSTALLED_APP" >/dev/null 2>&1 || true
            if [ -x "$LSREGISTER" ] && [ -z "$APP_DIR_OVERRIDE" ]; then
                "$LSREGISTER" -f "$INSTALLED_APP" >/dev/null 2>&1 || true
                pluginkit -a "$INSTALLED_APP/Contents/Extensions/VexFSAppEx.appex" \
                    >/dev/null 2>&1 || true
                /usr/bin/open -gj "$INSTALLED_APP" >/dev/null 2>&1 || true
            fi
        fi
        if [ "$CLI_BACKED_UP" = true ]; then
            cp -p "$TRANSACTION_DIR/previous-vexdb" "$BIN_DIR/vexdb" >/dev/null 2>&1 || true
        else
            rm -f "$BIN_DIR/vexdb"
        fi
        if [ "$LIBRARY_BACKED_UP" = true ]; then
            cp -p "$TRANSACTION_DIR/previous-vexdb_lite.dylib" \
                "$LIB_DIR/vexdb_lite.dylib" >/dev/null 2>&1 || true
        else
            rm -f "$LIB_DIR/vexdb_lite.dylib"
        fi
        rm -rf "$LIB_DIR/runtime"
        if [ "$RUNTIME_BACKED_UP" = true ]; then
            ditto "$TRANSACTION_DIR/previous-runtime" "$LIB_DIR/runtime" \
                >/dev/null 2>&1 || true
        fi
        echo "安装失败，已经恢复此前的 VexDB Lite App、CLI 和 runtime。" >&2
    fi
    rm -rf "$STAGED_APP" "$TRANSACTION_DIR"
    return "$status"
}
trap 'rollback_install $?' EXIT

# ditto 会合并已有目录，不能直接覆盖已安装 App。旧 bundle 中多出的文件（例如旧
# embedded.provisionprofile）会留在新签名封装内，导致 codesign 立即失效。先复制到
# 新目录并验签，再整体替换；失败时保留旧 App。
if [ -e "$BIN_DIR/vexdb" ]; then
    cp -p "$BIN_DIR/vexdb" "$TRANSACTION_DIR/previous-vexdb"
    CLI_BACKED_UP=true
fi
if [ -e "$LIB_DIR/vexdb_lite.dylib" ]; then
    cp -p "$LIB_DIR/vexdb_lite.dylib" "$TRANSACTION_DIR/previous-vexdb_lite.dylib"
    LIBRARY_BACKED_UP=true
fi
if [ -d "$LIB_DIR/runtime" ]; then
    ditto "$LIB_DIR/runtime" "$TRANSACTION_DIR/previous-runtime"
    RUNTIME_BACKED_UP=true
fi
ditto "$PACKAGE_APP" "$STAGED_APP"
if ! /usr/bin/codesign --verify --deep --strict "$STAGED_APP"; then
    echo "安装包 App 签名校验失败，未替换当前版本。" >&2
    exit 1
fi
if [ -d "$INSTALLED_APP" ]; then
    # 同 bundle ID 的旧进程仍存活时，open 会只激活旧实例。先结束宿主 App，
    # 确保替换后启动的是新二进制和新的权限说明；FSKit 扩展由系统单独管理。
    if /usr/bin/pgrep -x "VexDB Lite" >/dev/null 2>&1; then
        /usr/bin/pkill -TERM -x "VexDB Lite" >/dev/null 2>&1 || true
        for ATTEMPT in 1 2 3 4 5 6 7 8 9 10; do
            /usr/bin/pgrep -x "VexDB Lite" >/dev/null 2>&1 || break
            sleep 0.1
        done
        /usr/bin/pgrep -x "VexDB Lite" >/dev/null 2>&1 &&
            /usr/bin/pkill -KILL -x "VexDB Lite" >/dev/null 2>&1 || true
    fi
    if [ -x "$LSREGISTER" ] && [ -z "$APP_DIR_OVERRIDE" ]; then
        "$LSREGISTER" -u "$INSTALLED_APP" >/dev/null 2>&1 || true
    fi
    PREVIOUS_APP="$TRANSACTION_DIR/previous.bundle"
    mv "$INSTALLED_APP" "$PREVIOUS_APP"
fi
if ! mv "$STAGED_APP" "$INSTALLED_APP"; then
    [ -n "$PREVIOUS_APP" ] && [ -d "$PREVIOUS_APP" ] && \
        mv "$PREVIOUS_APP" "$INSTALLED_APP" 2>/dev/null || true
    echo "无法替换 VexDB Lite.app，已经尝试恢复旧版本" >&2
    exit 1
fi
APP_REPLACED=true
install -m 0755 "$ROOT/bin/vexdb" "$BIN_DIR/vexdb"
ln -sfn vexdb "$BIN_DIR/vexfs"
install -m 0644 "$ROOT/lib/vexdb_lite.dylib" "$LIB_DIR/vexdb_lite.dylib"
rm -rf "$LIB_DIR/runtime"
mkdir -p "$LIB_DIR/runtime"
find "$ROOT/lib/runtime" -type f -name '*.dylib' -print0 \
    | while IFS= read -r -d '' library; do
        install -m 0755 "$library" "$LIB_DIR/runtime/$(basename "$library")"
    done
verify_component "$INSTALLED_APP" --deep
verify_component "$BIN_DIR/vexdb"
verify_component "$LIB_DIR/vexdb_lite.dylib"
find "$LIB_DIR/runtime" -type f -name '*.dylib' -print0 \
    | while IFS= read -r -d '' library; do
        verify_component "$library"
    done

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
    # Registration is now stable. Refresh only fskit_agent; rotating pkd here
    # can assign another extension UUID after the agent has already cached the
    # previous identity and leads to ExtensionKit error 2 during the first mount.
    restart_current_user_fskit_agent
    sleep 1
    /usr/bin/open -gj "$INSTALLED_APP" >/dev/null 2>&1 || true
    sleep 2

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
        *'"extension_path_matches":true'*)
            if [[ "$EXTENSION_CHECK" == *'"mount_ready":true'* ]]; then
                if ! verify_installed_mount; then
                    echo "FSKit 已注册，但真实临时挂载仍未就绪；请稍后重试安装或运行 vexdb fs doctor。" >&2
                    exit 1
                fi
                echo "FSKit 真实挂载检查通过。"
            else
                case "$EXTENSION_CHECK" in
                    *'"extension":"disabled"'*)
                        echo "FSKit 扩展已安装，但新版本尚未获得系统授权。"
                        echo "请在系统设置 → 通用 → 登录项与扩展 → 文件系统扩展中启用 VexDB Lite，然后重新运行："
                        echo "  $BIN_DIR/vexdb fs doctor"
                        ;;
                    *)
                        echo "FSKit 扩展路径正确，但系统服务尚未准备好；请稍后运行 $BIN_DIR/vexdb fs doctor。"
                        ;;
                esac
            fi
            ;;
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

# 只有所有复制、签名和注册检查都完成后才提交事务。旧版本不作为长期备份
# 留在 Applications 中，否则 LaunchServices 仍会把 `.app.disabled` 当成候选 App。
INSTALL_COMMITTED=true
rm -rf "$TRANSACTION_DIR"
if [ -z "$APP_DIR_OVERRIDE" ]; then
    for STALE_BACKUP in \
        "$APP_DIR"/VexDB\ Lite\ Previous\ *.app.disabled \
        "$APP_DIR"/VexDB\ Lite\ Failed\ *.app.disabled; do
        [ -d "$STALE_BACKUP" ] || continue
        if [ -x "$LSREGISTER" ]; then
            "$LSREGISTER" -u "$STALE_BACKUP" >/dev/null 2>&1 || true
        fi
        rm -rf "$STALE_BACKUP"
    done
fi
trap - EXIT

echo "VexDB-Lite 已安装："
echo "  App: $APP_DIR/VexDB Lite.app"
echo "  CLI: $BIN_DIR/vexdb"
echo "  文件快捷命令: $BIN_DIR/vexfs"
echo "  SQLite: $LIB_DIR/vexdb_lite.dylib"
echo "  PostgreSQL runtime: $LIB_DIR/runtime/"
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
