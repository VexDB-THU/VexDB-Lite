#!/bin/bash
set -euo pipefail

# Copy the non-system dynamic-library closure needed by libpq into a portable
# @rpath directory. Usage:
#   bundle_pg_runtime.sh <binary> <destination> <architecture> <search-dir>...

[ "$#" -ge 4 ] || {
    echo "usage: $0 binary destination architecture search-dir..." >&2
    exit 2
}

BINARY="$1"
DESTINATION="$2"
ARCHITECTURE="$3"
shift 3

[ -f "$BINARY" ] || { echo "binary not found: $BINARY" >&2; exit 1; }
case "$ARCHITECTURE" in arm64|x86_64) ;; *) echo "unsupported architecture: $ARCHITECTURE" >&2; exit 2;; esac
OTOOL=/usr/bin/otool
INSTALL_NAME_TOOL=/usr/bin/install_name_tool
LIPO=/usr/bin/lipo
[ -x "$OTOOL" ] || { echo "Apple otool is required" >&2; exit 1; }
[ -x "$INSTALL_NAME_TOOL" ] || { echo "Apple install_name_tool is required" >&2; exit 1; }
[ -x "$LIPO" ] || { echo "Apple lipo is required" >&2; exit 1; }

mkdir -p "$DESTINATION"
QUEUE="$(mktemp "${TMPDIR:-/tmp}/vexfs-pg-runtime.queue.XXXXXX")"
SEARCH_DIRS="$(mktemp "${TMPDIR:-/tmp}/vexfs-pg-runtime.search.XXXXXX")"
trap 'rm -f "$QUEUE" "$SEARCH_DIRS"' EXIT
printf '%s\n' "$BINARY" > "$QUEUE"
for directory in "$@"; do
    [ -d "$directory" ] && printf '%s\n' "$directory" >> "$SEARCH_DIRS"
done

find_dependency() {
    local name="$1" directory
    while IFS= read -r directory; do
        [ -f "$directory/$name" ] && { printf '%s\n' "$directory/$name"; return 0; }
    done < "$SEARCH_DIRS"
    return 1
}

queue_index=1
while :; do
    item="$(sed -n "${queue_index}p" "$QUEUE")"
    [ -n "$item" ] || break
    queue_index=$((queue_index + 1))

    "$OTOOL" -L "$item" | tail -n +2 | awk '{print $1}' | while IFS= read -r dependency; do
        case "$dependency" in
            /System/Library/*|/usr/lib/*) continue ;;
            @rpath/*)
                name="${dependency#@rpath/}"
                case "$name" in */*) echo "nested @rpath dependency is unsupported: $dependency" >&2; exit 1;; esac
                source="$(find_dependency "$name" || true)"
                ;;
            /*)
                source="$dependency"
                name="$(basename "$dependency")"
                printf '%s\n' "$(dirname "$dependency")" >> "$SEARCH_DIRS"
                ;;
            @loader_path/*)
                relative="${dependency#@loader_path/}"
                source="$(cd "$(dirname "$item")" && pwd)/$relative"
                name="$(basename "$source")"
                ;;
            *)
                echo "unsupported dynamic-library reference: $dependency in $item" >&2
                exit 1
                ;;
        esac

        destination="$DESTINATION/$name"
        # A dylib lists its own install name first. If it is already bundled,
        # there is nothing to copy or queue again.
        if [ ! -f "$destination" ]; then
            [ -n "${source:-}" ] && [ -f "$source" ] || {
                echo "cannot resolve $dependency required by $item" >&2
                exit 1
            }
            cp -fL "$source" "$destination"
            chmod 0755 "$destination"
            "$LIPO" "$destination" -verify_arch "$ARCHITECTURE" >/dev/null || {
                echo "$destination does not contain $ARCHITECTURE" >&2
                exit 1
            }
            "$INSTALL_NAME_TOOL" -id "@rpath/$name" "$destination"
            printf '%s\n' "$destination" >> "$QUEUE"
        fi
        if [ "$dependency" != "@rpath/$name" ]; then
            "$INSTALL_NAME_TOOL" -change "$dependency" "@rpath/$name" "$item"
        fi
    done
done

# Refuse a bundle that still reaches into Homebrew, Conda, MacPorts, or another
# developer-machine path. Every non-system dependency must now resolve locally.
while IFS= read -r item; do
    "$OTOOL" -l "$item" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { getline; getline; print $2 }
    ' | while IFS= read -r runpath; do
        case "$runpath" in
            @*) ;;
            *) "$INSTALL_NAME_TOOL" -delete_rpath "$runpath" "$item" ;;
        esac
    done
    "$OTOOL" -L "$item" | tail -n +2 | awk '{print $1}' | while IFS= read -r dependency; do
        case "$dependency" in
            /System/Library/*|/usr/lib/*) ;;
            @rpath/*)
                name="${dependency#@rpath/}"
                [ -f "$DESTINATION/$name" ] || {
                    echo "bundled dependency is missing: $dependency for $item" >&2
                    exit 1
                }
                ;;
            *) echo "non-portable dependency remains: $dependency in $item" >&2; exit 1;;
        esac
    done
done < "$QUEUE"

echo "Bundled PostgreSQL runtime: $(find "$DESTINATION" -type f -name '*.dylib' | wc -l | tr -d ' ') dylibs"
