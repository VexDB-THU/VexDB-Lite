MODULE_big = pg_vexdb

SRCS = \
    src/graph_index_am.cpp \
    src/session_compat.cpp \
    src/graph_index.cpp \
    src/graph_index_build.cpp \
    src/core_memory_build_flush.cpp \
    src/graph_index_insert.cpp \
    src/graph_index_inspect.cpp \
    src/graph_index_scan.cpp \
    src/graph_index_utils.cpp \
    src/graph_index_vacuum.cpp \
    src/graph_index_xlog.cpp \
    src/floatvector.cpp \
    src/halfvec.cpp \
    src/ann_utils.cpp \
    src/bulkbuf_smgr.cpp \
    src/guc_config.cpp \
    src/pg_yield.cpp \
    src/pq.cpp \
    src/quantizer_stubs.cpp \
    src/rabitq_distancer.cpp \
    src/shared_alloc_set.cpp \
    src/vecbuf_worker.cpp \
    src/vector_storage.cpp \
    src/vector_smgr.cpp \
    knl/knl_alloc.cpp \
    src/distance/architecture.cpp \
    src/distance/distance.cpp \
    src/distance/general.cpp \
    src/distance/general_dispatcher.cpp \
    src/distance/sse.cpp \
    src/distance/sse_dispatcher.cpp \
    src/distance/avx.cpp \
    src/distance/avx_dispatcher.cpp \
    src/distance/avx512.cpp \
    src/distance/avx512_dispatcher.cpp

OBJS = $(SRCS:.cpp=.o)

EXTENSION = pg_vexdb
DATA = sql/pg_vexdb--1.0.sql

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_INCLUDEDIR := $(shell $(PG_CONFIG) --includedir)
BOOST_INCLUDEDIR ?=

PG_CPPFLAGS = -I$(CURDIR)/include -I$(CURDIR) -I$(CURDIR)/../libvex-core/include -I$(CURDIR)/distance -I$(CURDIR)/vec_types -I$(CURDIR)/rabitq -I$(CURDIR)/module -I$(PG_INCLUDEDIR) -I/usr/include
ifneq ($(strip $(BOOST_INCLUDEDIR)),)
PG_CPPFLAGS += -I$(BOOST_INCLUDEDIR)
endif

PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE ?= 1
SRCS += src/core_node_store_bridge.cpp \
        ../libvex-core/src/distance.cpp \
        ../libvex-core/src/graph_algo.cpp \
        ../libvex-core/src/node_store_memory.cpp \
        ../libvex-core/src/rabitq.cpp
ifeq ($(PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE),1)
PG_CPPFLAGS += -DPG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE
endif

override CXXFLAGS := -std=c++17 -O2 -fPIC -Wno-error=vla

ifneq ($(filter x86_64 amd64,$(shell uname -m)),)
    CXXFLAGS += -march=native
endif

ifneq ($(filter aarch64 arm64,$(shell uname -m)),)
    CXXFLAGS += -march=armv8-a+simd
endif

SHLIB_LINK += -lstdc++

# SIMD-specific flags
src/distance/sse.o: CXXFLAGS += -msse4.1
src/distance/sse_dispatcher.o: CXXFLAGS += -msse4.1 -mfma
src/distance/avx.o: CXXFLAGS += -mavx2 -mfma -mf16c
src/distance/avx_dispatcher.o: CXXFLAGS += -mavx2 -mfma -mf16c
src/distance/avx512.o: CXXFLAGS += -mavx512f -mavx512dq -mavx512bw -mavx512vl
src/distance/avx512_dispatcher.o: CXXFLAGS += -mavx512f -mavx512dq -mavx512bw -mavx512vl
src/distance/sse.bc: BITCODE_CXXFLAGS += -msse4.1
src/distance/sse_dispatcher.bc: BITCODE_CXXFLAGS += -msse4.1 -mfma
src/distance/avx.bc: BITCODE_CXXFLAGS += -mavx2 -mfma -mf16c
src/distance/avx_dispatcher.bc: BITCODE_CXXFLAGS += -mavx2 -mfma -mf16c
src/distance/avx512.bc: BITCODE_CXXFLAGS += -mavx512f -mavx512dq -mavx512bw -mavx512vl
src/distance/avx512_dispatcher.bc: BITCODE_CXXFLAGS += -mavx512f -mavx512dq -mavx512bw -mavx512vl

# Override PGXS implicit rule for C++
%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(PG_CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

../libvex-core/src/distance.o: CXXFLAGS += -fexceptions
../libvex-core/src/graph_algo.o: CXXFLAGS += -fexceptions
../libvex-core/src/node_store_memory.o: CXXFLAGS += -fexceptions
../libvex-core/src/rabitq.o: CXXFLAGS += -fexceptions

include $(PGXS)

override LLVM_BINPATH = /usr/lib64/llvm22/bin
