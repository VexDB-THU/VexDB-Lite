# PG 19devel + pg_vexdb 测试镜像
#
# 多阶段构建:
#   stage 1 (pg-build):  从源码 build PG 19devel
#   stage 2 (vex-build): build pg_vexdb 扩展, 链到上一阶段的 PG
#   stage 3 (runtime):   运行环境 - 含 PG server + pg_vexdb 已安装
#
# Build:
#   docker build -t vexdb-pg19 -f tests/spec/_lib/docker/Dockerfile .
# Run:
#   docker run --rm -v $(pwd)/build/spec/pg:/spec vexdb-pg19 \
#       /opt/pg19/bin/psql -d test -f /spec/sql/types__floatvector_basic.sql

# ─────────────────────────── stage 1: build PG 19devel ───────────────────────────
FROM debian:trixie-slim AS pg-build

ARG PG_GIT_TAG=master
ARG JOBS=4

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential git ca-certificates flex bison \
        libreadline-dev zlib1g-dev libicu-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --depth 1 --branch ${PG_GIT_TAG} https://github.com/postgres/postgres.git

WORKDIR /src/postgres
RUN ./configure --prefix=/opt/pg19 --enable-debug --enable-cassert \
        --with-includes=/usr/include --with-libraries=/usr/lib \
        CFLAGS="-O2 -g" \
    && make -j${JOBS} \
    && make install \
    && cd contrib && make -j${JOBS} && make install

# ─────────────────────────── stage 2: build pg_vexdb ─────────────────────────────
FROM pg-build AS vex-build

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake libboost-dev libboost-system-dev libboost-thread-dev g++ \
    && rm -rf /var/lib/apt/lists/* \
    && test -f /usr/include/boost/unordered/concurrent_flat_map.hpp \
       || (echo "ERROR: boost concurrent_flat_map.hpp missing - need Boost >= 1.83" && exit 1)

ENV PATH=/opt/pg19/bin:$PATH
ENV PG_CONFIG=/opt/pg19/bin/pg_config

# 通过 build 上下文挂载 PG_VEXDB 源码 (需要 build 时把它放在上下文里)
COPY pg_vexdb_src /src/pg_vexdb

WORKDIR /src/pg_vexdb
# - 替换硬编码 PG_CONFIG
# - ARM64 注入 -DFLT16_SUPPORT (用 (float)__fp16 cast 走 std::isnan, 避免 __fp16 直接位运算)
# - GCC 14 严格化对部分 const 用法报错, 加 -fpermissive 兜底
# Makefile 已在 host 端 patch (run_pg.sh 准备 build context 时重写)
RUN make -j${JOBS} && make install

# ─────────────────────────── stage 3: runtime ────────────────────────────────────
FROM debian:trixie-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        libreadline8 zlib1g libicu76 libboost-system1.83.0 libboost-thread1.83.0 \
        ca-certificates diffutils \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r postgres && useradd -r -g postgres -m -d /home/postgres postgres \
    && mkdir -p /var/lib/postgresql/data && chown -R postgres:postgres /var/lib/postgresql

COPY --from=vex-build /opt/pg19 /opt/pg19

ENV PATH=/opt/pg19/bin:$PATH
ENV PGDATA=/var/lib/postgresql/data

USER postgres
WORKDIR /home/postgres

RUN /opt/pg19/bin/initdb -D ${PGDATA} -E UTF8 --locale=C && \
    echo "shared_preload_libraries = 'pg_vexdb'" >> ${PGDATA}/postgresql.conf && \
    # 容器内 trust (容器边界即信任边界); 容器外暴露需走 docker -p 127.0.0.1:5433:5432
    echo "host all all 127.0.0.1/32 trust" >> ${PGDATA}/pg_hba.conf && \
    echo "host all all ::1/128 trust"     >> ${PGDATA}/pg_hba.conf && \
    # 容器网络内允许其他 container 连 (跨 container 测试需要)
    echo "host all all 172.17.0.0/16 trust" >> ${PGDATA}/pg_hba.conf && \
    echo "listen_addresses = '*'"         >> ${PGDATA}/postgresql.conf

EXPOSE 5432

# 启动: pg_ctl + 创建 test 数据库 + 加载扩展, 然后保持前台
CMD pg_ctl -D ${PGDATA} -l /tmp/pg.log -w start && \
    createdb test && \
    psql -d test -c 'CREATE EXTENSION pg_vexdb;' && \
    echo '=== pg_vexdb ready on port 5432, db=test ===' && \
    tail -f /tmp/pg.log
