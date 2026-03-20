#!/bin/bash
set -e

DUCKDB_DIR="$(cd "$(dirname "$0")" && pwd)"
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

usage() {
    echo "Usage: $0 [dev|release|test|clean|ios|android|wasm] [options]"
    echo ""
    echo "Commands:"
    echo "  dev       开发环境编译 (Debug, 含单元测试)"
    echo "  release   发布环境编译 (Release, 优化)"
    echo "  test      编译并运行测试"
    echo "  clean     清理构建目录"
    echo "  ios       iOS 交叉编译 (arm64)"
    echo "  android   Android 交叉编译 (arm64-v8a)"
    echo "  wasm      WebAssembly 编译 (需要 Emscripten)"
    echo ""
    echo "Options:"
    echo "  -j N      并行编译线程数 (默认: $NCPU)"
    echo "  -t TGT    编译目标 (默认: dev→unittest, release→duckdb)"
    echo "  --asan    启用 AddressSanitizer (仅 dev)"
    echo "  --filter  测试过滤 (仅 test 命令)"
    echo "  --sim     iOS Simulator 编译 (仅 ios 命令)"
    echo "  --abi ABI Android ABI (默认: arm64-v8a, 仅 android 命令)"
    echo "  --profile P 构建方案: full(默认,~11MB) | compact(~8MB) | minimal(~6MB)"
    echo ""
    echo "Examples:"
    echo "  $0 dev                    # Debug 编译"
    echo "  $0 release                # Release 编译"
    echo "  $0 test                   # 编译并运行所有 vex 测试"
    echo "  $0 test --filter 'graph'  # 运行匹配 graph 的测试"
    echo "  $0 dev -j4                # 4线程 Debug 编译"
    echo "  $0 dev --asan             # ASan Debug 编译"
    echo "  $0 clean                  # 清理所有构建"
    echo "  $0 ios                    # iOS arm64 编译"
    echo "  $0 ios --sim              # iOS Simulator 编译"
    echo "  $0 android                # Android arm64-v8a 编译"
    echo "  $0 android --abi x86_64   # Android x86_64 编译"
    echo "  $0 wasm                   # WebAssembly 编译"
    echo "  $0 ios --profile compact  # iOS 精简版 (~8MB)"
    echo "  $0 ios --profile minimal  # iOS 最小版 (~6MB)"
}

cmake_configure() {
    local build_dir="$1"
    local build_type="$2"
    shift 2
    local extra_flags=("$@")

    mkdir -p "$build_dir"
    echo -e "${YELLOW}[Configure] $build_type → $build_dir${NC}"
    cmake -S "$DUCKDB_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DBUILD_UNITTESTS=ON \
        -DEXTENSION_STATIC_BUILD=OFF \
        "${extra_flags[@]}"
}

cmake_build() {
    local build_dir="$1"
    local target="$2"
    local jobs="$3"

    echo -e "${YELLOW}[Build] target=$target jobs=$jobs${NC}"
    cmake --build "$build_dir" --target "$target" -j "$jobs"
    echo -e "${GREEN}[Done] Build successful${NC}"
}

# 默认值
CMD="${1:-dev}"
shift 2>/dev/null || true

JOBS="$NCPU"
TARGET=""
ASAN=0
FILTER=""
SIM=0
ABI="arm64-v8a"

# 解析选项
while [[ $# -gt 0 ]]; do
    case "$1" in
        -j)
            [[ -z "${2:-}" || "${2:-}" == -* ]] && echo -e "${RED}Error: -j requires a number${NC}" && exit 1
            JOBS="$2"; shift 2 ;;
        -t)
            [[ -z "${2:-}" || "${2:-}" == -* ]] && echo -e "${RED}Error: -t requires a target name${NC}" && exit 1
            TARGET="$2"; shift 2 ;;
        --asan) ASAN=1; shift ;;
        --filter)
            [[ -z "${2:-}" || "${2:-}" == -* ]] && echo -e "${RED}Error: --filter requires a pattern${NC}" && exit 1
            FILTER="$2"; shift 2 ;;
        --sim) SIM=1; shift ;;
        --abi)
            [[ -z "${2:-}" || "${2:-}" == -* ]] && echo -e "${RED}Error: --abi requires an ABI name${NC}" && exit 1
            ABI="$2"; shift 2 ;;
        --profile)
            [[ -z "${2:-}" || "${2:-}" == -* ]] && echo -e "${RED}Error: --profile requires: full|compact|minimal${NC}" && exit 1
            PROFILE="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo -e "${RED}Unknown option: $1${NC}"; usage; exit 1 ;;
    esac
done

MOBILE_EXT_CONFIG="$DUCKDB_DIR/extension/extension_config_mobile.cmake"

# ============================================================
# 移动端构建方案（--profile 选择）
# ============================================================
# full:    完整功能 — HNSW + HybridIndex + Optimizer + core_functions
#          适用于需要完整 SQL 分析能力的场景
# compact: 纯向量搜索 — HNSW + core_functions（无 HybridIndex/Optimizer）
#          适用于只需要向量搜索 + 基本 SQL 的场景
# minimal: 最小依赖 — 仅 HNSW（无 core_functions，基础 SQL 仍可用）
#          适用于只需要向量搜索、不依赖高级 SQL 函数的场景
#
# 体积说明：
#   最终二进制体积主要取决于链接器 dead_strip（自动移除未引用代码）。
#   不同 profile 的编译产物(.a)大小接近，但集成到 app 后由 app 实际
#   调用的功能决定最终体积。full/compact/minimal 的区别在于功能集承诺，
#   而非直接的体积差异。
#
# 平台差异：
#   iOS (arm64):   ~11MB (strip后) — Apple Clang + dead_strip
#   Android (arm64-v8a): 预估 ~12-14MB — NDK Clang + gc-sections
#   WASM:          预估 ~8-10MB  — Emscripten 有额外的 wasm-opt 优化
#   macOS/Linux:   不适用（桌面端用 release 构建，动态链接）
PROFILE="${PROFILE:-full}"

# 公共编译参数
MOBILE_CMAKE_FLAGS=(
    -DCMAKE_BUILD_TYPE=MinSizeRel
    -DCMAKE_C_FLAGS_MINSIZEREL="-Oz -DNDEBUG -ffunction-sections -fdata-sections -fvisibility=hidden"
    -DCMAKE_CXX_FLAGS_MINSIZEREL="-Oz -DNDEBUG -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden"
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
    -DBUILD_SHELL=OFF
    -DBUILD_UNITTESTS=OFF
    -DENABLE_EXTENSION_AUTOLOADING=OFF
    -DENABLE_EXTENSION_AUTOINSTALL=OFF
    -DEXTENSION_STATIC_BUILD=ON
    -DVEX_MOBILE_MODE=ON
    -DDISABLE_THREADS=ON
    -DSMALLER_BINARY=ON
    -DSKIP_EXTENSIONS="parquet;jemalloc"
)

# 按 profile 追加参数
case "$PROFILE" in
    compact)
        # 精简版：移除 HybridIndex 和 Optimizer（VEX 核心搜索保留）
        MOBILE_CMAKE_FLAGS+=(
            -DVEX_ENABLE_HYBRID_INDEX=OFF
            -DVEX_ENABLE_OPTIMIZER=OFF
        )
        echo -e "${YELLOW}[Profile] compact — 无 HybridIndex/Optimizer${NC}"
        ;;
    minimal)
        # 最小版：compact + 跳过 core_functions 扩展
        MOBILE_CMAKE_FLAGS+=(
            -DVEX_ENABLE_HYBRID_INDEX=OFF
            -DVEX_ENABLE_OPTIMIZER=OFF
        )
        MOBILE_EXT_CONFIG="$DUCKDB_DIR/extension/extension_config_minimal.cmake"
        echo -e "${YELLOW}[Profile] minimal — 最小体积${NC}"
        ;;
    full|*)
        echo -e "${YELLOW}[Profile] full — 完整功能${NC}"
        ;;
esac

case "$CMD" in
    dev)
        BUILD_DIR="$DUCKDB_DIR/build/debug"
        TARGET="${TARGET:-unittest}"
        EXTRA_FLAGS=()
        if [[ $ASAN -eq 1 ]]; then
            BUILD_DIR="$DUCKDB_DIR/build/asan"
            EXTRA_FLAGS+=(
                -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
                -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
                -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
            )
        fi
        cmake_configure "$BUILD_DIR" "Debug" "${EXTRA_FLAGS[@]}"
        cmake_build "$BUILD_DIR" "$TARGET" "$JOBS"
        echo -e "${GREEN}Binary: $BUILD_DIR/test/unittest${NC}"
        ;;

    release)
        BUILD_DIR="$DUCKDB_DIR/build/release"
        TARGET="${TARGET:-duckdb}"
        cmake_configure "$BUILD_DIR" "Release"
        cmake_build "$BUILD_DIR" "$TARGET" "$JOBS"
        echo -e "${GREEN}Binary: $BUILD_DIR/src/duckdb${NC}"
        ;;

    test)
        BUILD_DIR="$DUCKDB_DIR/build/release"
        TARGET="unittest"
        # 确保已配置
        if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
            cmake_configure "$BUILD_DIR" "Release"
        fi
        cmake_build "$BUILD_DIR" "$TARGET" "$JOBS"

        TEST_FILTER="${FILTER:-test/sql/vex/*}"
        echo -e "${YELLOW}[Test] filter=$TEST_FILTER${NC}"
        "$BUILD_DIR/test/unittest" "$TEST_FILTER"
        echo -e "${GREEN}[Done] All tests passed${NC}"
        ;;

    clean)
        echo -e "${YELLOW}[Clean] Removing build directories...${NC}"
        rm -rf "$DUCKDB_DIR/build/debug"
        rm -rf "$DUCKDB_DIR/build/release"
        rm -rf "$DUCKDB_DIR/build/asan"
        rm -rf "$DUCKDB_DIR/build/ios_arm64"
        rm -rf "$DUCKDB_DIR/build/ios_sim"
        rm -rf "$DUCKDB_DIR/build/android_"*
        rm -rf "$DUCKDB_DIR/build/wasm"
        echo -e "${GREEN}[Done] Clean complete${NC}"
        ;;

    ios)
        PROFILE_SUFFIX=""
        [[ "$PROFILE" != "full" ]] && PROFILE_SUFFIX="_${PROFILE}"
        if [[ $SIM -eq 1 ]]; then
            BUILD_DIR="$DUCKDB_DIR/build/ios_sim${PROFILE_SUFFIX}"
            echo -e "${YELLOW}[iOS] Building for Simulator${NC}"
            IOS_PLATFORM="SIMULATOR64"
        else
            BUILD_DIR="$DUCKDB_DIR/build/ios_arm64${PROFILE_SUFFIX}"
            echo -e "${YELLOW}[iOS] Building for Device (arm64)${NC}"
            IOS_PLATFORM="OS"
        fi

        mkdir -p "$BUILD_DIR"
        echo -e "${YELLOW}[Configure] iOS MinSizeRel → $BUILD_DIR${NC}"
        cmake -S "$DUCKDB_DIR" -B "$BUILD_DIR" \
            "${MOBILE_CMAKE_FLAGS[@]}" \
            -DCMAKE_TOOLCHAIN_FILE="$DUCKDB_DIR/scripts/ios-toolchain.cmake" \
            -DIOS_PLATFORM="$IOS_PLATFORM" \
            "${MOBILE_EXT_CONFIG:+-DDUCKDB_EXTENSION_CONFIGS=$MOBILE_EXT_CONFIG}"

        echo -e "${YELLOW}[Build] Building all targets...${NC}"
        cmake --build "$BUILD_DIR" --target duckdb_static vex_extension core_functions_extension duckdb_generated_extension_loader -j "$JOBS"

        # 合并为单个 libvexdb.a
        LIBVEXDB_DIR="$BUILD_DIR/libvexdb"
        mkdir -p "$LIBVEXDB_DIR"
        echo -e "${YELLOW}[Package] Merging into libvexdb.a...${NC}"
        xcrun libtool -static -o "$LIBVEXDB_DIR/libvexdb.a" \
            "$BUILD_DIR/extension/vex/libvex_extension.a" \
            "$BUILD_DIR/extension/core_functions/libcore_functions_extension.a" \
            "$BUILD_DIR/extension/libduckdb_generated_extension_loader.a" \
            "$BUILD_DIR/src/libduckdb_static.a" \
            $(find "$BUILD_DIR/third_party" -name "*.a")

        # 复制头文件
        mkdir -p "$LIBVEXDB_DIR/include"
        cp -r "$DUCKDB_DIR/src/include/duckdb" "$LIBVEXDB_DIR/include/"
        cp "$BUILD_DIR/src/include/duckdb/amalgamation_defs.hpp" "$LIBVEXDB_DIR/include/duckdb/" 2>/dev/null || true
        cp "$DUCKDB_DIR/src/include/duckdb.hpp" "$LIBVEXDB_DIR/include/"

        echo -e "${GREEN}[iOS] Output:${NC}"
        echo -e "${GREEN}  Library: $LIBVEXDB_DIR/libvexdb.a ($(du -h "$LIBVEXDB_DIR/libvexdb.a" | cut -f1))${NC}"
        echo -e "${GREEN}  Headers: $LIBVEXDB_DIR/include/${NC}"
        echo -e "${GREEN}  Usage: clang++ -I include/ -L . -lvexdb -lc++ -framework Foundation${NC}"
        ;;

    android)
        PROFILE_SUFFIX=""
        [[ "$PROFILE" != "full" ]] && PROFILE_SUFFIX="_${PROFILE}"
        BUILD_DIR="$DUCKDB_DIR/build/android_${ABI}${PROFILE_SUFFIX}"

        echo -e "${YELLOW}[Android] Building for ABI=$ABI${NC}"
        mkdir -p "$BUILD_DIR"
        echo -e "${YELLOW}[Configure] Android MinSizeRel → $BUILD_DIR${NC}"
        cmake -S "$DUCKDB_DIR" -B "$BUILD_DIR" \
            "${MOBILE_CMAKE_FLAGS[@]}" \
            -DCMAKE_TOOLCHAIN_FILE="$DUCKDB_DIR/scripts/android-toolchain.cmake" \
            -DANDROID_ABI="$ABI" \
            "${MOBILE_EXT_CONFIG:+-DDUCKDB_EXTENSION_CONFIGS=$MOBILE_EXT_CONFIG}"

        echo -e "${YELLOW}[Build] Building all targets...${NC}"
        cmake --build "$BUILD_DIR" --target duckdb_static vex_extension core_functions_extension duckdb_generated_extension_loader -j "$JOBS"

        # 合并为单个 libvexdb.a
        LIBVEXDB_DIR="$BUILD_DIR/libvexdb"
        mkdir -p "$LIBVEXDB_DIR"
        echo -e "${YELLOW}[Package] Merging into libvexdb.a...${NC}"
        ar crsT "$LIBVEXDB_DIR/libvexdb.a" \
            "$BUILD_DIR/extension/vex/libvex_extension.a" \
            "$BUILD_DIR/extension/core_functions/libcore_functions_extension.a" \
            "$BUILD_DIR/extension/libduckdb_generated_extension_loader.a" \
            "$BUILD_DIR/src/libduckdb_static.a" \
            $(find "$BUILD_DIR/third_party" -name "*.a")

        echo -e "${GREEN}[Android] Output:${NC}"
        echo -e "${GREEN}  Library: $LIBVEXDB_DIR/libvexdb.a ($(du -h "$LIBVEXDB_DIR/libvexdb.a" | cut -f1))${NC}"
        ;;

    wasm)
        TARGET="${TARGET:-duckdb_static}"
        BUILD_DIR="$DUCKDB_DIR/build/wasm"

        if ! command -v emcmake &>/dev/null; then
            echo -e "${RED}Error: emcmake not found. Install and activate Emscripten SDK first.${NC}"
            exit 1
        fi

        echo -e "${YELLOW}[WASM] Building with Emscripten${NC}"
        mkdir -p "$BUILD_DIR"
        echo -e "${YELLOW}[Configure] WASM Release → $BUILD_DIR${NC}"
        emcmake cmake -S "$DUCKDB_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_FLAGS="-pthread -msimd128" \
            -DCMAKE_CXX_FLAGS="-pthread -msimd128" \
            -DBUILD_SHELL=OFF \
            -DBUILD_UNITTESTS=OFF \
            -DENABLE_EXTENSION_AUTOLOADING=OFF \
            -DENABLE_EXTENSION_AUTOINSTALL=OFF \
            -DEXTENSION_STATIC_BUILD=ON \
            -DVEX_MOBILE_MODE=ON \
            -DWASM_ENABLED=ON \
            -DSKIP_EXTENSIONS="parquet;jemalloc" \
            "${MOBILE_EXT_CONFIG:+-DDUCKDB_EXTENSION_CONFIGS=$MOBILE_EXT_CONFIG}"

        cmake --build "$BUILD_DIR" --target "$TARGET" -j "$JOBS"
        echo -e "${GREEN}[WASM] Output: $BUILD_DIR${NC}"
        ;;

    -h|--help)
        usage
        ;;

    *)
        echo -e "${RED}Unknown command: $CMD${NC}"
        usage
        exit 1
        ;;
esac
