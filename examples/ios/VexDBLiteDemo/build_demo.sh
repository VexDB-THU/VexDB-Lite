#!/bin/bash
set -e

DEMO_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DEMO_DIR/../../.." && pwd)"
BUILD_DIR="$DEMO_DIR/build-simulator"
CMAKE="${CMAKE:-$(command -v cmake || true)}"
[ -n "$CMAKE" ] || CMAKE=/opt/homebrew/bin/cmake
[ -x "$CMAKE" ] || { echo "找不到 cmake" >&2; exit 1; }

IOS_DEPLOYMENT_TARGET=17.0 bash "$ROOT/build_sqlite.sh" ios

"$CMAKE" -S "$DEMO_DIR" -B "$BUILD_DIR" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0

"$CMAKE" --build "$BUILD_DIR" --config Debug --target VexDBLiteDemo -- \
    -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO

APP="$BUILD_DIR/Debug-iphonesimulator/VexDB Lite.app"
[ -d "$APP" ] || { echo "未找到 App: $APP" >&2; exit 1; }
echo ""
echo "=== VexDB Lite iOS Demo ==="
echo "$APP"

if [ "${1:-}" = "run" ]; then
    DEVICE=$(xcrun simctl list devices booted -j | \
        /usr/bin/python3 -c 'import json,sys; d=json.load(sys.stdin); print(next((x["udid"] for xs in d["devices"].values() for x in xs if x["state"]=="Booted"), ""))')
    [ -n "$DEVICE" ] || { echo "没有已启动的 iOS Simulator" >&2; exit 1; }
    xcrun simctl install "$DEVICE" "$APP"
    xcrun simctl launch "$DEVICE" org.vexdb.lite.demo
fi
