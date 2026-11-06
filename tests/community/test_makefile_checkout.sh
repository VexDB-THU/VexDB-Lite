#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
test_root="$(mktemp -d "${TMPDIR:-/tmp}/vexdb-community-makefile.XXXXXX")"
trap 'rm -rf "$test_root"' EXIT

source_repo="$test_root/source"
remote_repo="$test_root/duckdb.git"
worktree="$test_root/worktree"

git init -q "$source_repo"
git -C "$source_repo" config user.name "VexDB Test"
git -C "$source_repo" config user.email "vexdb-test@example.invalid"

printf 'first\n' >"$source_repo/version.txt"
git -C "$source_repo" add version.txt
git -C "$source_repo" commit -q -m first
git -C "$source_repo" tag v-test-1
first_sha="$(git -C "$source_repo" rev-parse HEAD)"

printf 'second\n' >"$source_repo/version.txt"
git -C "$source_repo" commit -q -am second
git -C "$source_repo" tag v-test-2
second_sha="$(git -C "$source_repo" rev-parse HEAD)"
source_branch="$(git -C "$source_repo" symbolic-ref --short HEAD)"

git clone -q --bare "$source_repo" "$remote_repo"

mkdir -p "$worktree/extension-ci-tools/makefiles"
cp "$project_root/Makefile" "$worktree/Makefile"
cat >"$worktree/extension-ci-tools/makefiles/duckdb_extension.Makefile" <<'MAKEFILE'
.PHONY: set_duckdb_version
set_duckdb_version:
	cd duckdb && git checkout $(DUCKDB_GIT_VERSION)
MAKEFILE

run_checkout() {
    local version="$1"
    local expected_sha="$2"
    local expected_tag="${3:-}"
    local output

    output="$(make -s -C "$worktree" set_duckdb_version \
        DUCKDB_GIT_VERSION="$version" \
        DUCKDB_GIT_REPOSITORY="$remote_repo" 2>&1)"

    if [[ "$output" == *"overriding recipe for target"* ]]; then
        echo "unexpected duplicate recipe warning" >&2
        echo "$output" >&2
        return 1
    fi

    local actual_sha
    actual_sha="$(git -C "$worktree/duckdb" rev-parse HEAD)"
    if [[ "$actual_sha" != "$expected_sha" ]]; then
        echo "expected $version at $expected_sha, got $actual_sha" >&2
        return 1
    fi

    if [[ -n "$expected_tag" ]]; then
        local actual_tag
        actual_tag="$(git -C "$worktree/duckdb" describe --tags --exact-match)"
        if [[ "$actual_tag" != "$expected_tag" ]]; then
            echo "expected tag $expected_tag, got $actual_tag" >&2
            return 1
        fi
    fi
}

run_checkout v-test-1 "$first_sha" v-test-1
run_checkout v-test-2 "$second_sha" v-test-2
run_checkout "$source_branch" "$second_sha"
run_checkout "$first_sha" "$first_sha"

if make -s -C "$worktree" set_duckdb_version \
    DUCKDB_GIT_REPOSITORY="$remote_repo" >"$test_root/missing-version.log" 2>&1; then
    echo "missing DUCKDB_GIT_VERSION unexpectedly succeeded" >&2
    exit 1
fi

grep -q "DUCKDB_GIT_VERSION is required" "$test_root/missing-version.log"

invalid_worktree="$test_root/invalid-worktree"
mkdir -p "$invalid_worktree/extension-ci-tools/makefiles" "$invalid_worktree/duckdb"
cp "$project_root/Makefile" "$invalid_worktree/Makefile"
cp "$worktree/extension-ci-tools/makefiles/duckdb_extension.Makefile" \
    "$invalid_worktree/extension-ci-tools/makefiles/duckdb_extension.Makefile"
if make -s -C "$invalid_worktree" set_duckdb_version \
    DUCKDB_GIT_VERSION=v-test-1 \
    DUCKDB_GIT_REPOSITORY="$remote_repo" >"$test_root/non-git.log" 2>&1; then
    echo "non-git duckdb directory unexpectedly succeeded" >&2
    exit 1
fi

grep -q "duckdb exists but is not a git checkout" "$test_root/non-git.log"
echo "community Makefile checkout test passed"
