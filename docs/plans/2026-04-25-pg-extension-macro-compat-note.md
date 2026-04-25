# PG_EXTENSION Macro Compatibility Note

## Background

During the PostgreSQL 17 PGXS compatibility work for `vexdb-pg`, the plugin failed to start with:

- `undefined symbol: pg_vexdb_session`

The direct trigger was that `src/session_compat.cpp` only defined `pg_vexdb_session` under `#ifdef PG_EXTENSION`, while the active PGXS build path did not define that macro.

This note records the original intent of `PG_EXTENSION`, the current mismatch, and the compatibility rules to follow in subsequent refactoring.

## Current Code Facts

The macro currently appears in two places:

1. [vexdb-pg/CMakeLists.txt](/Users/sunji/Work/VexDB-Lite/vexdb-pg/CMakeLists.txt)
   - `add_compile_definitions(PG_EXTENSION HAVE_CXX_TYPEOF_UNQUAL)`
2. [vexdb-pg/src/session_compat.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/session_compat.cpp)
   - wraps the definition of `pg_vexdb_session` in `#ifdef PG_EXTENSION`

Related compatibility context is implemented in:

- [vexdb-pg/include/pg_compat.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/pg_compat.h)
- [vexdb-pg/src/guc_config.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/guc_config.cpp)
- [vexdb-pg/src/distance/distance.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/distance.cpp)

## Original Design Intent

`PG_EXTENSION` is not a PostgreSQL official predefined macro. In this repository, it is a project-local build marker.

Its likely original intent was:

1. Mark the build as targeting the PostgreSQL extension host.
2. Enable PostgreSQL-specific compatibility shims only in that host mode.
3. Avoid exporting or defining PostgreSQL-host-only globals in other build modes.

In other words, it acts as a host-environment switch, not an algorithm switch.

The kinds of things it was meant to gate are:

- PostgreSQL-specific global symbols
- PostgreSQL session compatibility stubs
- host-only compatibility glue around upstream/openGauss-style code

## Why It Failed Under PGXS

The current implementation assumes:

- every real PostgreSQL plugin build defines `PG_EXTENSION`

That assumption is false in the active build matrix.

Observed behavior:

1. The CMake build path defines `PG_EXTENSION` explicitly.
2. The PGXS build path does not define it.
3. `src/session_compat.cpp` therefore compiles to an effectively empty object under PGXS.
4. The final `pg_vexdb.so` keeps a reference to `pg_vexdb_session` from other translation units, but does not provide the definition.
5. PostgreSQL fails to load the shared object at startup.

So the problem is not the idea of the macro itself. The problem is using it to guard a runtime-required plugin symbol while the macro is not guaranteed across all supported build paths.

## Compatibility Rule Going Forward

The following rule should be enforced for `vexdb-pg`:

1. Runtime-required plugin symbols must not depend on optional build-path markers such as `PG_EXTENSION`.
2. Build markers may still be used for optional compatibility branches, diagnostics, or non-essential host glue.
3. Any symbol required by the installed PostgreSQL shared object to load successfully must be defined unconditionally in all supported official build paths, especially PGXS.

Applied to the current case:

- `pg_vexdb_session` is part of the required runtime compatibility surface.
- It should not remain behind `#ifdef PG_EXTENSION`.

## Engineering Implication

This incident separates two categories of compatibility code:

### Category A: required runtime surface

Examples:

- `pg_vexdb_session`
- mandatory hook entry points
- required GUC/session backing objects
- symbols that PostgreSQL or sibling translation units resolve at load time

These must be present in every supported plugin build.

### Category B: optional build-mode adaptation

Examples:

- local-only testing shims
- alternate host scaffolding
- optional compile-time diagnostics
- feature branches that are not required for plugin loading

These may remain behind build macros, provided the macros are consistently defined where needed.

## Recommended Follow-up

Near-term follow-up should be minimal and compatibility-oriented:

1. Remove the `PG_EXTENSION` guard around `pg_vexdb_session` definition in [session_compat.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/session_compat.cpp).
2. Export the symbol with the same linkage expected by PostgreSQL-facing code.
3. Rebuild through the PG17 PGXS path.
4. Verify the installed [pg_vexdb.so](/usr/pgsql-17/lib/pg_vexdb.so) no longer shows `U pg_vexdb_session` in `nm -D` output.
5. Restart the PostgreSQL validation instance and continue functional and benchmark validation.

## Scope Boundary

This note only records the macro intent and the compatibility issue it exposed.

It does not change:

- HNSW or `graph_index` algorithm design
- the `libvex-core` migration direction
- PostgreSQL feature enablement policy

The goal is strictly to make the plugin compatible with the official PostgreSQL 17 build and load path.
