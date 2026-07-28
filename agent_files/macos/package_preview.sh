#!/bin/bash
set -euo pipefail

# 本机预览：bash agent_files/macos/package_preview.sh
# Developer ID：VEXDB_LITE_SIGN_MODE=developer-id bash agent_files/macos/package_preview.sh
# 签名并公证：VEXDB_LITE_SIGN_MODE=developer-id VEXDB_LITE_NOTARY_PROFILE=vexdb-lite-notary bash agent_files/macos/package_preview.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SQLITE_DIR="$ROOT/vexdb_sqlite"
PACKAGE_DIR="$SCRIPT_DIR/package"
RUNTIME_HEADER="$ROOT/agent_files/mount/common/include/vexfs_runtime_types.h"
RUNTIME_ABI="$(sed -n 's/^#define VEXFS_RUNTIME_ABI_VERSION \([0-9][0-9]*\)u$/\1/p' "$RUNTIME_HEADER")"
[ -n "$RUNTIME_ABI" ] || { echo "无法从 $RUNTIME_HEADER 读取 runtime ABI" >&2; exit 1; }

VERSION="${VEXDB_LITE_PREVIEW_VERSION:-${VEXFS_PREVIEW_VERSION:-0.1.0-preview.1}}"
ARCH="${VEXDB_LITE_ARCH:-${VEXFS_ARCH:-arm64}}"
MINIMUM_MACOS="${VEXDB_LITE_MINIMUM_MACOS:-13.0}"
FSKIT_MINIMUM_MACOS=26.0
SIGN_MODE="${VEXDB_LITE_SIGN_MODE:-${VEXFS_SIGN_MODE:-ad-hoc}}"
TEAM_ID="${VEXDB_LITE_TEAM_ID:-${VEXFS_TEAM_ID:-BB5VK42K87}}"
EXPORT_OPTIONS="${VEXDB_LITE_EXPORT_OPTIONS:-${VEXFS_EXPORT_OPTIONS:-$SCRIPT_DIR/ExportOptions-DeveloperID.plist}}"
NOTARY_PROFILE="${VEXDB_LITE_NOTARY_PROFILE:-${VEXFS_NOTARY_PROFILE:-}}"
GIT_COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_BRANCH="$(git -C "$ROOT" branch --show-current 2>/dev/null || echo unknown)"
if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null || true)" ]; then
    GIT_DIRTY=true
else
    GIT_DIRTY=false
fi
case "$VERSION" in
    *[!A-Za-z0-9._-]*) echo "版本只能包含字母、数字、点、下划线和连字符" >&2; exit 2 ;;
esac
if [[ ! "$MINIMUM_MACOS" =~ ^[0-9]+\.[0-9]+$ ]]; then
    echo "VEXDB_LITE_MINIMUM_MACOS 必须是 major.minor：$MINIMUM_MACOS" >&2
    exit 2
fi
BUNDLE_MARKETING_VERSION="${VEXDB_LITE_BUNDLE_MARKETING_VERSION:-${VERSION%%-*}}"
BUNDLE_BUILD_VERSION="${VEXDB_LITE_BUNDLE_BUILD_VERSION:-}"
if [ -z "$BUNDLE_BUILD_VERSION" ]; then
    preview_number="${VERSION##*.}"
    case "$VERSION:$preview_number" in
        *-preview.*:[0-9]*) BUNDLE_BUILD_VERSION="$preview_number" ;;
        *) BUNDLE_BUILD_VERSION="$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 1)" ;;
    esac
fi
if [[ ! "$BUNDLE_MARKETING_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Bundle marketing version 必须是数字三段式：$BUNDLE_MARKETING_VERSION" >&2
    exit 2
fi
case "$BUNDLE_BUILD_VERSION" in
    ''|*[!0-9]*|0) echo "Bundle build version 必须是正整数：$BUNDLE_BUILD_VERSION" >&2; exit 2 ;;
esac
case "$ARCH" in
    arm64|x86_64) ;;
    *) echo "VEXDB_LITE_ARCH 只能是 arm64 或 x86_64" >&2; exit 2 ;;
esac
case "$SIGN_MODE" in
    ad-hoc|developer-id) ;;
    *) echo "VEXDB_LITE_SIGN_MODE 只能是 ad-hoc 或 developer-id" >&2; exit 2 ;;
esac
if [ "$SIGN_MODE" = developer-id ]; then
    [ -n "$TEAM_ID" ] || { echo "Developer ID 签名需要 VEXDB_LITE_TEAM_ID" >&2; exit 2; }
    [ -f "$EXPORT_OPTIONS" ] || { echo "找不到导出配置：$EXPORT_OPTIONS" >&2; exit 1; }
    command -v openssl >/dev/null || { echo "Developer ID 签名需要 openssl" >&2; exit 1; }
    if [ "$GIT_DIRTY" = true ] && [ -n "$NOTARY_PROFILE" ]; then
        echo "拒绝公证脏工作树：请先提交本次源码，确保公证包可追溯" >&2
        exit 1
    fi
    if [ "$GIT_DIRTY" = true ] && [ "${VEXDB_LITE_ALLOW_DIRTY_SIGNING:-0}" != 1 ]; then
        echo "拒绝签名脏工作树；仅本地验证可显式设置 VEXDB_LITE_ALLOW_DIRTY_SIGNING=1" >&2
        exit 1
    fi
    if [ "$GIT_DIRTY" = true ]; then
        export VEXDB_LITE_ALLOW_DIRTY_PACKAGE_TEST=1
    fi
fi

CMAKE_BIN="$(command -v cmake || true)"
[ -x "$CMAKE_BIN" ] || CMAKE_BIN=/opt/homebrew/bin/cmake
[ -x "$CMAKE_BIN" ] || CMAKE_BIN=/usr/local/bin/cmake
[ -x "$CMAKE_BIN" ] || { echo "找不到 cmake" >&2; exit 1; }
command -v xcodebuild >/dev/null || { echo "找不到 xcodebuild" >&2; exit 1; }
command -v codesign >/dev/null || { echo "找不到 codesign" >&2; exit 1; }

PYTHON_BIN="${VEXDB_LITE_PYTHON:-}"
if [ -n "$PYTHON_BIN" ]; then
    [ -x "$PYTHON_BIN" ] || { echo "VEXDB_LITE_PYTHON 不可执行：$PYTHON_BIN" >&2; exit 1; }
    "$PYTHON_BIN" -c 'import sys; raise SystemExit(sys.version_info < (3, 8))' || {
        echo "VEXDB_LITE_PYTHON 需要 Python 3.8 或更高版本：$PYTHON_BIN" >&2
        exit 1
    }
else
    for candidate in \
        "$(command -v python3 2>/dev/null || true)" \
        /opt/anaconda3/bin/python3 \
        /opt/homebrew/bin/python3 \
        /usr/bin/python3; do
        [ -n "$candidate" ] && [ -x "$candidate" ] || continue
        if "$candidate" -c 'import sys; raise SystemExit(sys.version_info < (3, 8))' \
                >/dev/null 2>&1; then
            PYTHON_BIN="$candidate"
            break
        fi
    done
    [ -n "$PYTHON_BIN" ] || { echo "打包测试需要 Python 3.8 或更高版本" >&2; exit 1; }
fi

NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
BUILD_JOBS="${VEXDB_LITE_BUILD_JOBS:-$NCPU}"
XCODE_JOBS="${VEXDB_LITE_XCODE_JOBS:-2}"
FSKIT_SETTLE_SECONDS="${VEXDB_LITE_FSKIT_SETTLE_SECONDS:-20}"
case "$BUILD_JOBS:$XCODE_JOBS" in
    *[!0-9:]*|0:*|*:0) echo "构建并行度必须是正整数" >&2; exit 2 ;;
esac
case "$FSKIT_SETTLE_SECONDS" in
    ''|*[!0-9]*) echo "FSKit 收尾等待秒数必须是非负整数" >&2; exit 2 ;;
esac
# 打包是交付步骤，不需要占满开发机。默认最多四个 C/C++ 编译任务，Xcode 最多两个。
[ "$BUILD_JOBS" -le 4 ] || BUILD_JOBS=4
BUILD_DIR="$SQLITE_DIR/build-vexdb-lite-package-$ARCH"
DERIVED_DATA="$BUILD_DIR/xcode-derived-data"
ARCHIVE_PATH="$BUILD_DIR/VexDB-Lite.xcarchive"
EXPORT_DIR="$BUILD_DIR/developer-id-export"
DIST_DIR="$ROOT/dist/vexdb-lite"
PACKAGE_NAME="vexdb-lite-${VERSION}-macos-${ARCH}"
STAGE="$DIST_DIR/$PACKAGE_NAME"
ZIP="$DIST_DIR/$PACKAGE_NAME.zip"
GUIDE="$DIST_DIR/$PACKAGE_NAME-使用说明.md"
APP_NAME="VexDB Lite.app"
PAYLOAD_RELATIVE=".payload/$APP_NAME"
PAYLOAD_APP="$STAGE/$PAYLOAD_RELATIVE"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
FSKIT_RESTORE_REQUIRED=false

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
    [ -n "$current_uid" ] || return 1
    old_pids="$(/usr/bin/pgrep -u "$current_uid" -x fskit_agent 2>/dev/null || true)"
    [ -n "$old_pids" ] || return 0
    /usr/bin/pkill -KILL -u "$current_uid" -x fskit_agent >/dev/null 2>&1 || return 1
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
    return 1
}

unregister_temporary_fskit_apps() {
    [ -x "$LSREGISTER" ] || return 0
    local app
    for app in \
        "$DERIVED_DATA/Build/Products/Release/$APP_NAME" \
        "$ARCHIVE_PATH/Products/Applications/$APP_NAME" \
        "$EXPORT_DIR/$APP_NAME" \
        "$DERIVED_DATA/Build/Intermediates.noindex/ArchiveIntermediates/VexFSApp/InstallationBuildProductsLocation/Applications/$APP_NAME" \
        "$DERIVED_DATA/Build/Intermediates.noindex/ArchiveIntermediates/VexFSApp/BuildProductsPath/Release/$APP_NAME"; do
        [ -d "$app" ] || continue
        # Xcode 会自动登记这些构建 App；必须在删除目录前撤销。重复撤销只
        # 作用于当前仍真实存在的确定构建路径，
        # 不会清除用户已经批准的 extension 启用状态。
        "$LSREGISTER" -u "$app" >/dev/null 2>&1 || true
        FSKIT_RESTORE_REQUIRED=true
    done
}

restore_installed_fskit_app() {
    [ -x "$LSREGISTER" ] || return 0
    local installed=""
    local candidate
    for candidate in \
        "$HOME/Applications/$APP_NAME" \
        "/Applications/$APP_NAME"; do
        if [ -d "$candidate" ]; then
            installed="$candidate"
            break
        fi
    done
    [ -n "$installed" ] || return 0
    "$LSREGISTER" -f "$installed" >/dev/null 2>&1 || true
    if command -v pluginkit >/dev/null; then
        pluginkit -a "$installed/Contents/Extensions/VexFSAppEx.appex" >/dev/null 2>&1 || true
    fi
    # Xcode may rotate the registered extension UUID while temporary products
    # are removed. Refresh the idle per-user FSKit agent only after registration
    # is stable; do not rotate pkd again and create another identity.
    restart_current_user_fskit_agent || {
        echo "无法刷新当前用户的 fskit_agent" >&2
        return 1
    }
    sleep 1
    /usr/bin/open -gj "$installed" >/dev/null 2>&1 || true
    sleep 2

    local state plugin_state doctor_cli candidate attempt healthy
    doctor_cli=""
    for candidate in \
        "$HOME/.local/bin/vexdb" \
        "$STAGE/bin/vexdb"; do
        if [ -x "$candidate" ]; then
            doctor_cli="$candidate"
            break
        fi
    done
    healthy=0
    for attempt in 1 2 3 4 5 6 7 8; do
        if [ -n "$doctor_cli" ]; then
            state="$("$doctor_cli" fs --json doctor 2>/dev/null || true)"
            case "$state" in
                *'"extension":"service-unavailable"'*) healthy=0 ;;
                *'"extension":'*) healthy=$((healthy + 1)) ;;
                *) healthy=0 ;;
            esac
        elif command -v pluginkit >/dev/null; then
            plugin_state="$(pluginkit -mAvv -p com.apple.fskit.fsmodule 2>&1 || true)"
            case "$plugin_state" in
                *'Connection invalid'*) healthy=0 ;;
                *'io.vexdb.vexfs.extension'*) healthy=$((healthy + 1)) ;;
                *) healthy=0 ;;
            esac
        fi
        [ "$healthy" -lt 3 ] || return 0
        sleep 1
    done
    if [ "$SIGN_MODE" = ad-hoc ]; then
        echo "警告：当前用户的 FSKit 服务不可查询；ad-hoc 包未安装到系统，跳过服务恢复 Gate。" >&2
        return 0
    fi
    echo "清理 Xcode 临时扩展后，当前用户的 FSKit 服务仍不可连接" >&2
    return 1
}

cleanup_fskit_build_registration() {
    unregister_temporary_fskit_apps
    # The zip is the deliverable. Keeping an expanded signed App below dist/
    # lets LaunchServices discover a second copy of the same FSKit module.
    rm -rf "$STAGE"
    if [ "$FSKIT_RESTORE_REQUIRED" = true ]; then
        # Let LaunchServices finish every notification caused by Xcode and by
        # deleting the expanded stage. Recover only once, after no more bundle
        # work remains, so a delayed notification cannot invalidate FSKit again.
        sleep "$FSKIT_SETTLE_SECONDS"
        restore_installed_fskit_app
        FSKIT_RESTORE_REQUIRED=false
    fi
}

# archive/export 即使在后续步骤失败也不能留在 FSKit 的候选模块列表里。
trap cleanup_fskit_build_registration EXIT

mkdir -p "$BUILD_DIR" "$DIST_DIR"

if fskit_mounts_active; then
    echo "检测到正在使用的 FSKit 文件系统；请先卸载相关卷，再构建 macOS 包。" >&2
    exit 1
fi

echo "=== 构建 VexDB-Lite CLI 与 SQLite 扩展 ==="
"$CMAKE_BIN" -S "$SQLITE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MINIMUM_MACOS" \
    -DVEXDB_LITE_VERSION="$VERSION" \
    -DVEXFS_REQUIRE_POSTGRESQL=ON \
    -DVEXFS_BUNDLE_POSTGRESQL_RUNTIME=ON \
    -DVEXDB_SQLITE_BUILD_TESTS=OFF
"$CMAKE_BIN" --build "$BUILD_DIR" --target vexfs_cli vexdb_lite_loadable -j "$BUILD_JOBS"
PG_CLIENT_LIBRARY="$(sed -n 's/^VEXFS_POSTGRESQL_CLIENT_LIBRARY:FILEPATH=//p' \
    "$BUILD_DIR/CMakeCache.txt")"
[ -f "$PG_CLIENT_LIBRARY" ] || {
    echo "构建没有解析到可打包的 libpq：${PG_CLIENT_LIBRARY:-missing}" >&2
    exit 1
}
PG_RUNTIME_SEARCH_DIR="$(cd "$(dirname "$PG_CLIENT_LIBRARY")" && pwd)"

echo "=== 构建 VexDB Lite App 与 VexFS extension ==="
if [ "$SIGN_MODE" = developer-id ]; then
    rm -rf "$ARCHIVE_PATH" "$EXPORT_DIR"
    xcodebuild \
        -jobs "$XCODE_JOBS" \
        -project "$SCRIPT_DIR/VexFS.xcodeproj" \
        -scheme VexFSApp \
        -configuration Release \
        -derivedDataPath "$DERIVED_DATA" \
        -archivePath "$ARCHIVE_PATH" \
        -destination 'generic/platform=macOS' \
        ARCHS="$ARCH" ONLY_ACTIVE_ARCH=YES \
        MARKETING_VERSION="$BUNDLE_MARKETING_VERSION" \
        CURRENT_PROJECT_VERSION="$BUNDLE_BUILD_VERSION" \
        DEVELOPMENT_TEAM="$TEAM_ID" CODE_SIGN_STYLE=Automatic \
        -allowProvisioningUpdates archive
    # InstallationBuildProductsLocation 可能在 exportArchive 期间被 Xcode 删除；
    # 必须趁临时 App 仍存在时撤销登记，否则会留下无法按路径清除的 LS 记录。
    sleep 2
    unregister_temporary_fskit_apps
    xcodebuild \
        -jobs "$XCODE_JOBS" \
        -exportArchive \
        -archivePath "$ARCHIVE_PATH" \
        -exportPath "$EXPORT_DIR" \
        -exportOptionsPlist "$EXPORT_OPTIONS" \
        -allowProvisioningUpdates
    APP_SOURCE="$EXPORT_DIR/$APP_NAME"
else
    xcodebuild \
        -jobs "$XCODE_JOBS" \
        -project "$SCRIPT_DIR/VexFS.xcodeproj" \
        -scheme VexFSApp \
        -configuration Release \
        -derivedDataPath "$DERIVED_DATA" \
        ARCHS="$ARCH" ONLY_ACTIVE_ARCH=YES \
        MARKETING_VERSION="$BUNDLE_MARKETING_VERSION" \
        CURRENT_PROJECT_VERSION="$BUNDLE_BUILD_VERSION" \
        CODE_SIGNING_ALLOWED=NO build
    APP_SOURCE="$DERIVED_DATA/Build/Products/Release/$APP_NAME"
fi
[ -d "$APP_SOURCE" ] || { echo "没有找到 $APP_NAME" >&2; exit 1; }
[ -x "$BUILD_DIR/vexdb" ] || { echo "没有找到 vexdb CLI" >&2; exit 1; }
[ -x "$BUILD_DIR/vexfs-nfs-gateway" ] || { echo "没有找到 VexFS NFS gateway" >&2; exit 1; }
[ -f "$BUILD_DIR/vexdb_lite.dylib" ] || { echo "没有找到 vexdb_lite.dylib" >&2; exit 1; }
for bundle in \
    "$APP_SOURCE" \
    "$APP_SOURCE/Contents/Extensions/VexFSAppEx.appex"; do
    actual_marketing="$(plutil -extract CFBundleShortVersionString raw -o - \
        "$bundle/Contents/Info.plist")"
    actual_build="$(plutil -extract CFBundleVersion raw -o - \
        "$bundle/Contents/Info.plist")"
    [ "$actual_marketing" = "$BUNDLE_MARKETING_VERSION" ] || {
        echo "Bundle marketing version 错误：$bundle（$actual_marketing）" >&2
        exit 1
    }
    [ "$actual_build" = "$BUNDLE_BUILD_VERSION" ] || {
        echo "Bundle build version 错误：$bundle（$actual_build）" >&2
        exit 1
    }
done

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib/runtime" "$STAGE/licenses" "$STAGE/.payload"
ditto "$APP_SOURCE" "$PAYLOAD_APP"
if [ "$SIGN_MODE" = developer-id ]; then
    # 先复制交付 App，再撤销并删除 Xcode 的临时同 ID App。这样 LaunchServices
    # 后续不会重新发现 .xcarchive 并让 FSKit 加载到已经失效的构建路径。
    sleep 2
    unregister_temporary_fskit_apps
    rm -rf "$ARCHIVE_PATH" "$EXPORT_DIR" "$DERIVED_DATA"
fi
install -m 0755 "$BUILD_DIR/vexdb" "$STAGE/bin/vexdb"
install -m 0755 "$BUILD_DIR/vexfs-nfs-gateway" "$STAGE/bin/vexfs-nfs-gateway"
ln -s vexdb "$STAGE/bin/vexfs"
install -m 0644 "$BUILD_DIR/vexdb_lite.dylib" "$STAGE/lib/vexdb_lite.dylib"
install -m 0755 "$PACKAGE_DIR/install.sh" "$STAGE/install.sh"
install -m 0755 "$PACKAGE_DIR/Install.command" "$STAGE/Install.command"
install -m 0755 "$PACKAGE_DIR/uninstall.sh" "$STAGE/uninstall.sh"
install -m 0644 "$PACKAGE_DIR/README.md" "$STAGE/README.md"
install -m 0644 "$PACKAGE_DIR/使用说明.md" "$STAGE/使用说明.md"
install -m 0644 "$PACKAGE_DIR/使用说明.md" "$GUIDE"
install -m 0644 "$ROOT/LICENSE" "$STAGE/LICENSE"
install -m 0644 "$ROOT/agent_files/mount/macos/nfs_gateway/THIRD_PARTY.md" \
    "$STAGE/THIRD_PARTY.md"
install -m 0644 "$ROOT/agent_files/mount/macos/nfs_gateway/vendor/nfsserve/LICENSE" \
    "$STAGE/licenses/nfsserve-LICENSE"

EXTENSION="$PAYLOAD_APP/Contents/Extensions/VexFSAppEx.appex"
EXTENSION_EXECUTABLE="$EXTENSION/Contents/MacOS/VexFSAppEx"
EXTENSION_FRAMEWORKS="$EXTENSION/Contents/Frameworks"
[ -x "$EXTENSION_EXECUTABLE" ] || { echo "FSKit extension 主程序缺失" >&2; exit 1; }
EXTENSION_SIGN_ENTITLEMENTS="$SCRIPT_DIR/VexFSAppEx/VexFSAppEx.entitlements"
APP_SIGN_ENTITLEMENTS="$SCRIPT_DIR/VexFSApp/VexFSApp.entitlements"
if [ "$SIGN_MODE" = developer-id ]; then
    # Xcode injects the application identifier and team identifier required by
    # FSKit when it exports the Developer ID App. Bundling libpq invalidates the
    # original signature, so preserve the complete exported entitlements before
    # re-signing instead of falling back to the smaller source plist.
    EXTENSION_SIGN_ENTITLEMENTS="$BUILD_DIR/exported-extension-entitlements.plist"
    APP_SIGN_ENTITLEMENTS="$BUILD_DIR/exported-app-entitlements.plist"
    codesign -d --entitlements - --xml "$EXTENSION" 2>/dev/null \
        > "$EXTENSION_SIGN_ENTITLEMENTS"
    codesign -d --entitlements - --xml "$PAYLOAD_APP" 2>/dev/null \
        > "$APP_SIGN_ENTITLEMENTS"
    plutil -lint "$EXTENSION_SIGN_ENTITLEMENTS" "$APP_SIGN_ENTITLEMENTS" >/dev/null
fi

# The unpacked package and the installed CLI have different relative library
# layouts, so CMake wrote both safe runpaths when it linked the executable. The
# bundler removes any remaining build-machine paths from transitive libraries.
bash "$SCRIPT_DIR/bundle_pg_runtime.sh" \
    "$STAGE/bin/vexdb" "$STAGE/lib/runtime" "$ARCH" "$PG_RUNTIME_SEARCH_DIR"
bash "$SCRIPT_DIR/bundle_pg_runtime.sh" \
    "$STAGE/bin/vexfs-nfs-gateway" "$STAGE/lib/runtime" "$ARCH" "$PG_RUNTIME_SEARCH_DIR"
bash "$SCRIPT_DIR/bundle_pg_runtime.sh" \
    "$EXTENSION_EXECUTABLE" "$EXTENSION_FRAMEWORKS" "$ARCH" "$PG_RUNTIME_SEARCH_DIR"

SIGNATURE=ad-hoc
SIGNING_IDENTITY=-
SIGNING_TEAM=none
CERTIFICATE_SHA1=none
if [ "$SIGN_MODE" = developer-id ]; then
    CERT_PREFIX="$BUILD_DIR/vexfs-app-certificate-"
    rm -f "${CERT_PREFIX}"*
    codesign -d --extract-certificates="$CERT_PREFIX" "$PAYLOAD_APP"
    CERTIFICATE_SHA1="$(openssl x509 -inform DER -in "${CERT_PREFIX}0" -noout -fingerprint -sha1 | cut -d= -f2 | tr -d ':')"
    [ -n "$CERTIFICATE_SHA1" ] || { echo "无法读取 App 的签名证书" >&2; exit 1; }
    SIGNING_IDENTITY="$CERTIFICATE_SHA1"
    SIGNING_TEAM="$TEAM_ID"
    SIGNATURE=developer-id
    find "$STAGE/lib/runtime" "$EXTENSION_FRAMEWORKS" -type f -name '*.dylib' -print0 \
        | while IFS= read -r -d '' library; do
            codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$library"
        done
    codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$STAGE/bin/vexdb"
    codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$STAGE/bin/vexfs-nfs-gateway"
    codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$STAGE/lib/vexdb_lite.dylib"
    codesign --force --options runtime --timestamp --generate-entitlement-der \
        --sign "$SIGNING_IDENTITY" \
        --entitlements "$EXTENSION_SIGN_ENTITLEMENTS" "$EXTENSION"
    codesign --force --options runtime --timestamp --generate-entitlement-der \
        --sign "$SIGNING_IDENTITY" \
        --entitlements "$APP_SIGN_ENTITLEMENTS" "$PAYLOAD_APP"

    verify_team_identifier() {
        local item="$1"
        local actual
        actual="$(codesign -d --verbose=4 "$item" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
        if [ "$actual" != "$TEAM_ID" ]; then
            echo "Developer ID 签名团队错误：$item（实际 ${actual:-none}，期望 $TEAM_ID）" >&2
            exit 1
        fi
    }
    verify_team_identifier "$PAYLOAD_APP"
    verify_team_identifier "$EXTENSION"
    verify_team_identifier "$STAGE/bin/vexdb"
    verify_team_identifier "$STAGE/bin/vexfs-nfs-gateway"
    verify_team_identifier "$STAGE/lib/vexdb_lite.dylib"
    find "$STAGE/lib/runtime" "$EXTENSION_FRAMEWORKS" -type f -name '*.dylib' -print0 \
        | while IFS= read -r -d '' library; do verify_team_identifier "$library"; done
    SIGNED_EXTENSION_ENTITLEMENTS="$BUILD_DIR/signed-extension-entitlements.plist"
    SIGNED_EXTENSION_DIAGNOSTICS="$BUILD_DIR/signed-extension-entitlements.log"
    if ! codesign -d --entitlements - --xml "$EXTENSION" \
            >"$SIGNED_EXTENSION_ENTITLEMENTS" 2>"$SIGNED_EXTENSION_DIAGNOSTICS"; then
        cat "$SIGNED_EXTENSION_DIAGNOSTICS" >&2
        echo "无法读取重签后的 FSKit 扩展权限" >&2
        exit 1
    fi
    if grep -q 'invalid entitlements blob' "$SIGNED_EXTENSION_DIAGNOSTICS" ||
            ! plutil -lint "$SIGNED_EXTENSION_ENTITLEMENTS" >/dev/null 2>&1; then
        cat "$SIGNED_EXTENSION_DIAGNOSTICS" >&2
        echo "FSKit 扩展的 DER 权限无效，系统会忽略这些权限" >&2
        exit 1
    fi
    # plutil treats periods as key-path separators. Entitlement names contain
    # literal periods, so use PlistBuddy to read the exact top-level keys.
    signed_team="$(/usr/libexec/PlistBuddy \
        -c 'Print :com.apple.developer.team-identifier' \
        "$SIGNED_EXTENSION_ENTITLEMENTS" 2>/dev/null || true)"
    signed_application="$(/usr/libexec/PlistBuddy \
        -c 'Print :com.apple.application-identifier' \
        "$SIGNED_EXTENSION_ENTITLEMENTS" 2>/dev/null || true)"
    [ "$signed_team" = "$TEAM_ID" ] || {
        echo "FSKit 扩展缺少 Developer ID team entitlement：${signed_team:-missing}" >&2
        exit 1
    }
    [ "$signed_application" = "$TEAM_ID.io.vexdb.vexfs.extension" ] || {
        echo "FSKit 扩展 application identifier 错误：${signed_application:-missing}" >&2
        exit 1
    }
else
    # 没有 Developer ID 时仍做完整的 ad-hoc 签名，避免发送过程中破坏 bundle seal。
    find "$STAGE/lib/runtime" "$EXTENSION_FRAMEWORKS" -type f -name '*.dylib' -print0 \
        | while IFS= read -r -d '' library; do
            codesign --force --sign - --timestamp=none "$library"
        done
    codesign --force --sign - --timestamp=none "$STAGE/bin/vexdb"
    codesign --force --sign - --timestamp=none "$STAGE/bin/vexfs-nfs-gateway"
    codesign --force --sign - --timestamp=none "$STAGE/lib/vexdb_lite.dylib"
    codesign --force --sign - --timestamp=none --generate-entitlement-der \
        --entitlements "$EXTENSION_SIGN_ENTITLEMENTS" "$EXTENSION"
    codesign --force --sign - --timestamp=none --generate-entitlement-der \
        --entitlements "$APP_SIGN_ENTITLEMENTS" "$PAYLOAD_APP"
fi
codesign --verify --deep --strict --verbose=2 "$PAYLOAD_APP"
codesign --verify --strict --verbose=2 "$STAGE/bin/vexdb"
codesign --verify --strict --verbose=2 "$STAGE/bin/vexfs-nfs-gateway"
codesign --verify --strict --verbose=2 "$STAGE/lib/vexdb_lite.dylib"
find "$STAGE/lib/runtime" "$EXTENSION_FRAMEWORKS" -type f -name '*.dylib' -print0 \
    | while IFS= read -r -d '' library; do
        codesign --verify --strict --verbose=2 "$library"
    done

echo "=== 验证统一 SQLite、向量和文件入口 ==="
bash "$ROOT/agent_files/cli/test/vexdb_unified_smoke.sh" "$STAGE/bin/vexdb"

NOTARIZATION_STATUS=not-submitted
NOTARIZATION_SUBMISSION_ID=none

write_manifest() {
    {
        echo "product=VexDB-Lite"
        echo "filesystem=VexFS"
        echo "preview_version=$VERSION"
        echo "bundle_marketing_version=$BUNDLE_MARKETING_VERSION"
        echo "bundle_build_version=$BUNDLE_BUILD_VERSION"
        echo "contract_version=0.9.0"
        echo "mount_abi_version=$RUNTIME_ABI"
        echo "platform=macOS"
        echo "minimum_macos=$MINIMUM_MACOS"
        echo "fskit_minimum_macos=$FSKIT_MINIMUM_MACOS"
        echo "architecture=$ARCH"
        echo "source_branch=$GIT_BRANCH"
        echo "source_commit=$GIT_COMMIT"
        echo "source_dirty=$GIT_DIRTY"
        echo "signature=$SIGNATURE"
        echo "signing_team=$SIGNING_TEAM"
        echo "certificate_sha1=$CERTIFICATE_SHA1"
        echo "notarization=$NOTARIZATION_STATUS"
        echo "notarization_submission_id=$NOTARIZATION_SUBMISSION_ID"
    } > "$STAGE/MANIFEST.txt"
}

write_hashes() {
    (
        cd "$STAGE"
        shasum -a 256 \
            MANIFEST.txt \
            install.sh \
            Install.command \
            uninstall.sh \
            README.md \
            LICENSE \
            THIRD_PARTY.md \
            licenses/nfsserve-LICENSE \
            bin/vexdb \
            bin/vexfs-nfs-gateway \
            lib/vexdb_lite.dylib \
            使用说明.md \
            "$PAYLOAD_RELATIVE/Contents/MacOS/VexDB Lite" \
            "$PAYLOAD_RELATIVE/Contents/Extensions/VexFSAppEx.appex/Contents/MacOS/VexFSAppEx" \
            > SHA256SUMS.txt
        find lib/runtime \
            "$PAYLOAD_RELATIVE/Contents/Extensions/VexFSAppEx.appex/Contents/Frameworks" \
            -type f -name '*.dylib' -print | LC_ALL=C sort | while IFS= read -r library; do
                shasum -a 256 "$library"
            done >> SHA256SUMS.txt
    )
}

write_zip() {
    rm -f "$ZIP" "$ZIP.sha256"
    COPYFILE_DISABLE=1 ditto -c -k --norsrc --keepParent "$STAGE" "$ZIP"
    if unzip -Z1 "$ZIP" | grep -Eq '(^|/)__MACOSX/|(^|/)\._'; then
        echo "压缩包包含 AppleDouble 文件，停止交付" >&2
        exit 1
    fi
    (
        cd "$DIST_DIR"
        shasum -a 256 "$(basename "$ZIP")" > "$(basename "$ZIP").sha256"
    )
}

verify_zip() (
    local verify_dir verify_root verify_app
    verify_dir="$(mktemp -d "$BUILD_DIR/zip-verify.XXXXXX")"
    verify_root="$verify_dir/$PACKAGE_NAME"
    verify_app="$verify_root/$PAYLOAD_RELATIVE"
    cleanup_zip_verification() {
        rm -rf "$verify_dir"
    }
    trap cleanup_zip_verification EXIT

    (
        cd "$DIST_DIR"
        shasum -a 256 -c "$(basename "$ZIP").sha256"
    )
    ditto -x -k "$ZIP" "$verify_dir"
    [ -d "$verify_app" ] || { echo "压缩包缺少 $PAYLOAD_RELATIVE" >&2; return 1; }
    [ -x "$verify_root/bin/vexdb" ] || { echo "压缩包缺少 bin/vexdb" >&2; return 1; }
    [ -x "$verify_root/bin/vexfs-nfs-gateway" ] || { echo "压缩包缺少 NFS gateway" >&2; return 1; }
    [ -L "$verify_root/bin/vexfs" ] || { echo "压缩包中的 bin/vexfs 不是兼容链接" >&2; return 1; }
    [ -f "$verify_root/lib/vexdb_lite.dylib" ] || { echo "压缩包缺少 SQLite 扩展" >&2; return 1; }
    [ -f "$verify_root/lib/runtime/libpq.5.dylib" ] || { echo "压缩包缺少 PostgreSQL runtime" >&2; return 1; }
    [ -f "$verify_root/使用说明.md" ] || { echo "压缩包缺少使用说明" >&2; return 1; }
    [ -f "$verify_root/THIRD_PARTY.md" ] || { echo "压缩包缺少第三方软件说明" >&2; return 1; }
    [ -f "$verify_root/licenses/nfsserve-LICENSE" ] || { echo "压缩包缺少 nfsserve 许可证" >&2; return 1; }

    codesign --verify --deep --strict --verbose=2 "$verify_app"
    codesign --verify --strict --verbose=2 "$verify_root/bin/vexdb"
    codesign --verify --strict --verbose=2 "$verify_root/bin/vexfs-nfs-gateway"
    codesign --verify --strict --verbose=2 "$verify_root/lib/vexdb_lite.dylib"
    find "$verify_root/lib/runtime" \
        "$verify_app/Contents/Extensions/VexFSAppEx.appex/Contents/Frameworks" \
        -type f -name '*.dylib' -print0 | while IFS= read -r -d '' library; do
            codesign --verify --strict --verbose=2 "$library"
        done
    bash "$ROOT/agent_files/cli/test/vexdb_unified_smoke.sh" "$verify_root/bin/vexdb"
)

write_manifest
write_hashes
bash "$ROOT/tests/eval/vexfs/documentation_smoke.sh" "$STAGE"
VEXDB_LITE_PACKAGE_STAGE="$STAGE" "$PYTHON_BIN" "$ROOT/tests/eval/vexfs/run.py" \
    --mode quick --filter package.unified-install --include-package --fail-on-skip \
    --build-dir "$BUILD_DIR"
write_zip
verify_zip

if [ "$SIGN_MODE" = developer-id ] && [ -n "$NOTARY_PROFILE" ]; then
    echo "=== 提交 Apple 公证 ==="
    NOTARY_RESULT="$BUILD_DIR/notary-result.json"
    xcrun notarytool submit "$ZIP" \
        --keychain-profile "$NOTARY_PROFILE" \
        --output-format json \
        --wait > "$NOTARY_RESULT"
    cat "$NOTARY_RESULT"
    NOTARIZATION_STATUS="$(plutil -extract status raw -o - "$NOTARY_RESULT")"
    NOTARIZATION_SUBMISSION_ID="$(plutil -extract id raw -o - "$NOTARY_RESULT")"
    if [ "$NOTARIZATION_STATUS" != Accepted ]; then
        echo "Apple 公证未通过：$NOTARIZATION_STATUS（$NOTARIZATION_SUBMISSION_ID）" >&2
        exit 1
    fi
    NOTARIZATION_STATUS=accepted
    xcrun stapler staple "$PAYLOAD_APP"
    xcrun stapler validate "$PAYLOAD_APP"
    spctl --assess --type execute --verbose=4 "$PAYLOAD_APP"
    write_manifest
    write_hashes
    bash "$ROOT/tests/eval/vexfs/documentation_smoke.sh" "$STAGE"
    write_zip
    verify_zip
fi

# 成功返回前先完成临时注册清理并验证用户级 FSKit 服务。失败路径仍由 EXIT trap
# 执行同样的尽力清理。
cleanup_fskit_build_registration
trap - EXIT

echo ""
echo "=== VexDB-Lite 技术预览包 ==="
ls -lh "$ZIP" "$ZIP.sha256" "$GUIDE"
cat "$ZIP.sha256"
