# DuckDB Community Extension 构建入口 —— 仅供 duckdb/community-extensions CI 使用。
#
# CI 流程(_extension_distribution.yml)会把本仓库 checkout 到根，并把
# extension-ci-tools/ checkout 到同级目录，然后从这里跑 `make`。duckdb/ 由下面的
# vexdb_prepare_duckdb_checkout 目标按 CI 请求的版本准备。
#
# ⚠️ 本地开发/发版请用 ./build_duck.sh，不要用这个 Makefile —— 它依赖 CI 注入的
#    extension-ci-tools/，本地默认不存在。
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# 扩展构建逻辑隔离在 community/ 子目录，根目录只留这个薄入口 + vcpkg.json。
EXT_NAME=vexdb_lite
EXT_CONFIG=${PROJ_DIR}community/extension_config.cmake

# 保存 CI 传入的值。set_duckdb_version 会把上游 recipe 使用的版本临时改为
# FETCH_HEAD，避免为 tag、branch 和 commit SHA 分别维护不同的本地 ref。
VEXDB_DUCKDB_GIT_VERSION := $(strip $(DUCKDB_GIT_VERSION))
VEXDB_DUCKDB_GIT_REPOSITORY := $(if $(strip $(DUCKDB_GIT_REPOSITORY)),$(strip $(DUCKDB_GIT_REPOSITORY)),https://github.com/duckdb/duckdb.git)

include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: vexdb_prepare_duckdb_checkout

# 上游 duckdb_extension.Makefile 的 set_duckdb_version 只会进入已有的 duckdb/
# 执行 checkout。Community CI 不会替 override_repository 自动创建这个目录，所以先
# 初始化一个轻量 checkout，只拉取本次构建需要的 ref。
set_duckdb_version: vexdb_prepare_duckdb_checkout
set_duckdb_version: override DUCKDB_GIT_VERSION := FETCH_HEAD

vexdb_prepare_duckdb_checkout:
	@if [ -z "$(VEXDB_DUCKDB_GIT_VERSION)" ]; then \
		echo "DUCKDB_GIT_VERSION is required" >&2; \
		exit 2; \
	fi
	@if [ -e duckdb ] && ! git -C duckdb rev-parse --git-dir >/dev/null 2>&1; then \
		echo "duckdb exists but is not a git checkout" >&2; \
		exit 2; \
	fi
	@if [ ! -e duckdb ]; then \
		git init duckdb; \
		git -C duckdb remote add origin "$(VEXDB_DUCKDB_GIT_REPOSITORY)"; \
	elif ! git -C duckdb remote get-url origin >/dev/null 2>&1; then \
		git -C duckdb remote add origin "$(VEXDB_DUCKDB_GIT_REPOSITORY)"; \
	fi
	@if git -C duckdb ls-remote --exit-code --tags origin \
		"refs/tags/$(VEXDB_DUCKDB_GIT_VERSION)" >/dev/null 2>&1; then \
		git -C duckdb fetch --force --depth=1 origin \
			"refs/tags/$(VEXDB_DUCKDB_GIT_VERSION):refs/tags/$(VEXDB_DUCKDB_GIT_VERSION)"; \
	else \
		git -C duckdb fetch --force --depth=1 origin "$(VEXDB_DUCKDB_GIT_VERSION)"; \
	fi
