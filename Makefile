MODULE_big = pg_vexdb

SRCS = \
    src/graph_index_am.cpp \
    src/session_compat.cpp \
    src/graph_index.cpp \
    src/graph_index_build.cpp \
    src/graph_index_insert.cpp \
    src/graph_index_inspect.cpp \
    src/graph_index_scan.cpp \
    src/graph_index_utils.cpp \
    src/graph_index_vacuum.cpp \
    src/graph_index_xlog.cpp \
    src/floatvector.cpp \
    src/halfvec.cpp \
    src/ann_utils.cpp \
    src/vector_storage.cpp \
    src/vector_smgr.cpp \
    knl/knl_alloc.cpp \
    src/distance/architecture_minimal.cpp \
    src/distance/distance.cpp \
    src/distance/general.cpp \
    src/distance/core/general_dispatcher.cpp \
    src/distance/core/sse_dispatcher.cpp \
    src/distance/core/avx_dispatcher.cpp \
    src/distance/core/avx512_dispatcher.cpp
# Defer full SIMD implementations:
#    src/distance/general.cpp \
#    src/distance/sse.cpp \
#    src/distance/avx.cpp \
#    src/distance/avx512.cpp \
#    src/distance/distances_simd_template.cpp

OBJS = $(SRCS:.cpp=.o)

EXTENSION = pg_vexdb
DATA = sql/pg_vexdb--1.0.sql

PG_CONFIG = /home/mingwei6/workspace/postgres/pg-install/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

PG_CPPFLAGS = -I$(CURDIR)/include -I$(CURDIR) -I$(CURDIR)/distance -I$(CURDIR)/distance/core -I$(CURDIR)/distance/pg -I$(CURDIR)/vec_types -I$(CURDIR)/rabitq -I$(CURDIR)/module -I/usr/include -DPG_EXTENSION -DHAVE_CXX_TYPEOF_UNQUAL -DPG_VEXDB_TARGET_PG

override CXXFLAGS := -std=c++17 -O2 -fPIC -Wno-error=vla -Wno-write-strings

ifneq ($(filter x86_64 amd64,$(shell uname -m)),)
    CXXFLAGS += -march=native
endif

ifneq ($(filter aarch64 arm64,$(shell uname -m)),)
    CXXFLAGS += -march=armv8-a+simd
endif

SHLIB_LINK += -lstdc++

# SIMD-specific flags
src/distance/sse.o: CXXFLAGS += -msse4.1
src/distance/avx.o: CXXFLAGS += -mavx2 -mfma
src/distance/avx512.o: CXXFLAGS += -mavx512f -mavx512dq -mavx512bw -mavx512vl
src/distance/core/sse_dispatcher.o: CXXFLAGS += -msse4.1
src/distance/core/avx_dispatcher.o: CXXFLAGS += -mavx2 -mfma
src/distance/core/avx512_dispatcher.o: CXXFLAGS += -mavx512f -mavx512dq -mavx512bw -mavx512vl

# Override PGXS implicit rule for C++
%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(PG_CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

include $(PGXS)
