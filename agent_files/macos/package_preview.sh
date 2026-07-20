#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SQLITE_DIR="$ROOT/vexdb_sqlite"
PACKAGE_DIR="$SCRIPT_DIR/package"

VERSION="${VEXFS_PREVIEW_VERSION:-0.1.0-preview.1}"
ARCH="${VEXFS_ARCH:-arm64}"
case "$VERSION" in
    *[!A-Za-z0-9._-]*) echo "版本只能包含字母、数字、点、下划线和连字符" >&2; exit 2 ;;
esac
case "$ARCH" in
    arm64|x86_64) ;;
    *) echo "VEXFS_ARCH 只能是 arm64 或 x86_64" >&2; exit 2 ;;
esac

CMAKE_BIN="$(command -v cmake || true)"
[ -x "$CMAKE_BIN" ] || CMAKE_BIN=/opt/homebrew/bin/cmake
[ -x "$CMAKE_BIN" ] || CMAKE_BIN=/usr/local/bin/cmake
[ -x "$CMAKE_BIN" ] || { echo "找不到 cmake" >&2; exit 1; }
command -v xcodebuild >/dev/null || { echo "找不到 xcodebuild" >&2; exit 1; }
command -v codesign >/dev/null || { echo "找不到 codesign" >&2; exit 1; }

NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
BUILD_DIR="$SQLITE_DIR/build-vexfs-package-$ARCH"
DERIVED_DATA="$BUILD_DIR/xcode-derived-data"
DIST_DIR="$ROOT/dist/vexfs"
PACKAGE_NAME="vexfs-${VERSION}-macos-${ARCH}"
STAGE="$DIST_DIR/$PACKAGE_NAME"
ZIP="$DIST_DIR/$PACKAGE_NAME.zip"

mkdir -p "$BUILD_DIR" "$DIST_DIR"

echo "=== 构建 VexFS CLI 与 SQLite 扩展 ==="
"$CMAKE_BIN" -S "$SQLITE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DVEXDB_SQLITE_BUILD_TESTS=OFF
"$CMAKE_BIN" --build "$BUILD_DIR" --target vexfs_cli vexdb_lite_loadable -j "$NCPU"

echo "=== 构建 VexFS App 与 FSKit extension ==="
xcodebuild \
    -project "$SCRIPT_DIR/VexFS.xcodeproj" \
    -scheme VexFSApp \
    -configuration Release \
    -derivedDataPath "$DERIVED_DATA" \
    ARCHS="$ARCH" ONLY_ACTIVE_ARCH=YES CODE_SIGNING_ALLOWED=NO build

APP_SOURCE="$DERIVED_DATA/Build/Products/Release/VexFS.app"
[ -d "$APP_SOURCE" ] || { echo "没有找到 VexFS.app" >&2; exit 1; }
[ -x "$BUILD_DIR/vexfs" ] || { echo "没有找到 vexfs CLI" >&2; exit 1; }
[ -f "$BUILD_DIR/vexdb_lite.dylib" ] || { echo "没有找到 vexdb_lite.dylib" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib"
ditto "$APP_SOURCE" "$STAGE/VexFS.app"
install -m 0755 "$BUILD_DIR/vexfs" "$STAGE/bin/vexfs"
install -m 0644 "$BUILD_DIR/vexdb_lite.dylib" "$STAGE/lib/vexdb_lite.dylib"
install -m 0755 "$PACKAGE_DIR/install.sh" "$STAGE/install.sh"
install -m 0755 "$PACKAGE_DIR/Install.command" "$STAGE/Install.command"
install -m 0755 "$PACKAGE_DIR/uninstall.sh" "$STAGE/uninstall.sh"
install -m 0644 "$PACKAGE_DIR/README.md" "$STAGE/README.md"
install -m 0644 "$ROOT/LICENSE" "$STAGE/LICENSE"

# 没有 Developer ID 时仍做完整的 ad-hoc 签名，避免发送过程中破坏 bundle seal。
EXTENSION="$STAGE/VexFS.app/Contents/Extensions/VexFSAppEx.appex"
codesign --force --sign - --timestamp=none \
    --entitlements "$SCRIPT_DIR/VexFSAppEx/VexFSAppEx.entitlements" "$EXTENSION"
codesign --force --sign - --timestamp=none \
    --entitlements "$SCRIPT_DIR/VexFSApp/VexFSApp.entitlements" "$STAGE/VexFS.app"
codesign --force --sign - --timestamp=none "$STAGE/bin/vexfs"
codesign --force --sign - --timestamp=none "$STAGE/lib/vexdb_lite.dylib"
codesign --verify --deep --strict "$STAGE/VexFS.app"

GIT_COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_BRANCH="$(git -C "$ROOT" branch --show-current 2>/dev/null || echo unknown)"
if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null || true)" ]; then
    GIT_DIRTY=true
else
    GIT_DIRTY=false
fi
{
    echo "product=VexFS"
    echo "preview_version=$VERSION"
    echo "contract_version=0.3.0"
    echo "mount_abi_version=3"
    echo "platform=macOS"
    echo "minimum_macos=26.0"
    echo "architecture=$ARCH"
    echo "source_branch=$GIT_BRANCH"
    echo "source_commit=$GIT_COMMIT"
    echo "source_dirty=$GIT_DIRTY"
    echo "signature=ad-hoc"
} > "$STAGE/MANIFEST.txt"

(
    cd "$STAGE"
    shasum -a 256 \
        bin/vexfs \
        lib/vexdb_lite.dylib \
        VexFS.app/Contents/MacOS/VexFS \
        VexFS.app/Contents/Extensions/VexFSAppEx.appex/Contents/MacOS/VexFSAppEx \
        > SHA256SUMS.txt
)

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

echo ""
echo "=== VexFS 技术预览包 ==="
ls -lh "$ZIP" "$ZIP.sha256"
cat "$ZIP.sha256"
