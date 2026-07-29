#!/bin/bash
# vexdb_lite SQLite 适配层独立构建。
# 仿 vexdb_pg 独立 CMake 模式（根 build.sh 深耦合 DuckDB 源码树，不复用）。
#
#   bash build_sqlite.sh build    # 配置 + 编译双形态 + smoke test 二进制
#   bash build_sqlite.sh test     # build + 跑完整功能/并行/spec 测试
#   bash build_sqlite.sh benchmark # Release 量化器功能 + 性能门槛
#   bash build_sqlite.sh package  # Release 编 macOS arm64+x86_64 → dist/ tarball + SHA256
#   bash build_sqlite.sh ios      # Stage C：device+sim 静态库 → XCFramework（+sim smoke）
#   bash build_sqlite.sh android  # Stage C：NDK arm64-v8a/x86_64 静态库 + loadable .so
#   bash build_sqlite.sh wasm     # 浏览器版：构建 WASM + 跑真实图索引冒烟测试
#   bash build_sqlite.sh vendor   # 仅拉取 SQLite amalgamation
#   bash build_sqlite.sh clean    # 删 build 目录
set -e

DIR="$(cd "$(dirname "$0")" && pwd)/vexdb_sqlite"
BUILD_DIR="$DIR/build"
CMD="${1:-build}"
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)
PYTHON_BIN="${PYTHON_BIN:-python3}"

# 解析 cmake：优先 PATH，其次 conan/brew 常见位置（非交互 shell 常无 cmake on PATH）。
CMAKE="$(command -v cmake || true)"
if [ -z "$CMAKE" ]; then
    for c in \
        /opt/homebrew/bin/cmake \
        /usr/local/bin/cmake \
        "$HOME"/.conan/data/cmake/*/_/_/package/*/CMake.app/Contents/bin/cmake \
        "$HOME"/.conan2/p/cmak*/p/bin/cmake \
        /Applications/CMake.app/Contents/bin/cmake; do
        [ -x "$c" ] && CMAKE="$c" && break
    done
fi
[ -z "$CMAKE" ] && { echo "找不到 cmake：brew install cmake，或激活含 cmake 的 conan/conda 环境" >&2; exit 1; }

# 交叉 toolchain 不会搜索 host 的 Homebrew/Conda include。SQLite 距离 dispatcher
# 只依赖 header-only boost/preprocessor，因此可以安全复用 host 头文件。允许调用方
# 用 VEXDB_BOOST_INC 覆盖；否则从常见位置自动探测，并显式传给所有 CMake 配置。
BOOST_INC="${VEXDB_BOOST_INC:-}"
if [ -z "$BOOST_INC" ]; then
    for b in \
        "$DIR/../thirdparties/boost" \
        /opt/homebrew/include \
        /opt/anaconda3/include \
        /usr/local/include \
        /usr/include; do
        if [ -f "$b/boost/preprocessor/seq.hpp" ]; then
            BOOST_INC="$b"
            break
        fi
    done
fi
BOOST_CMAKE_ARGS=()
[ -n "$BOOST_INC" ] && BOOST_CMAKE_ARGS+=("-DVEXDB_BOOST_INC=$BOOST_INC")

case "$CMD" in
    vendor)
        bash "$DIR/vendor_sqlite.sh"
        ;;
    build|test)
        [ -f "$DIR/third_party/sqlite/sqlite3ext.h" ] || bash "$DIR/vendor_sqlite.sh"
        "$CMAKE" -S "$DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" \
            "${BOOST_CMAKE_ARGS[@]}"
        "$CMAKE" --build "$BUILD_DIR" -j "$NCPU"
        echo ""
        echo "=== 产物 ==="
        ls -la "$BUILD_DIR"/*.dylib "$BUILD_DIR"/*.so "$BUILD_DIR"/*.a 2>/dev/null || true
        if [ "$CMD" = "test" ]; then
            echo ""
            echo "=== M0 静态注册冒烟 ==="
            "$BUILD_DIR/m0_static_smoke"
            echo "=== M1 距离层冒烟 ==="
            "$BUILD_DIR/m1_distance_smoke"
            echo "=== RaBitQ SIMD 分发冒烟 ==="
            "$BUILD_DIR/simd_dispatch_smoke"
            echo "=== M2 虚拟表冒烟 ==="
            "$BUILD_DIR/m2_vtab_smoke"
            echo "=== M3 HNSW 冒烟 ==="
            "$BUILD_DIR/m3_hnsw_smoke"
            echo "=== M3+ 4 万向量并行/持久化/有界写回归 ==="
            "$BUILD_DIR/m3p_parallel_smoke"
            echo "=== SQLite v3 固定持久化文件兼容 ==="
            EXTENSION="$BUILD_DIR/vexdb_lite.dylib"
            [ -f "$EXTENSION" ] || EXTENSION="$BUILD_DIR/vexdb_lite.so"
            "$PYTHON_BIN" "$DIR/test/legacy_fixture_test.py" "$EXTENSION" \
                "$DIR/test/fixtures/sqlite_quantizer_legacy_v3.db.gz"
            echo "=== M4 spec（L2 DSL 渲染 + runner） ==="
            bash "$DIR/../tests/spec/_lib/docker/run_sqlite.sh"
        fi
        ;;
    benchmark)
        BUILD_TYPE=Release bash "$0" build
        EXTENSION="$BUILD_DIR/vexdb_lite.dylib"
        [ -f "$EXTENSION" ] || EXTENSION="$BUILD_DIR/vexdb_lite.so"
        "$PYTHON_BIN" "$DIR/test/quantizer_benchmark.py" "$EXTENSION" \
            --rows 20000 --queries 200 --repetitions 3 --pq-m 16 --check \
            --min-pq-qps-ratio 0.70 --min-rabitq-qps-ratio 0.65
        ;;
    package)
        # macOS 双 arch 发版包：loadable dylib + 静态 .a + 公共头 + 文档。
        # linux x86_64/aarch64 走 scripts/release.sh 的远程 manylinux 链（sqlite target）。
        [ -f "$DIR/third_party/sqlite/sqlite3ext.h" ] || bash "$DIR/vendor_sqlite.sh"
        VERSION="${VEXDB_SQLITE_VERSION:-0.1.0}"
        DIST="$DIR/../dist/sqlite"
        mkdir -p "$DIST"
        for ARCH in arm64 x86_64; do
            BD="$DIR/build-pkg-$ARCH"
            "$CMAKE" -S "$DIR" -B "$BD" -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_OSX_ARCHITECTURES=$ARCH -DVEXDB_SQLITE_BUILD_TESTS=OFF \
                "${BOOST_CMAKE_ARGS[@]}"
            "$CMAKE" --build "$BD" -j "$NCPU"
            PKG="vexdb-lite-sqlite-${VERSION}-macos-${ARCH}"
            STAGE="$DIST/$PKG"
            rm -rf "$STAGE"
            mkdir -p "$STAGE"
            cp "$BD/vexdb_lite.dylib" "$STAGE/"
            cp "$BD/libvexdb_lite_static.a" "$STAGE/"
            cp "$DIR/include/vexdb_sqlite.h" "$STAGE/"
            cp "$DIR/README.md" "$STAGE/"
            # macOS bsdtar 会把 provenance/resource fork 写成隐藏的
            # AppleDouble (`._*`) 条目。Linux 解包后这些会变成真实文件，
            # 因此从源头禁用并保留排除规则作为双保险。
            COPYFILE_DISABLE=1 tar --exclude='._*' --exclude='.DS_Store' \
                -C "$DIST" -czf "$DIST/$PKG.tar.gz" "$PKG"
            rm -rf "$STAGE"
            echo "打包: $DIST/$PKG.tar.gz"
        done
        ( cd "$DIST" && shasum -a 256 *.tar.gz > SHA256SUMS )
        echo ""
        ls -la "$DIST"/*.tar.gz "$DIST/SHA256SUMS"
        ;;
    ios)
        # Stage C M9-iOS：arm64 device + arm64 simulator 静态库 → XCFramework。
        # 静态注册形态（VEXDB_SQLITE_CORE）：App 自备 SQLite（系统 libsqlite3
        # 禁扩展加载），链接时解析 sqlite3_* 符号。sim 构建带 smoke 二进制，
        # 有可用模拟器则 simctl spawn 跑通静态注册链路。
        [ -f "$DIR/third_party/sqlite/sqlite3ext.h" ] || bash "$DIR/vendor_sqlite.sh"
        VERSION="${VEXDB_SQLITE_VERSION:-0.1.0}"
        DIST="$DIR/../dist/sqlite"
        mkdir -p "$DIST"
        IOS_MIN="${IOS_DEPLOYMENT_TARGET:-13.0}"
        for VARIANT in device sim; do
            BD="$DIR/build-ios-$VARIANT"
            if [ "$VARIANT" = "device" ]; then
                SYSROOT=iphoneos; TESTS=OFF
            else
                SYSROOT=iphonesimulator; TESTS=ON
            fi
            "$CMAKE" -S "$DIR" -B "$BD" -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
                -DCMAKE_OSX_SYSROOT=$SYSROOT \
                -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" \
                -DVEXDB_SQLITE_BUILD_TESTS=$TESTS \
                "${BOOST_CMAKE_ARGS[@]}"
            "$CMAKE" --build "$BD" -j "$NCPU"
        done
        XCF="$DIST/vexdb_lite.xcframework"
        rm -rf "$XCF"
        HDRS="$DIR/build-ios-headers"
        rm -rf "$HDRS" && mkdir -p "$HDRS"
        cp "$DIR/include/vexdb_sqlite.h" "$HDRS/"
        xcodebuild -create-xcframework \
            -library "$DIR/build-ios-device/libvexdb_lite_static.a" -headers "$HDRS" \
            -library "$DIR/build-ios-sim/libvexdb_lite_static.a" -headers "$HDRS" \
            -output "$XCF"
        echo ""
        echo "=== XCFramework ==="
        du -sh "$XCF"
        lipo -info "$DIR/build-ios-device/libvexdb_lite_static.a"
        # sim smoke（best effort：需要一台已启动的模拟器）
        BOOTED=$(xcrun simctl list devices booted 2>/dev/null | grep -c Booted || true)
        if [ "$BOOTED" -gt 0 ]; then
            echo "=== iOS Simulator 静态注册冒烟 ==="
            xcrun simctl spawn booted \
                "$DIR/build-ios-sim/m0_static_smoke.app/m0_static_smoke" && \
            xcrun simctl spawn booted \
                "$DIR/build-ios-sim/m3_hnsw_smoke.app/m3_hnsw_smoke" || \
                echo "sim smoke 失败（产物本身已构建成功）"
        else
            echo "（无已启动的模拟器，跳过 sim smoke：xcrun simctl boot <device> 后重跑）"
        fi
        ;;
    android)
        # Stage C M10-Android：NDK 交叉 arm64-v8a + x86_64。
        # 产物：libvexdb_lite_static.a（静态集成）+ vexdb_lite.so（loadable，
        # 给 requery 等开 load_extension 的自带-SQLite 宿主）。
        [ -f "$DIR/third_party/sqlite/sqlite3ext.h" ] || bash "$DIR/vendor_sqlite.sh"
        NDK="${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* 2>/dev/null | sort -V | tail -1)}"
        [ -d "$NDK" ] || { echo "找不到 Android NDK：设 ANDROID_NDK_HOME" >&2; exit 1; }
        echo "NDK: $NDK"
        VERSION="${VEXDB_SQLITE_VERSION:-0.1.0}"
        DIST="$DIR/../dist/sqlite/android"
        ANDROID_API="${ANDROID_PLATFORM:-24}"
        for ABI in arm64-v8a x86_64; do
            BD="$DIR/build-android-$ABI"
            # NDK toolchain 默认注入 -g（debug info 留给 ndk-stack，AGP 打包才
            # strip）；直接交付产物用 -g0 后置覆盖（.a 35-70MB → ~3MB）。
            "$CMAKE" -S "$DIR" -B "$BD" -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
                -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$ANDROID_API \
                -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -g0" \
                -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -g0" \
                -DVEXDB_SQLITE_BUILD_TESTS=OFF \
                "${BOOST_CMAKE_ARGS[@]}"
            "$CMAKE" --build "$BD" -j "$NCPU"
            mkdir -p "$DIST/$ABI"
            cp "$BD/libvexdb_lite_static.a" "$BD/vexdb_lite.so" "$DIST/$ABI/"
            STRIP="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | head -1)"
            [ -x "$STRIP" ] && "$STRIP" --strip-unneeded "$DIST/$ABI/vexdb_lite.so" \
                            && "$STRIP" --strip-debug "$DIST/$ABI/libvexdb_lite_static.a"
        done
        cp "$DIR/include/vexdb_sqlite.h" "$DIST/"
        echo ""
        echo "=== Android 产物 ==="
        ls -la "$DIST"/*/
        ;;
    wasm)
        WASM_DIR="$DIR/../examples/wasm"
        bash "$WASM_DIR/build.sh"
        npm --prefix "$WASM_DIR" test
        echo ""
        echo "可直接打开: $WASM_DIR/dist/vexdb-lite-demo.html"
        echo "开发调试服务: npm --prefix $WASM_DIR run serve"
        ;;
    clean)
        rm -rf "$BUILD_DIR" "$DIR"/build-pkg-* "$DIR"/build-ios-* "$DIR"/build-android-*
        echo "已清理 build 目录"
        ;;
    *)
        echo "Usage: $0 [build|test|benchmark|package|ios|android|wasm|vendor|clean]"
        exit 1
        ;;
esac
