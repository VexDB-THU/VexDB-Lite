#!/bin/bash
set -euo pipefail

STAGE="${1:?usage: documentation_smoke.sh /path/to/package-stage}"
STAGE="$(cd "$STAGE" && pwd)"
TMP_DIR="$(mktemp -d -t vexdb-documentation-smoke)"
TEST_HOME="$TMP_DIR/home"
APP_DIR="$TEST_HOME/Applications"
BIN_DIR="$TEST_HOME/.local/bin"
LIB_DIR="$TEST_HOME/.local/lib/vexdb-lite"
PAYLOAD_APP="$STAGE/.payload/VexDB Lite.app"
CHECKS=0
PACKAGE_VERSION="$(sed -n 's/^preview_version=//p' "$STAGE/MANIFEST.txt")"
[ -n "$PACKAGE_VERSION" ] || { echo "DOCUMENTATION SMOKE: FAIL: manifest has no preview_version" >&2; exit 1; }
BUNDLE_MARKETING_VERSION="$(sed -n 's/^bundle_marketing_version=//p' "$STAGE/MANIFEST.txt")"
[ -n "$BUNDLE_MARKETING_VERSION" ] || { echo "DOCUMENTATION SMOKE: FAIL: manifest has no bundle marketing version" >&2; exit 1; }
BUNDLE_BUILD_VERSION="$(sed -n 's/^bundle_build_version=//p' "$STAGE/MANIFEST.txt")"
[ -n "$BUNDLE_BUILD_VERSION" ] || { echo "DOCUMENTATION SMOKE: FAIL: manifest has no bundle build version" >&2; exit 1; }
PACKAGE_ARCHITECTURE="$(sed -n 's/^architecture=//p' "$STAGE/MANIFEST.txt")"
case "$PACKAGE_ARCHITECTURE" in
    arm64|x86_64) ;;
    *) echo "DOCUMENTATION SMOKE: FAIL: invalid manifest architecture: $PACKAGE_ARCHITECTURE" >&2; exit 1 ;;
esac
MANIFEST_RUNTIME_ABI="$(sed -n 's/^mount_abi_version=//p' "$STAGE/MANIFEST.txt")"
[ -n "$MANIFEST_RUNTIME_ABI" ] || { echo "DOCUMENTATION SMOKE: FAIL: manifest has no runtime ABI" >&2; exit 1; }
PACKAGE_SIGNATURE="$(sed -n 's/^signature=//p' "$STAGE/MANIFEST.txt")"
SOURCE_DIRTY="$(sed -n 's/^source_dirty=//p' "$STAGE/MANIFEST.txt")"
ALLOW_ADHOC=0
[ "$PACKAGE_SIGNATURE" != ad-hoc ] || ALLOW_ADHOC=1
ALLOW_DIRTY=0
if [ "$SOURCE_DIRTY" = true ] && [ "${VEXDB_LITE_ALLOW_DIRTY_PACKAGE_TEST:-0}" = 1 ]; then
    ALLOW_DIRTY=1
fi
EXPECTED_PRODUCT_VERSION="vexdb-lite $PACKAGE_VERSION"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

fail() {
    echo "DOCUMENTATION SMOKE: FAIL: $*" >&2
    exit 1
}

check() {
    CHECKS=$((CHECKS + 1))
    "$@" || fail "check $CHECKS failed: $*"
}

equal() {
    CHECKS=$((CHECKS + 1))
    [ "$1" = "$2" ] || fail "check $CHECKS expected '$1', got '$2'${3:+ ($3)}"
}

contains() {
    CHECKS=$((CHECKS + 1))
    case "$1" in
        *"$2"*) ;;
        *) fail "check $CHECKS did not find '$2'${3:+ ($3)}" ;;
    esac
}

expect_fail() {
    CHECKS=$((CHECKS + 1))
    if "$@" >/dev/null 2>&1; then
        fail "check $CHECKS expected failure: $*"
    fi
}

mkdir -p "$TEST_HOME"

# 交付物和使用说明。App 放在隐藏 payload，防止解压目录被 LaunchServices
# 当成第二个可运行副本，从而让 FSKit 随机选到未安装的扩展。
check test ! -e "$STAGE/VexDB Lite.app"
check test -d "$PAYLOAD_APP"
for bundle in \
    "$PAYLOAD_APP" \
    "$PAYLOAD_APP/Contents/Extensions/VexFSAppEx.appex"; do
    equal "$BUNDLE_MARKETING_VERSION" \
        "$(plutil -extract CFBundleShortVersionString raw -o - "$bundle/Contents/Info.plist")" \
        "bundle marketing version"
    equal "$BUNDLE_BUILD_VERSION" \
        "$(plutil -extract CFBundleVersion raw -o - "$bundle/Contents/Info.plist")" \
        "bundle build version"
done
equal "$PACKAGE_ARCHITECTURE" \
    "$(lipo -archs "$PAYLOAD_APP/Contents/MacOS/VexDB Lite")" \
    "app architecture"
equal "$PACKAGE_ARCHITECTURE" \
    "$(lipo -archs "$PAYLOAD_APP/Contents/Extensions/VexFSAppEx.appex/Contents/MacOS/VexFSAppEx")" \
    "FSKit extension architecture"
check test -x "$STAGE/bin/vexdb"
check test -L "$STAGE/bin/vexfs"
check test -x "$STAGE/bin/vexfs-nfs-gateway"
check test -f "$STAGE/lib/vexdb_lite.dylib"
check test -f "$STAGE/lib/runtime/libpq.5.dylib"
equal "$PACKAGE_ARCHITECTURE" \
    "$(lipo -archs "$STAGE/bin/vexfs-nfs-gateway")" \
    "NFS gateway architecture"
EXTENSION_ENTITLEMENTS="$TMP_DIR/extension-entitlements.plist"
EXTENSION_ENTITLEMENTS_LOG="$TMP_DIR/extension-entitlements.log"
if ! codesign -d --entitlements - --xml \
        "$PAYLOAD_APP/Contents/Extensions/VexFSAppEx.appex" \
        >"$EXTENSION_ENTITLEMENTS" 2>"$EXTENSION_ENTITLEMENTS_LOG"; then
    cat "$EXTENSION_ENTITLEMENTS_LOG" >&2
    echo "DOCUMENTATION SMOKE: FAIL: cannot read FSKit entitlements" >&2
    exit 1
fi
if grep -q 'invalid entitlements blob' "$EXTENSION_ENTITLEMENTS_LOG" ||
        ! plutil -lint "$EXTENSION_ENTITLEMENTS" >/dev/null 2>&1; then
    cat "$EXTENSION_ENTITLEMENTS_LOG" >&2
    echo "DOCUMENTATION SMOKE: FAIL: invalid FSKit DER entitlements" >&2
    exit 1
fi
CHECKS=$((CHECKS + 1))
FSKIT_ENTITLEMENT="$(/usr/libexec/PlistBuddy \
    -c 'Print :com.apple.developer.fskit.fsmodule' \
    "$EXTENSION_ENTITLEMENTS" 2>/dev/null || true)"
equal "true" "$FSKIT_ENTITLEMENT" "FSKit module entitlement"
if [ "$PACKAGE_SIGNATURE" = developer-id ]; then
    EXTENSION_TEAM="$(/usr/libexec/PlistBuddy \
        -c 'Print :com.apple.developer.team-identifier' \
        "$EXTENSION_ENTITLEMENTS" 2>/dev/null || true)"
    EXTENSION_APPLICATION="$(/usr/libexec/PlistBuddy \
        -c 'Print :com.apple.application-identifier' \
        "$EXTENSION_ENTITLEMENTS" 2>/dev/null || true)"
    equal "developer-id" "$PACKAGE_SIGNATURE" "Developer ID entitlement checks"
    equal "$(sed -n 's/^signing_team=//p' "$STAGE/MANIFEST.txt")" \
        "$EXTENSION_TEAM" "FSKit team entitlement"
    equal "$EXTENSION_TEAM.io.vexdb.vexfs.extension" \
        "$EXTENSION_APPLICATION" "FSKit application identifier"
fi
check test -f "$STAGE/使用说明.md"
check test -f "$STAGE/THIRD_PARTY.md"
check test -f "$STAGE/licenses/nfsserve-LICENSE"
check grep -q 'nfsserve 0.11.0' "$STAGE/THIRD_PARTY.md"
check grep -q 'BSD 3-Clause License' "$STAGE/licenses/nfsserve-LICENSE"
check grep -q 'Developer ID' "$STAGE/使用说明.md"
check grep -q '默认使用系统自带的 NFS client' "$STAGE/使用说明.md"
check grep -q '127.0.0.1:/' "$STAGE/使用说明.md"
check grep -q '底层目录设为' "$STAGE/使用说明.md"
check grep -q '不会被 VexDB Lite 自动重放' "$STAGE/使用说明.md"
check grep -q 'verify_installed_mount' "$STAGE/install.sh"
check grep -q '默认 NFS 真实挂载检查通过' "$STAGE/install.sh"
expect_fail grep -q 'killall -u.*pkd' "$STAGE/install.sh"
expect_fail grep -q 'lsregister.*-kill' "$STAGE/install.sh"
check grep -Eq '不会.*清空 LaunchServices 注册库' "$STAGE/使用说明.md"
expect_fail grep -q '没有提供可用的授权入口' "$STAGE/使用说明.md"
check grep -Eq '^signature=(developer-id|ad-hoc)$' "$STAGE/MANIFEST.txt"
check grep -Eq '^notarization=(not-submitted|accepted)$' "$STAGE/MANIFEST.txt"
NOTARIZATION_STATUS="$(sed -n 's/^notarization=//p' "$STAGE/MANIFEST.txt")"
case "$NOTARIZATION_STATUS" in
    accepted)
        check grep -Eq '^notarization_submission_id=[0-9a-fA-F-]{36}$' \
            "$STAGE/MANIFEST.txt"
        ;;
    not-submitted)
        check grep -q '^notarization_submission_id=none$' "$STAGE/MANIFEST.txt"
        ;;
esac
(
    cd "$STAGE"
    shasum -a 256 -c SHA256SUMS.txt >/dev/null
) || fail "package SHA256SUMS verification failed"
CHECKS=$((CHECKS + 1))

# 安装事务必须在 App 已替换、CLI 复制失败时恢复旧版本，并且不留下可被
# LaunchServices 再次发现的 `.app.disabled` 副本。
ROLLBACK_ROOT="$TMP_DIR/rollback-install"
ROLLBACK_APP_DIR="$ROLLBACK_ROOT/Applications"
ROLLBACK_BIN_DIR="$ROLLBACK_ROOT/read-only-bin"
ROLLBACK_LIB_DIR="$ROLLBACK_ROOT/lib"
mkdir -p "$ROLLBACK_APP_DIR" "$ROLLBACK_BIN_DIR" "$ROLLBACK_LIB_DIR"
ditto "$PAYLOAD_APP" "$ROLLBACK_APP_DIR/VexDB Lite.app"
touch "$ROLLBACK_APP_DIR/VexDB Lite.app/.rollback-marker"
chmod 0555 "$ROLLBACK_BIN_DIR"
expect_fail env \
    HOME="$ROLLBACK_ROOT" \
    VEXDB_LITE_APP_DIR="$ROLLBACK_APP_DIR" \
    VEXDB_LITE_BIN_DIR="$ROLLBACK_BIN_DIR" \
    VEXDB_LITE_LIB_DIR="$ROLLBACK_LIB_DIR" \
    VEXDB_LITE_ALLOW_ADHOC_INSTALL="$ALLOW_ADHOC" \
    VEXDB_LITE_ALLOW_DIRTY_INSTALL="$ALLOW_DIRTY" \
    "$STAGE/install.sh"
chmod 0755 "$ROLLBACK_BIN_DIR"
check test -f "$ROLLBACK_APP_DIR/VexDB Lite.app/.rollback-marker"
equal "" "$(find "$ROLLBACK_APP_DIR" -maxdepth 1 \
    -name '*.app.disabled' -print -quit)" "no discoverable rollback App"
equal "" "$(find "$ROLLBACK_APP_DIR" -maxdepth 1 \
    -name '.vexdb-lite-install-*' -print -quit)" "no installation transaction residue"

# 按文档执行无污染安装。所有安装位置和 HOME 都在临时目录。
HOME="$TEST_HOME" \
VEXDB_LITE_APP_DIR="$APP_DIR" \
VEXDB_LITE_BIN_DIR="$BIN_DIR" \
VEXDB_LITE_LIB_DIR="$LIB_DIR" \
VEXDB_LITE_ALLOW_ADHOC_INSTALL="$ALLOW_ADHOC" \
VEXDB_LITE_ALLOW_DIRTY_INSTALL="$ALLOW_DIRTY" \
    "$STAGE/install.sh" >"$TMP_DIR/install.out"

VEXDB="$BIN_DIR/vexdb"
VEXFS="$BIN_DIR/vexfs"
NFS_GATEWAY="$BIN_DIR/vexfs-nfs-gateway"
DYLIB="$LIB_DIR/vexdb_lite.dylib"
check test -d "$APP_DIR/VexDB Lite.app"
check test -x "$VEXDB"
check test -L "$VEXFS"
check test -x "$NFS_GATEWAY"
check test -f "$DYLIB"
contains "$("$VEXDB" --version)" "$EXPECTED_PRODUCT_VERSION" "version"
contains "$(env HOME="$TEST_HOME" PATH="$BIN_DIR:/usr/bin:/bin:/usr/sbin:/sbin" vexdb --version)" \
    "vexdb-lite" "PATH invocation"

# SQLite 交互和直接 SQL。
SQL_DB="$TEST_HOME/data/agent.db"
mkdir -p "$(dirname "$SQL_DB")"
INTERACTIVE_OUT="$(printf '%s\n' \
    'CREATE TABLE notes(id INTEGER PRIMARY KEY, content TEXT);' \
    "INSERT INTO notes(content) VALUES ('hello');" \
    'SELECT content FROM notes;' \
    '.quit' | "$VEXDB" "$SQL_DB")"
contains "$INTERACTIVE_OUT" "hello" "interactive SQLite"
contains "$("$VEXDB" "$SQL_DB" "SELECT sqlite_version(), vexdb_version();")" \
    "vexdb-lite" "direct SQL"

# 向量距离和 GRAPH_INDEX。
equal "5.0" "$("$VEXDB" "$SQL_DB" \
    "SELECT vexdb_l2_distance('[1,2]', '[4,6]');")" "L2 distance"
"$VEXDB" "$SQL_DB" \
    "CREATE VIRTUAL TABLE vectors USING GRAPH_INDEX(embedding FLOAT[2], metric=l2);" >/dev/null
"$VEXDB" "$SQL_DB" \
    "INSERT INTO vectors(rowid, embedding) VALUES (1, '[1,1]'), (2, '[9,9]');" >/dev/null
equal "1" "$("$VEXDB" "$SQL_DB" \
    "SELECT rowid FROM vectors WHERE embedding MATCH '[1,1]' AND k=1;")" \
    "nearest neighbor"

# 默认数据库的文件管理命令。
HOME="$TEST_HOME" "$VEXDB" fs setup >/dev/null
DOCTOR_JSON="$(HOME="$TEST_HOME" "$VEXDB" fs --json doctor || true)"
CLI_RUNTIME_ABI="$(printf '%s' "$DOCTOR_JSON" | /usr/bin/python3 -c \
    'import json,sys; print(json.load(sys.stdin)["runtime_abi"])')"
equal "$MANIFEST_RUNTIME_ABI" "$CLI_RUNTIME_ABI" "manifest runtime ABI"
HOME="$TEST_HOME" "$VEXDB" fs mkdir /docs
printf 'hello VexFS\n' | HOME="$TEST_HOME" "$VEXDB" fs write /docs/hello.txt >/dev/null
printf 'fake-png-data\x00tail' >"$TMP_DIR/photo.png"
HOME="$TEST_HOME" "$VEXDB" fs write /docs/photo.png "$TMP_DIR/photo.png" >/dev/null
LISTING="$(HOME="$TEST_HOME" "$VEXDB" fs ls /docs)"
contains "$LISTING" "hello.txt" "file listing"
contains "$LISTING" "photo.png" "local file write"
equal "hello VexFS" "$(HOME="$TEST_HOME" "$VEXDB" fs cat /docs/hello.txt)" "file cat"
contains "$(HOME="$TEST_HOME" "$VEXDB" fs stat /docs/hello.txt)" '"kind":"file"' "file stat"
HOME="$TEST_HOME" "$VEXDB" fs mv /docs/hello.txt /docs/readme.txt
equal "hello VexFS" "$(HOME="$TEST_HOME" "$VEXDB" fs cat /docs/readme.txt)" "file move"
expect_fail env HOME="$TEST_HOME" "$VEXDB" fs stat /docs/hello.txt
HOME="$TEST_HOME" "$VEXDB" fs rm /docs/readme.txt
expect_fail env HOME="$TEST_HOME" "$VEXDB" fs stat /docs/readme.txt

# 指定数据库、workspace 和 vexfs 兼容入口。
HOME="$TEST_HOME" "$VEXDB" fs --db "$SQL_DB" --workspace project-a setup >/dev/null
printf 'project only' | HOME="$TEST_HOME" "$VEXDB" fs --db "$SQL_DB" \
    --workspace project-a write /project.txt >/dev/null
equal "project only" "$(HOME="$TEST_HOME" "$VEXDB" fs --db "$SQL_DB" \
    --workspace project-a cat /project.txt)" "workspace file"
equal "project only" "$(HOME="$TEST_HOME" "$VEXFS" --db "$SQL_DB" \
    --workspace project-a cat /project.txt)" "vexfs compatibility"
HOME="$TEST_HOME" "$VEXDB" fs --db "$SQL_DB" --workspace default setup >/dev/null
expect_fail env HOME="$TEST_HOME" "$VEXDB" fs --db "$SQL_DB" \
    --workspace default stat /project.txt

# 文件版本、diff、dry-run 和恢复。
printf 'first\n' | HOME="$TEST_HOME" "$VEXDB" fs write /docs/plan.md >"$TMP_DIR/v1"
printf 'second\n' | HOME="$TEST_HOME" "$VEXDB" fs write /docs/plan.md >"$TMP_DIR/v2"
V1="$(tr -d '\n' <"$TMP_DIR/v1")"
V2="$(tr -d '\n' <"$TMP_DIR/v2")"
contains "$(HOME="$TEST_HOME" "$VEXDB" fs history /docs/plan.md)" "$V1" "history"
equal "first" "$(HOME="$TEST_HOME" "$VEXDB" fs show /docs/plan.md --version "$V1")" \
    "historical content"
set +e
DIFF_OUT="$(HOME="$TEST_HOME" "$VEXDB" fs diff /docs/plan.md --from "$V1" 2>&1)"
DIFF_RC=$?
set -e
equal "1" "$DIFF_RC" "diff exit code"
contains "$DIFF_OUT" "first" "diff old content"
contains "$DIFF_OUT" "second" "diff new content"
HOME="$TEST_HOME" "$VEXDB" fs restore /docs/plan.md --version "$V1" --dry-run >/dev/null
RESTORED_VERSION="$(HOME="$TEST_HOME" "$VEXDB" fs restore /docs/plan.md --version "$V1")"
equal "first" "$(HOME="$TEST_HOME" "$VEXDB" fs cat /docs/plan.md)" "restored content"
check test "$RESTORED_VERSION" -gt "$V2"

# 完整 workspace 快照、差异、dry-run 和一键还原。
SNAPSHOT_COMMIT="$(HOME="$TEST_HOME" "$VEXDB" fs snapshot create before-agent)"
check test "$SNAPSHOT_COMMIT" -gt 0
contains "$(HOME="$TEST_HOME" "$VEXDB" fs snapshot list)" "before-agent" \
    "workspace snapshot list"
contains "$(HOME="$TEST_HOME" "$VEXDB" fs snapshot show before-agent)" \
    '"path":"/docs/plan.md"' "workspace snapshot tree"
printf 'third\n' | HOME="$TEST_HOME" "$VEXDB" fs write /docs/plan.md >/dev/null
HOME="$TEST_HOME" "$VEXDB" fs mkdir /after-snapshot
printf 'new file\n' | HOME="$TEST_HOME" "$VEXDB" fs write /after-snapshot/new.txt >/dev/null
set +e
SNAPSHOT_DIFF="$(HOME="$TEST_HOME" "$VEXDB" fs snapshot diff before-agent 2>&1)"
SNAPSHOT_DIFF_RC=$?
set -e
equal "1" "$SNAPSHOT_DIFF_RC" "workspace snapshot diff exit code"
contains "$SNAPSHOT_DIFF" '"path":"/docs/plan.md"' "workspace modified file"
contains "$SNAPSHOT_DIFF" '"path":"/after-snapshot"' "workspace added directory"
HOME="$TEST_HOME" "$VEXDB" fs snapshot restore before-agent --dry-run >/dev/null
SNAPSHOT_RESTORE_COMMIT="$(HOME="$TEST_HOME" "$VEXDB" fs snapshot restore before-agent)"
check test "$SNAPSHOT_RESTORE_COMMIT" -gt "$SNAPSHOT_COMMIT"
equal "first" "$(HOME="$TEST_HOME" "$VEXDB" fs cat /docs/plan.md)" \
    "workspace restored content"
expect_fail env HOME="$TEST_HOME" "$VEXDB" fs stat /after-snapshot/new.txt
HOME="$TEST_HOME" "$VEXDB" fs snapshot diff before-agent >/dev/null

# 默认 doctor 只要求系统 NFS client 和同包 gateway；FSKit 状态只作为可选信息。
DOCTOR_JSON="$(HOME="$TEST_HOME" "$VEXDB" fs --json doctor)"
contains "$DOCTOR_JSON" '"mount_driver":"NFSv3"' "default NFS driver"
contains "$DOCTOR_JSON" '"mount_ready":true' "default NFS readiness"
contains "$DOCTOR_JSON" '"schema_ready":true' "doctor database state"

# 数据库备份、拒绝覆盖和备份验证。
DEFAULT_DB="$TEST_HOME/Library/Application Support/VexDB-Lite/default.sqlite3"
BACKUP_DB="$TEST_HOME/Desktop/vexdb-backup.sqlite3"
"$VEXDB" backup "$DEFAULT_DB" "$BACKUP_DB" >/dev/null
check test -f "$DEFAULT_DB"
check test -f "$BACKUP_DB"
equal "ok" "$("$VEXDB" "$BACKUP_DB" "PRAGMA integrity_check;")" "backup integrity"
equal "first" "$(HOME="$TEST_HOME" "$VEXDB" fs --db "$BACKUP_DB" cat /docs/plan.md)" \
    "backup VexFS content"
contains "$(HOME="$TEST_HOME" "$VEXDB" fs --db "$BACKUP_DB" snapshot list)" \
    "before-agent" "backup workspace snapshot"
HOME="$TEST_HOME" "$VEXDB" fs --db "$BACKUP_DB" snapshot diff before-agent >/dev/null
expect_fail "$VEXDB" backup "$DEFAULT_DB" "$BACKUP_DB"
HOME="$TEST_HOME" "$VEXDB" fs snapshot drop before-agent

# macOS 系统 sqlite3 通常关闭 .load；另有支持动态加载的 sqlite3 时验证 dylib。
if /usr/bin/sqlite3 :memory: ".help" | grep -q '^\.load'; then
    fail "documentation must be reviewed: /usr/bin/sqlite3 unexpectedly supports .load"
fi
CHECKS=$((CHECKS + 1))
LOADABLE_SQLITE="$(command -v sqlite3 || true)"
LOADABLE_SQLITE_ARCHS=""
LOADABLE_SQLITE_COMPATIBLE=0
if [ -n "$LOADABLE_SQLITE" ]; then
    LOADABLE_SQLITE_ARCHS="$(lipo -archs "$LOADABLE_SQLITE" 2>/dev/null || true)"
fi
case " $LOADABLE_SQLITE_ARCHS " in
    *" $PACKAGE_ARCHITECTURE "*) LOADABLE_SQLITE_COMPATIBLE=1 ;;
esac
if [ -n "$LOADABLE_SQLITE" ] && [ "$LOADABLE_SQLITE" != "/usr/bin/sqlite3" ] && \
   [ "$LOADABLE_SQLITE_COMPATIBLE" = 1 ] && \
   "$LOADABLE_SQLITE" :memory: ".help" | grep -q '^\.load'; then
    contains "$("$LOADABLE_SQLITE" "$SQL_DB" ".load $DYLIB" "SELECT vexdb_version();")" \
        "$EXPECTED_PRODUCT_VERSION" "third-party sqlite loadable extension"
fi

# 卸载只移除程序，不删除数据库。
HOME="$TEST_HOME" \
VEXDB_LITE_APP_DIR="$APP_DIR" \
VEXDB_LITE_BIN_DIR="$BIN_DIR" \
VEXDB_LITE_LIB_DIR="$LIB_DIR" \
    "$STAGE/uninstall.sh" >"$TMP_DIR/uninstall.out"
check test ! -e "$APP_DIR/VexDB Lite.app"
check test ! -e "$VEXDB"
check test ! -e "$VEXFS"
check test ! -e "$NFS_GATEWAY"
check test ! -e "$DYLIB"
check test -f "$DEFAULT_DB"
check test -f "$BACKUP_DB"

echo "VEXDB DOCUMENTATION SMOKE: PASS ($CHECKS checks)"
