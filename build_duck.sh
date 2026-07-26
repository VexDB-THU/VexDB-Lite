#!/usr/bin/env bash
# build_duck.sh — manage the vexdb_duckdb DuckDB extension build under build/duck/
#
# Usage:
#   ./build_duck.sh setup          # clone DuckDB ($DUCKDB_VERSION) + cmake configure
#
# Target DuckDB version is set via the DUCKDB_VERSION env var (default v1.5.2).
# C++ extensions are version-locked, so build one per target version, e.g.:
#   DUCKDB_VERSION=v1.5.0 ./build_duck.sh setup build strip
#   DUCKDB_VERSION=v1.5.2 ./build_duck.sh setup build strip
# Each version gets its own build/duck/<version>/ tree; datasets are shared.
#   ./build_duck.sh build          # incremental build of vex extension + static libs
#   ./build_duck.sh bin            # compile smoke + benchmark binaries
#   ./build_duck.sh data           # convert SIFT HDF5 → fbin (one-time)
#   ./build_duck.sh smoke          # run smoke_create_index
#   ./build_duck.sh explain        # run explain_literal_query
#   ./build_duck.sh crash-recovery # run hard-kill DuckDB graph index recovery checks
#   ./build_duck.sh crash-recovery-matrix # run crash-recovery for DUCKDB_VERSIONS
#   ./build_duck.sh bench-10k      # run SIFT 10k benchmark
#   ./build_duck.sh bench-100k     # run SIFT 100k benchmark
#   ./build_duck.sh bench-rabitq   # run generated-data RaBitQ benchmark
#   ./build_duck.sh bench          # run both 10k + 100k
#   ./build_duck.sh build-unittest # compile DuckDB unittest binary
#   ./build_duck.sh unittest [pat] # run sqllogic .test files (default 'test/sql/vex/*')
#   ./build_duck.sh strip          # strip extension + re-append metadata footer (release flow)
#   ./build_duck.sh all            # setup + data + build + bin + smoke + bench-10k
#   ./build_duck.sh clean          # rm build/ (keep src + data)
#   ./build_duck.sh purge          # rm build/ + duckdb_src/ (keep data)
#   ./build_duck.sh status         # show disk usage
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Target DuckDB version. C++ extensions have no stable ABI, so one build only
# works against the exact DuckDB version it was compiled for. Override to build
# for another release, e.g.:  DUCKDB_VERSION=v1.5.0 ./build_duck.sh setup build
DUCKDB_VERSION="${DUCKDB_VERSION:-v1.5.2}"
# Each target version gets its own source + build tree under build/duck/<version>/
# so multiple DuckDB versions can be built and cached side by side (full matrix).
DUCK_BASE="$PROJECT_DIR/build/duck"
DUCK_ROOT="$DUCK_BASE/$DUCKDB_VERSION"
DUCK_SRC="$DUCK_ROOT/duckdb_src"
DUCK_BUILD="$DUCK_ROOT/build"
DUCK_BIN="$DUCK_ROOT/bin"
# Benchmark datasets are version-independent — share one copy across versions.
DUCK_DATA="$DUCK_BASE/data"

VEX_SRC="$PROJECT_DIR/vexdb_duckdb"
VEX_INCLUDE="$VEX_SRC/include"
VEX_TEST="$VEX_SRC/test"
EXTENSION_PATH="$DUCK_BUILD/extension/vexdb_lite/vexdb_lite.duckdb_extension"
LIBDUCKDB="$DUCK_BUILD/src/libduckdb_static.a"
LIBLOADER="$DUCK_BUILD/extension/libdummy_static_extension_loader.a"
LIBCORE="$DUCK_BUILD/extension/core_functions/libcore_functions_extension.a"
LIBVEX="$DUCK_BUILD/extension/vexdb_lite/libvexdb_lite_extension.a"

ANN_HDF5_SRC="${ANN_HDF5_SRC:-}"  # path to sift-128-euclidean.hdf5 from ann-benchmarks

NCPU="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)"
CXX_BIN="${CXX:-c++}"

YEL='\033[1;33m'; GRN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
info()  { printf '%b[duck]%b %s\n' "$YEL" "$NC" "$*"; }
ok()    { printf '%b[duck]%b %s\n' "$GRN" "$NC" "$*"; }
fail()  { printf '%b[duck]%b %s\n' "$RED" "$NC" "$*" >&2; exit 1; }

cmd_setup() {
    mkdir -p "$DUCK_ROOT" "$DUCK_BIN" "$DUCK_DATA"
    # Version stamp written by us — git is unreliable here: release.sh leaves an
    # empty .git placeholder and rsync'd trees have no tag, so `git describe`
    # silently resolves to the PARENT repo's tag. A plain file is the source of
    # truth for "which DuckDB tag this src tree is".
    local stamp="$DUCK_SRC/.vexdb_target_version"
    if [[ ! -e "$DUCK_SRC/CMakeLists.txt" ]]; then
        info "shallow-cloning DuckDB $DUCKDB_VERSION → $DUCK_SRC"
        rm -rf "$DUCK_SRC"
        git clone --depth=1 --branch="$DUCKDB_VERSION" https://github.com/duckdb/duckdb.git "$DUCK_SRC"
        echo "$DUCKDB_VERSION" > "$stamp"
    else
        local have_ver
        have_ver="$(cat "$stamp" 2>/dev/null || echo unknown)"
        info "DuckDB source already at $DUCK_SRC (stamp: $have_ver)"
        # Guard against the "thought I built v1.5.0 but it's still v1.5.2" trap.
        # Unstamped legacy/rsync'd trees pass through (assumed correct); stamped
        # trees that disagree with DUCKDB_VERSION abort and ask for a purge.
        if [[ "$have_ver" != "unknown" && "$have_ver" != "$DUCKDB_VERSION" ]]; then
            fail "existing DuckDB src is '$have_ver' but DUCKDB_VERSION=$DUCKDB_VERSION. Run: $0 purge  (then setup again)"
        fi
    fi

    cat > "$DUCK_SRC/extension/extension_config_local.cmake" <<EOF
duckdb_extension_load(vexdb_lite
    SOURCE_DIR $VEX_SRC
    INCLUDE_DIR $VEX_INCLUDE
    LOAD_TESTS
)
EOF

    info "cmake configure → $DUCK_BUILD"
    cmake -S "$DUCK_SRC" -B "$DUCK_BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHELL=OFF \
        -DBUILD_UNITTESTS=ON \
        -DENABLE_EXTENSION_AUTOLOADING=OFF \
        -DENABLE_EXTENSION_AUTOINSTALL=OFF \
        -DOVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION" \
        -DDUCKDB_EXTENSION_CONFIGS="$DUCK_SRC/extension/extension_config_local.cmake"
    ok "setup complete"
}

cmd_build() {
    [[ -f "$DUCK_BUILD/CMakeCache.txt" ]] || cmd_setup
    info "building vexdb_lite_loadable_extension + vexdb_lite_extension + core_functions + duckdb_static + dummy_loader (j=$NCPU)"
    cmake --build "$DUCK_BUILD" \
        --target vexdb_lite_loadable_extension vexdb_lite_extension core_functions_extension duckdb_static dummy_static_extension_loader \
        -j "$NCPU"
    [[ -f "$EXTENSION_PATH" ]] || fail "build finished but extension missing: $EXTENSION_PATH"
    ok "build done: $EXTENSION_PATH ($(du -h "$EXTENSION_PATH" | cut -f1))"
}

# strip vexdb_lite.duckdb_extension and re-append metadata footer.
#
# DuckDB extension metadata is a 534-byte raw-bytes footer appended after the
# ELF tables by append_metadata.cmake (POST_BUILD). `strip` rewrites the ELF
# and truncates everything past the ELF section table — so the footer is lost
# whenever the .duckdb_extension is stripped. The DuckDB loader rejects the
# file with "metadata at the end of the file is invalid" if the footer is
# missing, so any release flow that strips MUST re-seal the footer.
#
# Following DuckDB upstream's stance: ship a single stripped + footered file,
# no separate .debug. We still archive an unstripped copy locally for our own
# gdb work (see UNSTRIPPED_OUT below). Customer crashes that need symbol-level
# investigation are diagnosed off-band with that local copy.
cmd_strip() {
    [[ -f "$EXTENSION_PATH" ]] || fail "extension not built; run: $0 build"

    local strip_bin="${STRIP:-strip}"
    command -v "$strip_bin" >/dev/null 2>&1 || fail "strip not found: $strip_bin"
    command -v cmake >/dev/null 2>&1 || fail "cmake not in PATH"

    # Optional: archive unstripped copy for internal debugging. Caller sets
    # UNSTRIPPED_OUT to override (release.sh points this to dist/<arch>/).
    if [[ -n "${UNSTRIPPED_OUT:-}" ]]; then
        mkdir -p "$(dirname "$UNSTRIPPED_OUT")"
        cp "$EXTENSION_PATH" "$UNSTRIPPED_OUT"
        info "unstripped archived → $UNSTRIPPED_OUT ($(du -h "$UNSTRIPPED_OUT" | cut -f1))"
    fi

    local before_size after_size
    before_size=$(stat -c%s "$EXTENSION_PATH" 2>/dev/null || stat -f%z "$EXTENSION_PATH")
    info "strip --strip-unneeded (before: $((before_size/1024/1024)) MB)"
    "$strip_bin" --strip-unneeded "$EXTENSION_PATH"
    after_size=$(stat -c%s "$EXTENSION_PATH" 2>/dev/null || stat -f%z "$EXTENSION_PATH")
    info "  → $((after_size/1024/1024)) MB"

    # Re-append metadata footer using the exact same invocation that cmake's
    # POST_BUILD step would have used. Parameters mirror append_metadata.cmake
    # registration in extension/extension_build_tools.cmake:208-213. ABI_TYPE
    # is "CPP" (the default for duckdb_extension_load); VERSION_FIELD is the
    # DuckDB tag the extension links against (DUCKDB_NORMALIZED_VERSION).
    local platform_file="$DUCK_BUILD/duckdb_platform_out"
    local null_file="$DUCK_SRC/scripts/null.txt"
    local append_cmake="$DUCK_SRC/scripts/append_metadata.cmake"
    [[ -f "$platform_file" ]] || fail "platform file missing: $platform_file"
    [[ -f "$null_file"     ]] || fail "null file missing: $null_file"
    [[ -f "$append_cmake"  ]] || fail "append_metadata.cmake missing: $append_cmake"

    info "re-append metadata footer (platform=$(cat "$platform_file"))"
    cmake \
        -DABI_TYPE=CPP \
        -DEXTENSION="$EXTENSION_PATH" \
        -DPLATFORM_FILE="$platform_file" \
        -DVERSION_FIELD="$DUCKDB_VERSION" \
        -DEXTENSION_VERSION="" \
        -DNULL_FILE="$null_file" \
        -P "$append_cmake"

    # Verify the footer landed (canary: append_metadata.cmake writes the
    # literal string "duckdb_signature" as part of the custom section name).
    if ! grep -aq "duckdb_signature" "$EXTENSION_PATH"; then
        fail "footer re-seal failed: duckdb_signature marker not found"
    fi

    # Workaround upstream DuckDB cmake bug: append_metadata.cmake's NULL-byte
    # padding is empty, so loader rejects the footer. See scripts/fix_duckdb_footer.py.
    local fix_script="$(dirname "$(readlink -f "$0" 2>/dev/null || echo "$0")")/scripts/fix_duckdb_footer.py"
    [[ -f "$fix_script" ]] || fail "fix_duckdb_footer.py missing: $fix_script"
    info "post-fix footer padding (DuckDB cmake bug workaround)"
    python3 "$fix_script" "$EXTENSION_PATH" "$(cat "$platform_file")" "$DUCKDB_VERSION" CPP ""
    local final_size
    final_size=$(stat -c%s "$EXTENSION_PATH" 2>/dev/null || stat -f%z "$EXTENSION_PATH")
    ok "strip+reseal done: $EXTENSION_PATH ($((final_size/1024/1024)) MB, footer present)"
}

cmd_build_unittest() {
    [[ -f "$DUCK_BUILD/CMakeCache.txt" ]] || cmd_setup
    info "building unittest (j=$NCPU)"
    cmake --build "$DUCK_BUILD" --target unittest -j "$NCPU"
    [[ -f "$DUCK_BUILD/test/unittest" ]] || fail "unittest binary missing: $DUCK_BUILD/test/unittest"
    ok "unittest built: $DUCK_BUILD/test/unittest"
}

cmd_unittest() {
    [[ -f "$DUCK_BUILD/test/unittest" ]] || cmd_build_unittest
    local pattern="${1:-test/sql/vex/*}"
    info "running unittest filter='$pattern'"
    "$DUCK_BUILD/test/unittest" "$pattern"
}

_compile_one() {
    local src="$1" out="$2"; shift 2
    local boost_include
    boost_include=$(sed -n 's/^Boost_INCLUDE_DIR:PATH=//p' "$DUCK_BUILD/CMakeCache.txt" | head -1)
    [[ -f "$src" ]]      || fail "missing source: $src"
    [[ -f "$LIBDUCKDB" ]]|| fail "missing $LIBDUCKDB — run: $0 build"
    [[ -f "$LIBLOADER" ]]|| fail "missing $LIBLOADER — run: $0 build"
    [[ -n "$boost_include" && -f "$boost_include/boost/preprocessor/seq.hpp" ]] \
        || fail "Boost include path missing from DuckDB CMake cache"
    info "compile $(basename "$src") → $(basename "$out")"
    "$CXX_BIN" -std=c++17 -O2 \
        "$src" \
        -I"$DUCK_SRC/src/include" \
        -I"$DUCK_SRC/extension/core_functions/include" \
        -I"$VEX_INCLUDE" \
        -I"$boost_include" \
        "$@" \
        "$LIBDUCKDB" \
        "$LIBLOADER" \
        -o "$out"
}

cmd_bin() {
    cmd_build  # ensures static libs exist (incremental, fast if up-to-date)
    _compile_one "$VEX_TEST/smoke_create_index.cpp"        "$DUCK_BIN/smoke_create_index"
    _compile_one "$VEX_TEST/explain_literal_query.cpp"     "$DUCK_BIN/explain_literal_query"        "$LIBCORE"
    _compile_one "$VEX_TEST/crash_recovery.cpp"            "$DUCK_BIN/crash_recovery"               "$LIBVEX" "$LIBCORE"
    _compile_one "$VEX_TEST/create_legacy_quantizer_fixture.cpp" "$DUCK_BIN/create_legacy_quantizer_fixture" "$LIBCORE"
    _compile_one "$VEX_TEST/rabitq_reconstruct_smoke.cpp"  "$DUCK_BIN/rabitq_reconstruct_smoke" \
        -DPG_VEXDB_TARGET_DUCK -I"$PROJECT_DIR/common" -I"$PROJECT_DIR/common/include" \
        "$LIBVEX" "$LIBCORE"
    _compile_one "$VEX_TEST/pq_failure_cleanup_smoke.cpp" "$DUCK_BIN/pq_failure_cleanup_smoke" \
        -DPG_VEXDB_TARGET_DUCK -I"$PROJECT_DIR/common" -I"$PROJECT_DIR/common/include" \
        "$LIBVEX" "$LIBCORE"
    _compile_one "$VEX_TEST/benchmark/vex_sift_sql_benchmark.cpp" "$DUCK_BIN/vex_sift_sql_benchmark" "$LIBCORE"
    _compile_one "$VEX_TEST/benchmark/vex_rabitq_synthetic_benchmark.cpp" "$DUCK_BIN/vex_rabitq_synthetic_benchmark" "$LIBCORE"
    ok "binaries built under $DUCK_BIN/"
}

cmd_data() {
    if [[ -f "$DUCK_DATA/sift_train_10k.fbin" && -f "$DUCK_DATA/sift_gt_100k_200q.ibin" ]]; then
        info "fbin data already present at $DUCK_DATA — skipping (delete to regenerate)"
        return 0
    fi
    [[ -f "$ANN_HDF5_SRC" ]] || fail "missing source HDF5: $ANN_HDF5_SRC (override with ANN_HDF5_SRC=...)"
    info "converting SIFT HDF5 → fbin"
    python3 "$DUCK_BASE/convert_sift.py"
    ok "data ready under $DUCK_DATA/"
}

cmd_smoke() {
    [[ -f "$DUCK_BIN/smoke_create_index" && \
       -f "$DUCK_BIN/rabitq_reconstruct_smoke" && \
       -f "$DUCK_BIN/pq_failure_cleanup_smoke" ]] || cmd_bin
    info "running smoke_create_index"
    "$DUCK_BIN/smoke_create_index" "$EXTENSION_PATH"
    info "running rabitq_reconstruct_smoke"
    "$DUCK_BIN/rabitq_reconstruct_smoke"
    info "running pq_failure_cleanup_smoke"
    "$DUCK_BIN/pq_failure_cleanup_smoke"
}

cmd_explain() {
    [[ -f "$DUCK_BIN/explain_literal_query" ]] || cmd_bin
    info "running explain_literal_query"
    "$DUCK_BIN/explain_literal_query" "$EXTENSION_PATH"
}

cmd_crash_recovery() {
    cmd_bin
    info "running crash_recovery"
    "$DUCK_BIN/crash_recovery" "$EXTENSION_PATH"
}

cmd_crash_recovery_matrix() {
    local versions="${DUCKDB_VERSIONS:-$DUCKDB_VERSION}"
    local failed=0
    for dver in $versions; do
        info "running crash_recovery for $dver"
        if DUCKDB_VERSION="$dver" bash "$PROJECT_DIR/build_duck.sh" crash-recovery; then
            ok "crash_recovery passed for $dver"
        else
            failed=1
        fi
    done
    [[ "$failed" -eq 0 ]] || fail "crash_recovery matrix failed"
}

_bench() {
    local dataset="$1"
    [[ -f "$DUCK_BIN/vex_sift_sql_benchmark" ]] || cmd_bin
    [[ -f "$DUCK_DATA/sift_train_10k.fbin" ]]   || cmd_data
    info "running SIFT benchmark dataset=$dataset"
    "$DUCK_BIN/vex_sift_sql_benchmark" "$DUCK_DATA" "$dataset" "$EXTENSION_PATH"
}
cmd_bench_10k()  { _bench 10k;  }
cmd_bench_100k() { _bench 100k; }
cmd_bench()      { _bench both; }
cmd_bench_rabitq() {
    [[ -f "$DUCK_BIN/vex_rabitq_synthetic_benchmark" ]] || cmd_bin
    info "running generated-data RaBitQ benchmark"
    "$DUCK_BIN/vex_rabitq_synthetic_benchmark" "$EXTENSION_PATH"
}

cmd_all() {
    cmd_setup; cmd_data; cmd_build; cmd_bin; cmd_smoke; cmd_bench_10k; cmd_bench_rabitq
}

cmd_clean() {
    if [[ -d "$DUCK_BUILD" ]]; then
        info "removing build/ ($(du -sh "$DUCK_BUILD" | cut -f1))"
        rm -rf "$DUCK_BUILD"
    fi
    if [[ -d "$DUCK_BIN" ]]; then
        info "removing bin/ binaries"
        rm -rf "$DUCK_BIN"
        mkdir -p "$DUCK_BIN"
    fi
    ok "clean done — kept duckdb_src/ and data/"
}

cmd_purge() {
    cmd_clean
    if [[ -d "$DUCK_SRC" ]]; then
        info "removing duckdb_src/ ($(du -sh "$DUCK_SRC" | cut -f1))"
        rm -rf "$DUCK_SRC"
    fi
    ok "purge done — kept data/ only"
}

cmd_status() {
    info "build/duck disk usage:"
    if [[ -d "$DUCK_ROOT" ]]; then
        du -sh "$DUCK_ROOT" "$DUCK_SRC" "$DUCK_BUILD" "$DUCK_BIN" "$DUCK_DATA" 2>/dev/null \
            | awk '{printf "  %-10s %s\n", $1, $2}'
    else
        echo "  (build/duck not initialized — run: $0 setup)"
    fi
    [[ -f "$EXTENSION_PATH" ]] && info "extension: $EXTENSION_PATH ($(du -h "$EXTENSION_PATH" | cut -f1))"
}

usage() {
    sed -n '2,26p' "$0"
}

CMD="${1:-}"; shift 2>/dev/null || true
case "$CMD" in
    setup)       cmd_setup ;;
    build)       cmd_build ;;
    bin)         cmd_bin ;;
    data)        cmd_data ;;
    smoke)       cmd_smoke ;;
    explain)     cmd_explain ;;
    crash-recovery) cmd_crash_recovery ;;
    crash-recovery-matrix) cmd_crash_recovery_matrix ;;
    bench-10k)   cmd_bench_10k ;;
    bench-100k)  cmd_bench_100k ;;
    bench-rabitq) cmd_bench_rabitq ;;
    bench)       cmd_bench ;;
    build-unittest) cmd_build_unittest ;;
    unittest)    cmd_unittest "$@" ;;
    strip)       cmd_strip ;;
    all)         cmd_all ;;
    clean)       cmd_clean ;;
    purge)       cmd_purge ;;
    status)      cmd_status ;;
    -h|--help|"") usage ;;
    *) fail "unknown command: $CMD (try --help)" ;;
esac
