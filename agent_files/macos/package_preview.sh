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

NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
BUILD_JOBS="${VEXDB_LITE_BUILD_JOBS:-$NCPU}"
XCODE_JOBS="${VEXDB_LITE_XCODE_JOBS:-2}"
case "$BUILD_JOBS:$XCODE_JOBS" in
    *[!0-9:]*|0:*|*:0) echo "构建并行度必须是正整数" >&2; exit 2 ;;
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
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

unregister_temporary_fskit_apps() {
    [ -x "$LSREGISTER" ] || return 0
    local app
    for app in \
        "$ARCHIVE_PATH/Products/Applications/$APP_NAME" \
        "$EXPORT_DIR/$APP_NAME" \
        "$DERIVED_DATA/Build/Intermediates.noindex/ArchiveIntermediates/VexFSApp/InstallationBuildProductsLocation/Applications/$APP_NAME" \
        "$DERIVED_DATA/Build/Intermediates.noindex/ArchiveIntermediates/VexFSApp/BuildProductsPath/Release/$APP_NAME"; do
        [ -d "$app" ] || continue
        # xcodebuild 的登记可能延迟落盘。重复撤销只作用于这两个构建路径，
        # 不会清除用户已经批准的 extension 启用状态。
        "$LSREGISTER" -u "$app" >/dev/null 2>&1 || true
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
    local mounts
    mounts="$(/sbin/mount 2>/dev/null | tr '[:upper:]' '[:lower:]')"
    case "$mounts" in
        *"(vexfs,"*|*"(exfat,"*|*"(msdos,"*|*" type vexfs"*|*" type exfat"*|*" type msdos"*) ;;
        *) pkill -KILL -x fskit_agent >/dev/null 2>&1 || true ;;
    esac
}

cleanup_fskit_build_registration() {
    unregister_temporary_fskit_apps
    restore_installed_fskit_app
}

# archive/export 即使在后续步骤失败也不能留在 FSKit 的候选模块列表里。
trap cleanup_fskit_build_registration EXIT

mkdir -p "$BUILD_DIR" "$DIST_DIR"

echo "=== 构建 VexDB-Lite CLI 与 SQLite 扩展 ==="
"$CMAKE_BIN" -S "$SQLITE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DVEXDB_LITE_VERSION="$VERSION" \
    -DVEXDB_SQLITE_BUILD_TESTS=OFF
"$CMAKE_BIN" --build "$BUILD_DIR" --target vexdb_cli vexdb_lite_loadable -j "$BUILD_JOBS"

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
        ARCHS="$ARCH" ONLY_ACTIVE_ARCH=YES CODE_SIGNING_ALLOWED=NO build
    APP_SOURCE="$DERIVED_DATA/Build/Products/Release/$APP_NAME"
fi
[ -d "$APP_SOURCE" ] || { echo "没有找到 $APP_NAME" >&2; exit 1; }
[ -x "$BUILD_DIR/vexdb" ] || { echo "没有找到 vexdb CLI" >&2; exit 1; }
[ -f "$BUILD_DIR/vexdb_lite.dylib" ] || { echo "没有找到 vexdb_lite.dylib" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib"
ditto "$APP_SOURCE" "$STAGE/$APP_NAME"
if [ "$SIGN_MODE" = developer-id ]; then
    # 先复制交付 App，再撤销并删除 Xcode 的临时同 ID App。这样 LaunchServices
    # 后续不会重新发现 .xcarchive 并让 FSKit 加载到已经失效的构建路径。
    sleep 2
    unregister_temporary_fskit_apps
    rm -rf "$ARCHIVE_PATH" "$EXPORT_DIR" "$DERIVED_DATA"
    restore_installed_fskit_app
fi
install -m 0755 "$BUILD_DIR/vexdb" "$STAGE/bin/vexdb"
ln -s vexdb "$STAGE/bin/vexfs"
install -m 0644 "$BUILD_DIR/vexdb_lite.dylib" "$STAGE/lib/vexdb_lite.dylib"
install -m 0755 "$PACKAGE_DIR/install.sh" "$STAGE/install.sh"
install -m 0755 "$PACKAGE_DIR/Install.command" "$STAGE/Install.command"
install -m 0755 "$PACKAGE_DIR/uninstall.sh" "$STAGE/uninstall.sh"
install -m 0644 "$PACKAGE_DIR/README.md" "$STAGE/README.md"
install -m 0644 "$PACKAGE_DIR/使用说明.md" "$STAGE/使用说明.md"
install -m 0644 "$PACKAGE_DIR/使用说明.md" "$GUIDE"
install -m 0644 "$ROOT/LICENSE" "$STAGE/LICENSE"

EXTENSION="$STAGE/$APP_NAME/Contents/Extensions/VexFSAppEx.appex"
SIGNATURE=ad-hoc
SIGNING_IDENTITY=-
SIGNING_TEAM=none
CERTIFICATE_SHA1=none
if [ "$SIGN_MODE" = developer-id ]; then
    CERT_PREFIX="$BUILD_DIR/vexfs-app-certificate-"
    rm -f "${CERT_PREFIX}"*
    codesign -d --extract-certificates="$CERT_PREFIX" "$STAGE/$APP_NAME"
    CERTIFICATE_SHA1="$(openssl x509 -inform DER -in "${CERT_PREFIX}0" -noout -fingerprint -sha1 | cut -d= -f2 | tr -d ':')"
    [ -n "$CERTIFICATE_SHA1" ] || { echo "无法读取 App 的签名证书" >&2; exit 1; }
    SIGNING_IDENTITY="$CERTIFICATE_SHA1"
    SIGNING_TEAM="$TEAM_ID"
    SIGNATURE=developer-id
    codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$STAGE/bin/vexdb"
    codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$STAGE/lib/vexdb_lite.dylib"

    verify_team_identifier() {
        local item="$1"
        local actual
        actual="$(codesign -d --verbose=4 "$item" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
        if [ "$actual" != "$TEAM_ID" ]; then
            echo "Developer ID 签名团队错误：$item（实际 ${actual:-none}，期望 $TEAM_ID）" >&2
            exit 1
        fi
    }
    verify_team_identifier "$STAGE/$APP_NAME"
    verify_team_identifier "$EXTENSION"
    verify_team_identifier "$STAGE/bin/vexdb"
    verify_team_identifier "$STAGE/lib/vexdb_lite.dylib"
else
    # 没有 Developer ID 时仍做完整的 ad-hoc 签名，避免发送过程中破坏 bundle seal。
    codesign --force --sign - --timestamp=none \
        --entitlements "$SCRIPT_DIR/VexFSAppEx/VexFSAppEx.entitlements" "$EXTENSION"
    codesign --force --sign - --timestamp=none \
        --entitlements "$SCRIPT_DIR/VexFSApp/VexFSApp.entitlements" "$STAGE/$APP_NAME"
    codesign --force --sign - --timestamp=none "$STAGE/bin/vexdb"
    codesign --force --sign - --timestamp=none "$STAGE/lib/vexdb_lite.dylib"
fi
codesign --verify --deep --strict --verbose=2 "$STAGE/$APP_NAME"
codesign --verify --strict --verbose=2 "$STAGE/bin/vexdb"
codesign --verify --strict --verbose=2 "$STAGE/lib/vexdb_lite.dylib"

echo "=== 验证统一 SQLite、向量和文件入口 ==="
bash "$ROOT/agent_files/cli/test/vexdb_unified_smoke.sh" "$STAGE/bin/vexdb"

NOTARIZATION_STATUS=not-submitted
NOTARIZATION_SUBMISSION_ID=none

write_manifest() {
    {
        echo "product=VexDB-Lite"
        echo "filesystem=VexFS"
        echo "preview_version=$VERSION"
        echo "contract_version=0.9.0"
        echo "mount_abi_version=$RUNTIME_ABI"
        echo "platform=macOS"
        echo "minimum_macos=26.0"
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
            bin/vexdb \
            lib/vexdb_lite.dylib \
            使用说明.md \
            "$APP_NAME/Contents/MacOS/VexDB Lite" \
            "$APP_NAME/Contents/Extensions/VexFSAppEx.appex/Contents/MacOS/VexFSAppEx" \
            > SHA256SUMS.txt
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

verify_zip() {
    local verify_dir verify_root
    verify_dir="$(mktemp -d "$BUILD_DIR/zip-verify.XXXXXX")"
    verify_root="$verify_dir/$PACKAGE_NAME"

    (
        cd "$DIST_DIR"
        shasum -a 256 -c "$(basename "$ZIP").sha256"
    )
    ditto -x -k "$ZIP" "$verify_dir"
    [ -d "$verify_root/$APP_NAME" ] || { echo "压缩包缺少 $APP_NAME" >&2; return 1; }
    [ -x "$verify_root/bin/vexdb" ] || { echo "压缩包缺少 bin/vexdb" >&2; return 1; }
    [ -L "$verify_root/bin/vexfs" ] || { echo "压缩包中的 bin/vexfs 不是兼容链接" >&2; return 1; }
    [ -f "$verify_root/lib/vexdb_lite.dylib" ] || { echo "压缩包缺少 SQLite 扩展" >&2; return 1; }
    [ -f "$verify_root/使用说明.md" ] || { echo "压缩包缺少使用说明" >&2; return 1; }

    codesign --verify --deep --strict --verbose=2 "$verify_root/$APP_NAME"
    codesign --verify --strict --verbose=2 "$verify_root/bin/vexdb"
    codesign --verify --strict --verbose=2 "$verify_root/lib/vexdb_lite.dylib"
    bash "$ROOT/agent_files/cli/test/vexdb_unified_smoke.sh" "$verify_root/bin/vexdb"
    rm -rf "$verify_dir"
}

write_manifest
write_hashes
bash "$ROOT/tests/eval/vexfs/documentation_smoke.sh" "$STAGE"
VEXDB_LITE_PACKAGE_STAGE="$STAGE" python3 "$ROOT/tests/eval/vexfs/run.py" \
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
    xcrun stapler staple "$STAGE/$APP_NAME"
    xcrun stapler validate "$STAGE/$APP_NAME"
    spctl --assess --type execute --verbose=4 "$STAGE/$APP_NAME"
    write_manifest
    write_hashes
    bash "$ROOT/tests/eval/vexfs/documentation_smoke.sh" "$STAGE"
    write_zip
    verify_zip
fi

echo ""
echo "=== VexDB-Lite 技术预览包 ==="
ls -lh "$ZIP" "$ZIP.sha256" "$GUIDE"
cat "$ZIP.sha256"
