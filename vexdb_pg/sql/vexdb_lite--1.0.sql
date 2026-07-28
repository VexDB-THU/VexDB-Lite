-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION vexdb_lite" to load this file. \quit

-- floatvector type

CREATE TYPE floatvector;

CREATE FUNCTION floatvector_in(cstring, oid, integer) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_out(floatvector) RETURNS cstring
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_typmod_in(cstring[]) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_recv(internal, oid, integer) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_send(floatvector) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE floatvector (
    INPUT     = floatvector_in,
    OUTPUT    = floatvector_out,
    TYPMOD_IN = floatvector_typmod_in,
    RECEIVE   = floatvector_recv,
    SEND      = floatvector_send,
    STORAGE   = external
);

-- floatvector distance functions

CREATE FUNCTION l2_distance(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_l2_squared_distance(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION inner_product(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_negative_inner_product(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cosine_distance(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_spherical_distance(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector utility functions

CREATE FUNCTION vector_dims(floatvector) RETURNS integer
    AS 'MODULE_PATHNAME', 'floatvector_dims' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_norm(floatvector) RETURNS float8
    AS 'MODULE_PATHNAME', 'floatvector_norm' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION l2_normalize(floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector private functions

CREATE FUNCTION floatvector_add(floatvector, floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_sub(floatvector, floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_lt(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_le(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_eq(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_ne(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_ge(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_gt(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_cmp(floatvector, floatvector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hashfloatvector(floatvector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector cast functions

CREATE FUNCTION floatvector(floatvector, integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'floatvector_to_floatvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_floatvector(real[], integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_floatvector(double precision[], integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'array_to_floatvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_floatvector(integer[], integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'array_to_floatvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_floatvector(numeric[], integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'array_to_floatvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_to_float4(floatvector, integer, boolean) RETURNS real[]
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector casts

CREATE CAST (floatvector AS floatvector)
    WITH FUNCTION floatvector(floatvector, integer, boolean) AS IMPLICIT;

CREATE CAST (floatvector AS real[])
    WITH FUNCTION floatvector_to_float4(floatvector, integer, boolean) AS IMPLICIT;

-- Accept all four common numeric array element types; the underlying C
-- impl already handles INT4 / FLOAT4 / FLOAT8 / NUMERIC element OIDs.
-- Without these, `INSERT … VALUES (array[1.2, 2.2, 3.3])` fails because
-- the array literal infers `numeric[]`, for which no cast existed.
CREATE CAST (real[] AS floatvector)
    WITH FUNCTION array_to_floatvector(real[], integer, boolean) AS ASSIGNMENT;

CREATE CAST (double precision[] AS floatvector)
    WITH FUNCTION array_to_floatvector(double precision[], integer, boolean) AS ASSIGNMENT;

CREATE CAST (integer[] AS floatvector)
    WITH FUNCTION array_to_floatvector(integer[], integer, boolean) AS ASSIGNMENT;

CREATE CAST (numeric[] AS floatvector)
    WITH FUNCTION array_to_floatvector(numeric[], integer, boolean) AS ASSIGNMENT;

-- floatvector operators

CREATE OPERATOR <-> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = l2_distance,
    COMMUTATOR = '<->'
);

CREATE OPERATOR <#> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_negative_inner_product,
    COMMUTATOR = '<#>'
);

CREATE OPERATOR <=> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = cosine_distance,
    COMMUTATOR = '<=>'
);

-- Duck-side parity: <~> is negative inner product, matching DuckDB's <~> /
-- list_negative_inner_product. (Same procedure as <#>; <#> is the pgvector-style
-- alias, <~> keeps the operator token usable on DuckDB where # is a comment char.)
CREATE OPERATOR <~> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_negative_inner_product,
    COMMUTATOR = '<~>'
);

-- Duck-side parity: vector_add/vector_sub alias floatvector_add/sub.
CREATE FUNCTION vector_add(floatvector, floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'floatvector_add' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION vector_sub(floatvector, floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'floatvector_sub' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR + (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_add,
    COMMUTATOR = +
);

CREATE OPERATOR - (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_sub
);

CREATE OPERATOR < (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_lt,
    COMMUTATOR = >, NEGATOR = >=,
    RESTRICT = scalarltsel, JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_le,
    COMMUTATOR = >=, NEGATOR = >,
    RESTRICT = scalarlesel, JOIN = scalarlejoinsel
);

CREATE OPERATOR = (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_eq,
    COMMUTATOR = =, NEGATOR = <>,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR <> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_ne,
    COMMUTATOR = <>, NEGATOR = =,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR >= (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_ge,
    COMMUTATOR = <=, NEGATOR = <,
    RESTRICT = scalargesel, JOIN = scalargejoinsel
);

CREATE OPERATOR > (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_gt,
    COMMUTATOR = <, NEGATOR = <=,
    RESTRICT = scalargtsel, JOIN = scalargtjoinsel
);

-- floatvector opclasses

CREATE OPERATOR CLASS floatvector_ops
    DEFAULT FOR TYPE floatvector USING btree AS
    OPERATOR 1 <,
    OPERATOR 2 <=,
    OPERATOR 3 =,
    OPERATOR 4 >=,
    OPERATOR 5 >,
    FUNCTION 1 floatvector_cmp(floatvector, floatvector);

CREATE OPERATOR CLASS hash_floatvector_ops
    FOR TYPE floatvector USING hash AS
    OPERATOR 1 =,
    FUNCTION 1 hashfloatvector(floatvector);


-- access method

CREATE FUNCTION vexdb_graph_amhandler(internal) RETURNS index_am_handler
    AS 'MODULE_PATHNAME', 'graph_index_amhandler' LANGUAGE C;

CREATE ACCESS METHOD vexdb_graph
    TYPE INDEX
    HANDLER vexdb_graph_amhandler;

COMMENT ON ACCESS METHOD vexdb_graph IS 'Self-developed graph index access method for vector similarity search';

-- vexdb_graph opclasses for floatvector

CREATE OPERATOR CLASS floatvector_l2_ops
    FOR TYPE floatvector USING vexdb_graph AS
    OPERATOR 1 <-> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_l2_squared_distance(floatvector, floatvector);

CREATE OPERATOR CLASS floatvector_ip_ops
    FOR TYPE floatvector USING vexdb_graph AS
    -- <#> (strategy 1): pgvector 兼容写法。
    -- <~> (strategy 2): 跨引擎统一写法，与 DuckDB 的 <~> 负内积一致。
    -- 两者同为负内积；metric 来自索引元数据(FUNCTION 1)，与查询用哪个算符无关。
    OPERATOR 1 <#> (floatvector, floatvector) FOR ORDER BY float_ops,
    OPERATOR 2 <~> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_negative_inner_product(floatvector, floatvector);

CREATE OPERATOR CLASS floatvector_cosine_ops
    DEFAULT FOR TYPE floatvector USING vexdb_graph AS
    OPERATOR 1 <=> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_negative_inner_product(floatvector, floatvector),
    FUNCTION 2 vector_norm(floatvector);


-- inspect functions

CREATE FUNCTION index_inspect(regclass)
    RETURNS TABLE(attribute text, content text)
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

COMMENT ON FUNCTION index_inspect(regclass) IS
    'Returns statistics about a vexdb_graph index';

CREATE FUNCTION vectorbuffer_inspect()
    RETURNS TABLE(used_space text, elem_size text, elem_nums int8,
                  hit int8, miss int8, eviction_rate float8)
    AS 'MODULE_PATHNAME' LANGUAGE C;

COMMENT ON FUNCTION vectorbuffer_inspect() IS
    'Returns statistics about the vector buffer cache';

-- vexdb_index_info: SRF that lists all vexdb_graph indexes with metadata.
-- Schema mirrors duckdb/vexdb_duckdb/functions/index_info_function.cpp.
CREATE FUNCTION vexdb_index_info()
    RETURNS TABLE(
        index_name        text,
        indexname         text,
        index_type        text,
        table_name        text,
        partition_count   int4,
        node_count        int8,
        max_level         int4,
        dimension         int4,
        row_id_map_size   int8,
        m                 int4,
        ef_construction   int4,
        metric            text,
        use_pq            bool,
        pq_m              int4,
        memory_bytes      int8,
        pq_codes_bytes    int8,
        pq_codebook_bytes int8,
        memory_mode       text)
    AS 'MODULE_PATHNAME' LANGUAGE C;

COMMENT ON FUNCTION vexdb_index_info() IS
    'Lists all vexdb_graph indexes with metadata (mirrors duck-side schema)';


-- VexFS PostgreSQL adapter alpha
--
-- This is the first PostgreSQL vertical slice. PostgreSQL tables, MVCC, WAL
-- and the authenticated session role are authoritative. Mount processes are
-- deliberately not started from the extension.

CREATE SCHEMA _vexfs;
REVOKE ALL ON SCHEMA _vexfs FROM PUBLIC;

CREATE TABLE _vexfs.workspaces (
    workspace_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name text NOT NULL UNIQUE CHECK (name <> ''),
    state text NOT NULL DEFAULT 'active' CHECK (state IN ('active', 'importing')),
    owner_oid oid NOT NULL,
    owner_role name NOT NULL,
    root_inode bigint,
    head_commit bigint NOT NULL DEFAULT 0,
    history_floor_commit bigint NOT NULL DEFAULT 0 CHECK (history_floor_commit >= 0),
    cache_generation bigint NOT NULL DEFAULT 0 CHECK (cache_generation >= 0),
    quota_max_bytes bigint CHECK (quota_max_bytes IS NULL OR quota_max_bytes >= 0),
    quota_max_files bigint CHECK (quota_max_files IS NULL OR quota_max_files >= 0),
    quota_max_file_bytes bigint
        CHECK (quota_max_file_bytes IS NULL OR quota_max_file_bytes >= 0),
    live_files bigint NOT NULL DEFAULT 0 CHECK (live_files >= 0),
    live_bytes bigint NOT NULL DEFAULT 0 CHECK (live_bytes >= 0),
    retention_keep_versions integer NOT NULL DEFAULT 32
        CHECK (retention_keep_versions BETWEEN 0 AND 1000000),
    retention_keep_days integer NOT NULL DEFAULT 30
        CHECK (retention_keep_days BETWEEN 0 AND 36500),
    gc_paused boolean NOT NULL DEFAULT false,
    grep_index_enabled boolean NOT NULL DEFAULT false,
    grep_index_dirty boolean NOT NULL DEFAULT false,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

-- ACLs are immutable, content-addressed sets.  Inodes and snapshots reference
-- one set instead of copying the same inherited entries for every file.
CREATE TABLE _vexfs.acl_sets (
    acl_set_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    fingerprint text NOT NULL CHECK (fingerprint ~ '^[0-9a-f]{64}$'),
    canonical_acl jsonb NOT NULL CHECK (jsonb_typeof(canonical_acl) = 'array'),
    entry_count integer NOT NULL CHECK (entry_count BETWEEN 1 AND 1024),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (workspace_id, fingerprint)
);

CREATE INDEX vexfs_acl_sets_workspace_idx
    ON _vexfs.acl_sets(workspace_id, acl_set_id);

CREATE TABLE _vexfs.acl_set_entries (
    acl_set_id bigint NOT NULL REFERENCES _vexfs.acl_sets(acl_set_id) ON DELETE CASCADE,
    principal text NOT NULL CHECK (principal <> '' AND octet_length(principal) <= 255),
    effect text NOT NULL CHECK (effect IN ('allow', 'deny')),
    permissions text NOT NULL CHECK (permissions <> '' AND octet_length(permissions) <= 1024),
    inherit_flags integer NOT NULL DEFAULT 0 CHECK (inherit_flags BETWEEN 0 AND 255),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (acl_set_id, principal, effect)
);

CREATE INDEX vexfs_acl_set_entries_principal_idx
    ON _vexfs.acl_set_entries(principal, acl_set_id);

CREATE TABLE _vexfs.inodes (
    inode_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    kind text NOT NULL CHECK (kind IN ('file', 'directory', 'symlink')),
    mode integer NOT NULL CHECK (mode BETWEEN 0 AND 4095),
    owner_oid oid NOT NULL,
    owner_role name NOT NULL,
    owner_principal text NOT NULL,
    uid bigint NOT NULL DEFAULT 0 CHECK (uid BETWEEN 0 AND 4294967295),
    gid bigint NOT NULL DEFAULT 0 CHECK (gid BETWEEN 0 AND 4294967295),
    acl_set_id bigint REFERENCES _vexfs.acl_sets(acl_set_id) ON DELETE SET NULL,
    current_version bigint NOT NULL DEFAULT 0 CHECK (current_version >= 0),
    size_bytes bigint NOT NULL DEFAULT 0 CHECK (size_bytes >= 0),
    live boolean NOT NULL DEFAULT true,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    accessed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    modified_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    changed_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX vexfs_inodes_workspace_idx
    ON _vexfs.inodes(workspace_id, inode_id);
CREATE INDEX vexfs_inodes_acl_set_fk_idx
    ON _vexfs.inodes(acl_set_id) WHERE acl_set_id IS NOT NULL;

CREATE TABLE _vexfs.dentries (
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    parent_inode bigint NOT NULL REFERENCES _vexfs.inodes(inode_id) ON DELETE CASCADE,
    name text NOT NULL CHECK (name <> '' AND octet_length(name) <= 255),
    inode_id bigint NOT NULL REFERENCES _vexfs.inodes(inode_id) ON DELETE CASCADE,
    PRIMARY KEY (workspace_id, parent_inode, name)
);

CREATE INDEX vexfs_dentries_inode_idx
    ON _vexfs.dentries(workspace_id, inode_id);
CREATE INDEX vexfs_dentries_inode_fk_idx
    ON _vexfs.dentries(inode_id);
CREATE INDEX vexfs_dentries_parent_fk_idx
    ON _vexfs.dentries(parent_inode);

CREATE TABLE _vexfs.commits (
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    commit_no bigint NOT NULL CHECK (commit_no > 0),
    parent_commit bigint NOT NULL CHECK (parent_commit >= 0),
    operation text NOT NULL,
    path text NOT NULL DEFAULT '/' CHECK (path <> '' AND octet_length(path) <= 4096),
    created_by_oid oid NOT NULL,
    created_by name NOT NULL,
    session_id text CHECK (session_id IS NULL OR (session_id <> '' AND octet_length(session_id) <= 255)),
    run_id text CHECK (run_id IS NULL OR (run_id <> '' AND octet_length(run_id) <= 255)),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (workspace_id, commit_no)
);

-- One workspace commit may describe many filesystem changes.  Keeping the
-- header separate from these path-level rows avoids advancing workspace HEAD,
-- auditing and notifying once per file while preserving exact history.
CREATE TABLE _vexfs.commit_changes (
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    commit_no bigint NOT NULL,
    ordinal integer NOT NULL CHECK (ordinal > 0),
    operation text NOT NULL,
    path text NOT NULL CHECK (path <> '' AND octet_length(path) <= 4096),
    inode_id bigint,
    before_version bigint CHECK (before_version IS NULL OR before_version >= 0),
    after_version bigint CHECK (after_version IS NULL OR after_version >= 0),
    details jsonb NOT NULL DEFAULT '{}'::jsonb,
    PRIMARY KEY (workspace_id, commit_no, ordinal),
    FOREIGN KEY (workspace_id, commit_no)
        REFERENCES _vexfs.commits(workspace_id, commit_no) ON DELETE CASCADE
);

CREATE INDEX vexfs_commit_changes_inode_idx
    ON _vexfs.commit_changes(workspace_id, inode_id, commit_no DESC);
-- A legal VexFS path may be 4096 bytes, larger than one PostgreSQL btree item.
-- Index its stable digest so long paths never make history insertion fail.
CREATE INDEX vexfs_commit_changes_path_idx
    ON _vexfs.commit_changes(
        workspace_id,
        (pg_catalog.md5(path)),
        commit_no DESC);
-- Private, transaction-scoped state used while one SQL call publishes many
-- files.  It is disposable coordination data, not workspace history, so it is
-- intentionally excluded from pg_extension_config_dump and archives.
CREATE TABLE _vexfs.commit_batch_contexts (
    backend_pid integer NOT NULL,
    transaction_id xid8 NOT NULL,
    workspace_id bigint NOT NULL,
    commit_no bigint,
    next_ordinal integer NOT NULL DEFAULT 0 CHECK (next_ordinal >= 0),
    operation text NOT NULL,
    path text,
    inode_id bigint,
    details jsonb NOT NULL DEFAULT '{}'::jsonb,
    PRIMARY KEY (backend_pid, transaction_id),
    FOREIGN KEY (workspace_id, commit_no)
        REFERENCES _vexfs.commits(workspace_id, commit_no) ON DELETE CASCADE
);

CREATE INDEX vexfs_commit_batch_context_commit_idx
    ON _vexfs.commit_batch_contexts(workspace_id, commit_no);

CREATE TABLE _vexfs.manifests (
    manifest_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    file_size bigint NOT NULL CHECK (file_size >= 0),
    chunk_size integer NOT NULL CHECK (chunk_size = 65536),
    chunk_count integer NOT NULL CHECK (chunk_count >= 0),
    checksum text NOT NULL CHECK (checksum ~ '^[0-9a-f]{64}$'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

-- All empty files in a workspace share one immutable content root.
CREATE UNIQUE INDEX vexfs_manifests_empty_workspace_idx
    ON _vexfs.manifests(workspace_id)
    WHERE file_size = 0 AND chunk_count = 0;

CREATE TABLE _vexfs.chunks (
    chunk_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    content bytea NOT NULL,
    size_bytes integer NOT NULL CHECK (size_bytes BETWEEN 1 AND 65536),
    checksum text NOT NULL CHECK (checksum ~ '^[0-9a-f]{64}$'),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK (octet_length(content) = size_bytes)
);

CREATE INDEX vexfs_chunks_workspace_idx
    ON _vexfs.chunks(workspace_id, chunk_id);

CREATE TABLE _vexfs.manifest_chunks (
    manifest_id bigint NOT NULL REFERENCES _vexfs.manifests(manifest_id) ON DELETE CASCADE,
    chunk_no integer NOT NULL CHECK (chunk_no >= 0),
    chunk_id bigint NOT NULL REFERENCES _vexfs.chunks(chunk_id),
    PRIMARY KEY (manifest_id, chunk_no)
);

CREATE INDEX vexfs_manifest_chunks_chunk_idx
    ON _vexfs.manifest_chunks(chunk_id);

CREATE TABLE _vexfs.xattrs (
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    inode_id bigint NOT NULL REFERENCES _vexfs.inodes(inode_id) ON DELETE CASCADE,
    name text NOT NULL CHECK (name <> '' AND octet_length(name) <= 255),
    value bytea NOT NULL CHECK (octet_length(value) <= 65536),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (workspace_id, inode_id, name)
);

CREATE INDEX vexfs_xattrs_inode_fk_idx
    ON _vexfs.xattrs(inode_id);

-- Compatibility read model used by permission checks and archive export.
-- Mutations go through the ACL API so changing one inode never mutates a set
-- shared by another inode or snapshot.
CREATE VIEW _vexfs.acl_entries AS
SELECT inode.workspace_id,
       inode.inode_id,
       entry.principal,
       entry.effect,
       entry.permissions,
       entry.inherit_flags,
       entry.created_at,
       entry.updated_at
  FROM _vexfs.inodes AS inode
  JOIN _vexfs.acl_set_entries AS entry
    ON entry.acl_set_id = inode.acl_set_id;

CREATE TABLE _vexfs.file_versions (
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    inode_id bigint NOT NULL REFERENCES _vexfs.inodes(inode_id) ON DELETE CASCADE,
    version_no bigint NOT NULL CHECK (version_no > 0),
    commit_no bigint NOT NULL,
    manifest_id bigint REFERENCES _vexfs.manifests(manifest_id),
    source_version_no bigint,
    size_bytes bigint NOT NULL CHECK (size_bytes >= 0),
    checksum text NOT NULL CHECK (checksum ~ '^[0-9a-f]{64}$'),
    created_by_oid oid NOT NULL,
    created_by name NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK ((manifest_id IS NOT NULL AND source_version_no IS NULL) OR
           (manifest_id IS NULL AND source_version_no IS NOT NULL)),
    PRIMARY KEY (workspace_id, inode_id, version_no),
    FOREIGN KEY (workspace_id, commit_no)
        REFERENCES _vexfs.commits(workspace_id, commit_no) ON DELETE CASCADE
);

CREATE INDEX vexfs_file_versions_inode_fk_idx
    ON _vexfs.file_versions(inode_id);
CREATE INDEX vexfs_file_versions_manifest_fk_idx
    ON _vexfs.file_versions(manifest_id);
CREATE INDEX vexfs_file_versions_commit_fk_idx
    ON _vexfs.file_versions(workspace_id, commit_no);

-- Optional, derived search state. This table is deliberately not registered
-- with pg_extension_config_dump: authoritative file versions are restored
-- first, then an enabled workspace reports dirty until an explicit rebuild.
CREATE TABLE _vexfs.grep_documents (
    inode_id bigint PRIMARY KEY REFERENCES _vexfs.inodes(inode_id) ON DELETE CASCADE,
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    version_no bigint NOT NULL CHECK (version_no > 0),
    content text NOT NULL,
    indexed_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX vexfs_grep_documents_workspace_idx
    ON _vexfs.grep_documents(workspace_id, inode_id);

CREATE TABLE _vexfs.snapshots (
    snapshot_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    name text NOT NULL CHECK (name <> ''),
    head_commit bigint NOT NULL CHECK (head_commit > 0),
    created_by_oid oid NOT NULL,
    created_by name NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (workspace_id, name),
    FOREIGN KEY (workspace_id, head_commit)
        REFERENCES _vexfs.commits(workspace_id, commit_no)
);

CREATE INDEX vexfs_snapshots_commit_fk_idx
    ON _vexfs.snapshots(workspace_id, head_commit);

CREATE TABLE _vexfs.snapshot_inodes (
    snapshot_id bigint NOT NULL REFERENCES _vexfs.snapshots(snapshot_id) ON DELETE CASCADE,
    inode_id bigint NOT NULL,
    kind text NOT NULL CHECK (kind IN ('file', 'directory', 'symlink')),
    mode integer NOT NULL CHECK (mode BETWEEN 0 AND 4095),
    owner_oid oid NOT NULL,
    owner_role name NOT NULL,
    owner_principal text NOT NULL,
    uid bigint NOT NULL CHECK (uid BETWEEN 0 AND 4294967295),
    gid bigint NOT NULL CHECK (gid BETWEEN 0 AND 4294967295),
    acl_set_id bigint REFERENCES _vexfs.acl_sets(acl_set_id) ON DELETE SET NULL,
    current_version bigint NOT NULL CHECK (current_version >= 0),
    size_bytes bigint NOT NULL CHECK (size_bytes >= 0),
    created_at timestamptz NOT NULL,
    accessed_at timestamptz NOT NULL,
    modified_at timestamptz NOT NULL,
    changed_at timestamptz NOT NULL,
    PRIMARY KEY (snapshot_id, inode_id)
);

CREATE INDEX vexfs_snapshot_inodes_acl_set_fk_idx
    ON _vexfs.snapshot_inodes(acl_set_id) WHERE acl_set_id IS NOT NULL;

CREATE TABLE _vexfs.snapshot_dentries (
    snapshot_id bigint NOT NULL REFERENCES _vexfs.snapshots(snapshot_id) ON DELETE CASCADE,
    parent_inode bigint NOT NULL,
    name text NOT NULL,
    inode_id bigint NOT NULL,
    PRIMARY KEY (snapshot_id, parent_inode, name)
);

CREATE TABLE _vexfs.snapshot_xattrs (
    snapshot_id bigint NOT NULL REFERENCES _vexfs.snapshots(snapshot_id) ON DELETE CASCADE,
    inode_id bigint NOT NULL,
    name text NOT NULL,
    value bytea NOT NULL,
    updated_at timestamptz NOT NULL,
    PRIMARY KEY (snapshot_id, inode_id, name)
);

CREATE VIEW _vexfs.snapshot_acl_entries AS
SELECT inode.snapshot_id,
       inode.inode_id,
       entry.principal,
       entry.effect,
       entry.permissions,
       entry.inherit_flags,
       entry.created_at,
       entry.updated_at
  FROM _vexfs.snapshot_inodes AS inode
  JOIN _vexfs.acl_set_entries AS entry
    ON entry.acl_set_id = inode.acl_set_id;

CREATE TABLE _vexfs.audit_events (
    event_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    workspace_id bigint REFERENCES _vexfs.workspaces(workspace_id) ON DELETE SET NULL,
    workspace_name text NOT NULL,
    commit_no bigint,
    actor_oid oid NOT NULL,
    actor_role name NOT NULL,
    operation text NOT NULL,
    path text,
    inode_id bigint,
    details jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX vexfs_audit_events_workspace_idx
    ON _vexfs.audit_events(workspace_id, event_id DESC);

CREATE INDEX vexfs_audit_events_workspace_name_idx
    ON _vexfs.audit_events(workspace_name, event_id DESC);

-- format v2 import is streamed through these transaction-local job rows. They
-- are deliberately not extension configuration data: a committed unfinished
-- job is disposable staging, never authoritative workspace content.
CREATE TABLE _vexfs.archive_import_jobs (
    job_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    workspace_name text NOT NULL,
    owner_oid oid NOT NULL,
    owner_role name NOT NULL,
    backend_pid integer NOT NULL,
    manifest jsonb NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE _vexfs.archive_import_records (
    job_id bigint NOT NULL REFERENCES _vexfs.archive_import_jobs(job_id) ON DELETE CASCADE,
    record_type text NOT NULL,
    record_key text NOT NULL,
    record_json jsonb NOT NULL,
    content bytea,
    PRIMARY KEY (job_id, record_type, record_key)
);

CREATE TABLE _vexfs.mount_sessions (
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    session_id text NOT NULL CHECK (session_id <> '' AND octet_length(session_id) <= 255),
    owner_oid oid NOT NULL,
    owner_role name NOT NULL,
    lease_until timestamptz NOT NULL,
    started_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    heartbeat_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (workspace_id, session_id)
);

CREATE INDEX vexfs_mount_sessions_lease_idx
    ON _vexfs.mount_sessions(workspace_id, lease_until);

CREATE TABLE _vexfs.handles (
    handle_id text PRIMARY KEY CHECK (handle_id <> '' AND octet_length(handle_id) <= 128),
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    inode_id bigint REFERENCES _vexfs.inodes(inode_id) ON DELETE SET NULL,
    path text NOT NULL,
    flags text NOT NULL,
    create_mode integer CHECK (create_mode IS NULL OR create_mode BETWEEN 0 AND 4095),
    owner_oid oid NOT NULL,
    owner_role name NOT NULL,
    session_id text,
    expected_version bigint NOT NULL CHECK (expected_version >= 0),
    dirty_generation bigint NOT NULL DEFAULT 0 CHECK (dirty_generation >= 0),
    published_generation bigint NOT NULL DEFAULT 0 CHECK (published_generation >= 0),
    state text NOT NULL DEFAULT 'open' CHECK (state IN ('open', 'retained', 'closed')),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    lease_until timestamptz NOT NULL DEFAULT clock_timestamp() + interval '30 seconds'
);

CREATE INDEX vexfs_handles_workspace_state_idx
    ON _vexfs.handles(workspace_id, state, lease_until);
CREATE INDEX vexfs_handles_inode_idx
    ON _vexfs.handles(workspace_id, inode_id);
CREATE INDEX vexfs_handles_inode_fk_idx
    ON _vexfs.handles(inode_id);

-- Writable handles keep only a reference to their published base plus the
-- chunks changed since that base. A small positional write therefore stores at
-- most the affected 64 KiB chunks instead of replacing one full-file bytea.
CREATE TABLE _vexfs.handle_staging (
    handle_id text PRIMARY KEY REFERENCES _vexfs.handles(handle_id) ON DELETE CASCADE,
    generation bigint NOT NULL CHECK (generation >= 0),
    base_manifest_id bigint REFERENCES _vexfs.manifests(manifest_id),
    base_size bigint NOT NULL CHECK (base_size >= 0),
    base_visible_size bigint NOT NULL CHECK (base_visible_size >= 0),
    logical_size bigint NOT NULL CHECK (logical_size BETWEEN 0 AND 134217728),
    dirty_bytes bigint NOT NULL DEFAULT 0 CHECK (dirty_bytes >= 0),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK (base_visible_size <= base_size)
);

CREATE INDEX vexfs_handle_staging_updated_idx
    ON _vexfs.handle_staging(updated_at, handle_id);
CREATE INDEX vexfs_handle_staging_base_manifest_idx
    ON _vexfs.handle_staging(base_manifest_id);

CREATE TABLE _vexfs.handle_staging_chunks (
    handle_id text NOT NULL REFERENCES _vexfs.handles(handle_id) ON DELETE CASCADE,
    chunk_no integer NOT NULL CHECK (chunk_no >= 0),
    content bytea NOT NULL CHECK (
        octet_length(content) BETWEEN 1 AND 65536),
    PRIMARY KEY (handle_id, chunk_no)
);

CREATE TABLE _vexfs.request_replays (
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    owner_oid oid NOT NULL,
    request_id text NOT NULL CHECK (request_id <> '' AND octet_length(request_id) <= 255),
    operation text NOT NULL,
    argument_hash text NOT NULL CHECK (argument_hash ~ '^[0-9a-f]{64}$'),
    result_text text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (workspace_id, owner_oid, request_id)
);

CREATE INDEX vexfs_request_replays_created_idx
    ON _vexfs.request_replays(workspace_id, created_at);

CREATE TABLE _vexfs.file_locks (
    workspace_id bigint NOT NULL REFERENCES _vexfs.workspaces(workspace_id) ON DELETE CASCADE,
    inode_id bigint NOT NULL REFERENCES _vexfs.inodes(inode_id) ON DELETE CASCADE,
    owner_oid oid NOT NULL,
    session_id text NOT NULL,
    lock_kind text NOT NULL CHECK (lock_kind IN ('shared', 'exclusive')),
    lease_until timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (workspace_id, inode_id, owner_oid, session_id)
);

CREATE INDEX vexfs_file_locks_lease_idx
    ON _vexfs.file_locks(workspace_id, inode_id, lease_until);
CREATE INDEX vexfs_file_locks_inode_fk_idx
    ON _vexfs.file_locks(inode_id);

-- PostgreSQL excludes extension-owned table data from pg_dump unless every
-- authoritative relation is registered as extension configuration data.
-- The workspace root reference is intentionally validated by vexfs_check
-- instead of a circular workspaces -> inodes foreign key because circular
-- configuration-table dependencies cannot be restored safely by pg_dump.
-- Version aliases are validated the same way instead of using a self-referencing
-- file_versions foreign key, which would create another circular restore order.
SELECT pg_catalog.pg_extension_config_dump('_vexfs.workspaces', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.acl_sets', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.acl_set_entries', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.inodes', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.dentries', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.commits', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.commit_changes', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.manifests', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.chunks', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.manifest_chunks', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.file_versions', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.xattrs', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.snapshots', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.snapshot_inodes', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.snapshot_dentries', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.snapshot_xattrs', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.audit_events', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.workspaces_workspace_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.acl_sets_acl_set_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.inodes_inode_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.manifests_manifest_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.chunks_chunk_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.snapshots_snapshot_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('_vexfs.audit_events_event_id_seq', '');

CREATE FUNCTION _vexfs.reset_live_usage_on_workspace_insert()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
BEGIN
    -- pg_dump restores the saved workspace row before inode rows. Rebuild the
    -- derived counters through inode triggers instead of counting them twice.
    NEW.live_files := 0;
    NEW.live_bytes := 0;
    NEW.grep_index_dirty := NEW.grep_index_enabled;
    RETURN NEW;
END;
$$;

CREATE TRIGGER vexfs_workspace_reset_live_usage
BEFORE INSERT ON _vexfs.workspaces
FOR EACH ROW EXECUTE FUNCTION _vexfs.reset_live_usage_on_workspace_insert();

CREATE FUNCTION _vexfs.update_live_usage_after_insert()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
BEGIN
    UPDATE _vexfs.workspaces AS workspace
       SET live_files = workspace.live_files + delta.files,
           live_bytes = workspace.live_bytes + delta.bytes
      FROM (
          SELECT inserted.workspace_id,
                 count(*) AS files,
                 coalesce(sum(inserted.size_bytes), 0) AS bytes
            FROM inserted_inodes AS inserted
           WHERE inserted.kind <> 'directory' AND inserted.live
           GROUP BY inserted.workspace_id) AS delta
     WHERE workspace.workspace_id = delta.workspace_id;
    RETURN NULL;
END;
$$;

CREATE TRIGGER vexfs_inode_live_usage_insert
AFTER INSERT ON _vexfs.inodes
REFERENCING NEW TABLE AS inserted_inodes
FOR EACH STATEMENT EXECUTE FUNCTION _vexfs.update_live_usage_after_insert();

CREATE FUNCTION _vexfs.update_live_usage_after_delete()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
BEGIN
    UPDATE _vexfs.workspaces AS workspace
       SET live_files = workspace.live_files - delta.files,
           live_bytes = workspace.live_bytes - delta.bytes
      FROM (
          SELECT removed.workspace_id,
                 count(*) AS files,
                 coalesce(sum(removed.size_bytes), 0) AS bytes
            FROM removed_inodes AS removed
           WHERE removed.kind <> 'directory' AND removed.live
           GROUP BY removed.workspace_id) AS delta
     WHERE workspace.workspace_id = delta.workspace_id;
    RETURN NULL;
END;
$$;

CREATE TRIGGER vexfs_inode_live_usage_delete
AFTER DELETE ON _vexfs.inodes
REFERENCING OLD TABLE AS removed_inodes
FOR EACH STATEMENT EXECUTE FUNCTION _vexfs.update_live_usage_after_delete();

CREATE FUNCTION _vexfs.update_live_usage_after_update()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
BEGIN
    UPDATE _vexfs.workspaces AS workspace
       SET live_files = workspace.live_files + delta.files,
           live_bytes = workspace.live_bytes + delta.bytes
      FROM (
          SELECT workspace_id,
                 sum(files)::bigint AS files,
                 sum(bytes)::bigint AS bytes
            FROM (
                SELECT old_row.workspace_id,
                       -count(*) AS files,
                       -coalesce(sum(old_row.size_bytes), 0) AS bytes
                  FROM previous_inodes AS old_row
                 WHERE old_row.kind <> 'directory' AND old_row.live
                 GROUP BY old_row.workspace_id
                UNION ALL
                SELECT new_row.workspace_id,
                       count(*) AS files,
                       coalesce(sum(new_row.size_bytes), 0) AS bytes
                  FROM updated_inodes AS new_row
                 WHERE new_row.kind <> 'directory' AND new_row.live
                 GROUP BY new_row.workspace_id) AS changes
           GROUP BY workspace_id
          HAVING sum(files) <> 0 OR sum(bytes) <> 0) AS delta
     WHERE workspace.workspace_id = delta.workspace_id;
    RETURN NULL;
END;
$$;

CREATE TRIGGER vexfs_inode_live_usage_update
AFTER UPDATE ON _vexfs.inodes
REFERENCING OLD TABLE AS previous_inodes NEW TABLE AS updated_inodes
FOR EACH STATEMENT EXECUTE FUNCTION _vexfs.update_live_usage_after_update();

CREATE FUNCTION _vexfs.path_parts(p_path text)
RETURNS text[]
LANGUAGE plpgsql
IMMUTABLE
STRICT
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_parts text[];
    v_part text;
BEGIN
    IF left(p_path, 1) <> '/' THEN
        RAISE EXCEPTION 'VEXFS_INVALID_PATH: path must be absolute'
            USING ERRCODE = '22023';
    END IF;
    IF p_path = '/' THEN
        RETURN ARRAY[]::text[];
    END IF;

    v_parts := string_to_array(substr(p_path, 2), '/');
    FOREACH v_part IN ARRAY v_parts LOOP
        IF v_part = '' OR v_part = '.' OR v_part = '..' THEN
            RAISE EXCEPTION 'VEXFS_INVALID_PATH: invalid path component'
                USING ERRCODE = '22023';
        END IF;
        IF octet_length(v_part) > 255 THEN
            RAISE EXCEPTION 'VEXFS_INVALID_PATH: path component is too long'
                USING ERRCODE = '22023';
        END IF;
    END LOOP;
    RETURN v_parts;
END;
$$;

CREATE FUNCTION _vexfs.canonical_acl(p_acl jsonb)
RETURNS jsonb
LANGUAGE plpgsql
IMMUTABLE
PARALLEL SAFE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_canonical jsonb;
BEGIN
    IF p_acl IS NULL OR jsonb_typeof(p_acl) <> 'array'
       OR jsonb_array_length(p_acl) > 1024 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ACL: ACL must be an array with at most 1024 entries'
            USING ERRCODE = '22023';
    END IF;
    IF EXISTS (
        SELECT 1
         FROM jsonb_array_elements(p_acl) AS item(value)
         WHERE jsonb_typeof(item.value) <> 'object'
            OR coalesce(item.value->'principal',
                        item.value->'principal_id') IS NULL
            OR jsonb_typeof(coalesce(item.value->'principal',
                                     item.value->'principal_id')) <> 'string'
            OR (item.value ? 'effect'
                AND jsonb_typeof(item.value->'effect') <> 'string')
            OR item.value->'permissions' IS NULL
            OR jsonb_typeof(item.value->'permissions') <> 'string'
            OR ((item.value ? 'inherit' OR item.value ? 'inherit_flags')
                AND coalesce(item.value->>'inherit',
                             item.value->>'inherit_flags') !~ '^[0-9]+$')
            OR EXISTS (
                SELECT 1 FROM jsonb_object_keys(item.value) AS key
                 WHERE key NOT IN (
                     'principal', 'principal_id', 'effect', 'permissions',
                     'inherit', 'inherit_flags'))) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ACL: invalid ACL entry'
            USING ERRCODE = '22023';
    END IF;

    SELECT coalesce(jsonb_agg(jsonb_build_object(
               'principal', normalized.principal,
               'effect', normalized.effect,
               'permissions', normalized.permissions,
               'inherit', normalized.inherit_flags)
               ORDER BY normalized.principal, normalized.effect), '[]'::jsonb)
      INTO v_canonical
      FROM (
          SELECT coalesce(item.value->>'principal',
                          item.value->>'principal_id') AS principal,
                 lower(coalesce(item.value->>'effect', 'allow')) AS effect,
                 item.value->>'permissions' AS permissions,
                 coalesce(item.value->>'inherit',
                          item.value->>'inherit_flags', '0')::integer AS inherit_flags
            FROM jsonb_array_elements(p_acl) AS item(value)
      ) AS normalized;

    IF EXISTS (
        SELECT 1
         FROM jsonb_array_elements(v_canonical) AS item(value)
         WHERE item.value->>'principal' IS NULL
            OR item.value->>'principal' = ''
            OR octet_length(item.value->>'principal') > 255
            OR item.value->>'effect' NOT IN ('allow', 'deny')
            OR item.value->>'permissions' IS NULL
            OR item.value->>'permissions' = ''
            OR octet_length(item.value->>'permissions') > 1024
            OR (item.value->>'inherit')::integer NOT BETWEEN 0 AND 255) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ACL: invalid ACL entry'
            USING ERRCODE = '22023';
    END IF;
    IF EXISTS (
        SELECT 1
          FROM jsonb_array_elements(v_canonical) AS item(value)
         GROUP BY item.value->>'principal', item.value->>'effect'
        HAVING count(*) > 1) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ACL: principal/effect entries must be unique'
            USING ERRCODE = '22023';
    END IF;
    RETURN v_canonical;
END;
$$;

CREATE FUNCTION _vexfs.get_or_create_acl_set(
    p_workspace_id bigint,
    p_acl jsonb)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_canonical jsonb;
    v_fingerprint text;
    v_acl_set bigint;
    v_existing jsonb;
BEGIN
    v_canonical := _vexfs.canonical_acl(p_acl);
    IF jsonb_array_length(v_canonical) = 0 THEN
        RETURN NULL;
    END IF;
    PERFORM 1 FROM _vexfs.workspaces AS workspace
     WHERE workspace.workspace_id = p_workspace_id;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_WORKSPACE_NOT_FOUND: %', p_workspace_id
            USING ERRCODE = 'P0002';
    END IF;
    v_fingerprint := encode(pg_catalog.sha256(
        convert_to(v_canonical::text, 'UTF8')), 'hex');
    SELECT acl_set.acl_set_id, acl_set.canonical_acl
      INTO v_acl_set, v_existing
      FROM _vexfs.acl_sets AS acl_set
     WHERE acl_set.workspace_id = p_workspace_id
       AND acl_set.fingerprint = v_fingerprint;
    IF FOUND THEN
        IF v_existing IS DISTINCT FROM v_canonical THEN
            RAISE EXCEPTION 'VEXFS_CORRUPT: ACL fingerprint collision'
                USING ERRCODE = 'XX001';
        END IF;
        RETURN v_acl_set;
    END IF;

    INSERT INTO _vexfs.acl_sets(
        workspace_id, fingerprint, canonical_acl, entry_count)
    VALUES (
        p_workspace_id, v_fingerprint, v_canonical,
        jsonb_array_length(v_canonical))
    ON CONFLICT (workspace_id, fingerprint) DO NOTHING
    RETURNING acl_set_id INTO v_acl_set;
    IF v_acl_set IS NULL THEN
        SELECT acl_set.acl_set_id, acl_set.canonical_acl
          INTO v_acl_set, v_existing
          FROM _vexfs.acl_sets AS acl_set
         WHERE acl_set.workspace_id = p_workspace_id
           AND acl_set.fingerprint = v_fingerprint;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'VEXFS_ACL_RETRY: concurrent ACL set is not visible'
                USING ERRCODE = '40001';
        END IF;
        IF v_existing IS DISTINCT FROM v_canonical THEN
            RAISE EXCEPTION 'VEXFS_CORRUPT: ACL fingerprint collision'
                USING ERRCODE = 'XX001';
        END IF;
        RETURN v_acl_set;
    END IF;
    INSERT INTO _vexfs.acl_set_entries(
        acl_set_id, principal, effect, permissions, inherit_flags)
    SELECT v_acl_set,
           item.value->>'principal',
           item.value->>'effect',
           item.value->>'permissions',
           (item.value->>'inherit')::integer
      FROM jsonb_array_elements(v_canonical) AS item(value)
    ;
    RETURN v_acl_set;
END;
$$;

CREATE FUNCTION _vexfs.acl_json_for_inode(
    p_workspace_id bigint,
    p_inode_id bigint)
RETURNS jsonb
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT coalesce(set.canonical_acl, '[]'::jsonb)
      FROM _vexfs.inodes AS inode
      LEFT JOIN _vexfs.acl_sets AS set
        ON set.acl_set_id = inode.acl_set_id
     WHERE inode.workspace_id = p_workspace_id
       AND inode.inode_id = p_inode_id
$$;

CREATE FUNCTION _vexfs.permission_contains(
    p_permissions text,
    p_required text)
RETURNS boolean
LANGUAGE sql
IMMUTABLE
STRICT
PARALLEL SAFE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT EXISTS (
        SELECT 1
          FROM regexp_split_to_table(lower(p_permissions), '\s*,\s*') AS token
         WHERE token IN (lower(p_required), 'admin'))
$$;

CREATE FUNCTION _vexfs.has_acl_permission(
    p_workspace_id bigint,
    p_inode_id bigint,
    p_permission text)
RETURNS boolean
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_actor_oid oid;
    v_superuser boolean;
    v_owner_oid oid;
    v_denied boolean;
    v_allowed boolean;
BEGIN
    SELECT r.oid, r.rolsuper INTO STRICT v_actor_oid, v_superuser
      FROM pg_catalog.pg_roles AS r
     WHERE r.rolname = session_user;
    SELECT i.owner_oid INTO v_owner_oid
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = p_workspace_id
       AND i.inode_id = p_inode_id
       AND i.live;
    IF NOT FOUND THEN
        RETURN false;
    END IF;
    IF v_superuser OR v_owner_oid = v_actor_oid THEN
        RETURN true;
    END IF;

    SELECT coalesce(bool_or(
               acl.effect = 'deny'
               AND _vexfs.permission_contains(acl.permissions, p_permission)), false),
           coalesce(bool_or(
               acl.effect = 'allow'
               AND _vexfs.permission_contains(acl.permissions, p_permission)), false)
      INTO v_denied, v_allowed
      FROM _vexfs.acl_entries AS acl
      LEFT JOIN pg_catalog.pg_roles AS acl_role
        ON acl_role.rolname = acl.principal
     WHERE acl.workspace_id = p_workspace_id
       AND acl.inode_id = p_inode_id
       AND (acl.principal IN (session_user, 'public', '*')
            OR (acl_role.oid IS NOT NULL
                AND pg_catalog.pg_has_role(v_actor_oid, acl_role.oid, 'member')));
    RETURN NOT v_denied AND v_allowed;
END;
$$;

CREATE FUNCTION _vexfs.require_inode_permission(
    p_workspace_id bigint,
    p_inode_id bigint,
    p_permission text)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
BEGIN
    IF NOT _vexfs.has_acl_permission(
        p_workspace_id, p_inode_id, p_permission) THEN
        RAISE EXCEPTION 'VEXFS_PERMISSION_DENIED: % permission denied for inode %',
            p_permission, p_inode_id
            USING ERRCODE = '42501';
    END IF;
END;
$$;

CREATE FUNCTION _vexfs.require_workspace(
    p_name text,
    p_permission text DEFAULT 'read')
RETURNS _vexfs.workspaces
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_principal_oid oid;
    v_superuser boolean;
BEGIN
    SELECT * INTO v_workspace
      FROM _vexfs.workspaces AS w
     WHERE w.name = p_name;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_WORKSPACE_NOT_FOUND: %', p_name
            USING ERRCODE = 'P0002';
    END IF;

    SELECT r.oid, r.rolsuper INTO v_principal_oid, v_superuser
      FROM pg_catalog.pg_roles AS r
     WHERE r.rolname = session_user;
    IF v_workspace.state <> 'active' AND NOT coalesce(v_superuser, false) THEN
        RAISE EXCEPTION 'VEXFS_WORKSPACE_BUSY: workspace % is being imported', p_name
            USING ERRCODE = '55006';
    END IF;
    IF v_workspace.owner_oid <> v_principal_oid AND NOT coalesce(v_superuser, false) THEN
        IF v_workspace.root_inode IS NULL OR NOT _vexfs.has_acl_permission(
            v_workspace.workspace_id, v_workspace.root_inode, p_permission) THEN
            RAISE EXCEPTION 'VEXFS_PERMISSION_DENIED: workspace % belongs to role %',
                p_name, v_workspace.owner_role
                USING ERRCODE = '42501';
        END IF;
    END IF;
    RETURN v_workspace;
END;
$$;

CREATE FUNCTION _vexfs.resolve_path(p_workspace_id bigint, p_path text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_parts text[];
    v_part text;
    v_inode bigint;
    v_next bigint;
    v_kind text;
BEGIN
    SELECT w.root_inode INTO v_inode
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = p_workspace_id;
    IF v_inode IS NULL THEN
        RAISE EXCEPTION 'VEXFS_WORKSPACE_NOT_FOUND: %', p_workspace_id
            USING ERRCODE = 'P0002';
    END IF;

    v_parts := _vexfs.path_parts(p_path);
    FOREACH v_part IN ARRAY v_parts LOOP
        PERFORM _vexfs.require_inode_permission(
            p_workspace_id, v_inode, 'execute');
        SELECT d.inode_id INTO v_next
          FROM _vexfs.dentries AS d
         WHERE d.workspace_id = p_workspace_id
           AND d.parent_inode = v_inode
           AND d.name = v_part;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'VEXFS_PATH_NOT_FOUND: %', p_path
                USING ERRCODE = 'P0002';
        END IF;
        SELECT i.kind INTO v_kind
          FROM _vexfs.inodes AS i
         WHERE i.inode_id = v_next;
        v_inode := v_next;
    END LOOP;
    RETURN v_inode;
END;
$$;

CREATE FUNCTION _vexfs.resolve_parent(p_workspace_id bigint, p_path text)
RETURNS TABLE(parent_inode bigint, entry_name text)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_parts text[];
    v_count integer;
    v_parent_path text;
BEGIN
    v_parts := _vexfs.path_parts(p_path);
    v_count := coalesce(array_length(v_parts, 1), 0);
    IF v_count = 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_PATH: workspace root has no parent'
            USING ERRCODE = '22023';
    END IF;

    entry_name := v_parts[v_count];
    IF v_count = 1 THEN
        v_parent_path := '/';
    ELSE
        v_parent_path := '/' || array_to_string(v_parts[1:v_count - 1], '/');
    END IF;
    parent_inode := _vexfs.resolve_path(p_workspace_id, v_parent_path);
    RETURN NEXT;
END;
$$;

CREATE FUNCTION _vexfs.path_for_inode(
    p_workspace_id bigint,
    p_inode_id bigint)
RETURNS text
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    WITH RECURSIVE paths(inode_id, path, visited) AS (
        SELECT w.root_inode, '/'::text, ARRAY[w.root_inode]::bigint[]
          FROM _vexfs.workspaces AS w
         WHERE w.workspace_id = p_workspace_id
        UNION ALL
        SELECT d.inode_id,
               CASE WHEN paths.path = '/' THEN '/' || d.name
                    ELSE paths.path || '/' || d.name END,
               paths.visited || d.inode_id
          FROM paths
          JOIN _vexfs.dentries AS d
            ON d.workspace_id = p_workspace_id
           AND d.parent_inode = paths.inode_id
         WHERE NOT d.inode_id = ANY(paths.visited)
           AND octet_length(paths.path) + octet_length(d.name) + 1 <= 4096)
    SELECT paths.path
      FROM paths
     WHERE paths.inode_id = p_inode_id
     ORDER BY paths.path
     LIMIT 1
$$;

CREATE FUNCTION _vexfs.record_commit_header(
    p_workspace_id bigint,
    p_operation text,
    p_path text DEFAULT NULL,
    p_inode_id bigint DEFAULT NULL,
    p_details jsonb DEFAULT '{}'::jsonb)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_commit bigint;
    v_principal_oid oid;
    v_session_id text;
    v_run_id text;
BEGIN
    SELECT r.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS r
     WHERE r.rolname = session_user;

    UPDATE _vexfs.workspaces
       SET head_commit = head_commit + 1,
           history_floor_commit = CASE
               WHEN history_floor_commit = 0 THEN head_commit + 1
               ELSE history_floor_commit
           END,
           cache_generation = cache_generation + 1
     WHERE workspace_id = p_workspace_id
    RETURNING head_commit INTO v_commit;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_WORKSPACE_NOT_FOUND: %', p_workspace_id
            USING ERRCODE = 'P0002';
    END IF;

    v_session_id := nullif(pg_catalog.current_setting('vexfs.session_id', true), '');
    v_run_id := nullif(pg_catalog.current_setting('vexfs.run_id', true), '');
    IF v_session_id IS NULL AND p_inode_id IS NOT NULL THEN
        SELECT handle.session_id INTO v_session_id
          FROM _vexfs.handles AS handle
         WHERE handle.workspace_id = p_workspace_id
           AND handle.inode_id = p_inode_id
           AND handle.owner_oid = v_principal_oid
           AND handle.session_id IS NOT NULL
         ORDER BY handle.updated_at DESC, handle.handle_id DESC
         LIMIT 1;
    END IF;
    IF v_session_id IS NOT NULL AND octet_length(v_session_id) > 255 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SESSION: session id is longer than 255 bytes'
            USING ERRCODE = '22023';
    END IF;
    IF v_run_id IS NOT NULL AND octet_length(v_run_id) > 255 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RUN: run id is longer than 255 bytes'
            USING ERRCODE = '22023';
    END IF;

    INSERT INTO _vexfs.commits(
        workspace_id, commit_no, parent_commit, operation, path,
        created_by_oid, created_by, session_id, run_id)
    VALUES (
        p_workspace_id, v_commit, v_commit - 1, p_operation, coalesce(p_path, '/'),
        v_principal_oid, session_user, v_session_id, v_run_id);
    PERFORM _vexfs.audit(
        p_workspace_id, v_commit, p_operation,
        p_path, p_inode_id, p_details);
    PERFORM pg_catalog.pg_notify(
        'vexfs_change',
        jsonb_build_object(
            'workspace_id', p_workspace_id,
            'head_commit', v_commit,
            'operation', p_operation)::text);
    RETURN v_commit;
END;
$$;

CREATE FUNCTION _vexfs.record_commit(
    p_workspace_id bigint,
    p_operation text,
    p_path text DEFAULT NULL,
    p_inode_id bigint DEFAULT NULL,
    p_details jsonb DEFAULT '{}'::jsonb)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_commit bigint;
    v_ordinal integer;
    v_batch _vexfs.commit_batch_contexts%ROWTYPE;
BEGIN
    SELECT * INTO v_batch
      FROM _vexfs.commit_batch_contexts AS context
     WHERE context.backend_pid = pg_catalog.pg_backend_pid()
       AND context.transaction_id = pg_catalog.pg_current_xact_id()
       AND context.workspace_id = p_workspace_id
     FOR UPDATE;
    IF FOUND THEN
        IF v_batch.commit_no IS NULL THEN
            v_commit := _vexfs.record_commit_header(
                p_workspace_id, v_batch.operation, v_batch.path,
                v_batch.inode_id, v_batch.details);
            v_ordinal := 1;
            UPDATE _vexfs.commit_batch_contexts AS context
               SET commit_no = v_commit,
                   next_ordinal = v_ordinal
             WHERE context.backend_pid = pg_catalog.pg_backend_pid()
               AND context.transaction_id = pg_catalog.pg_current_xact_id();
        ELSE
            UPDATE _vexfs.commit_batch_contexts AS context
               SET next_ordinal = context.next_ordinal + 1
             WHERE context.backend_pid = pg_catalog.pg_backend_pid()
               AND context.transaction_id = pg_catalog.pg_current_xact_id()
            RETURNING context.commit_no, context.next_ordinal
                 INTO v_commit, v_ordinal;
        END IF;
    ELSE
        v_commit := _vexfs.record_commit_header(
            p_workspace_id, p_operation, p_path, p_inode_id, p_details);
        v_ordinal := 1;
    END IF;
    INSERT INTO _vexfs.commit_changes(
        workspace_id, commit_no, ordinal, operation, path, inode_id,
        before_version, after_version, details)
    VALUES (
        p_workspace_id, v_commit, v_ordinal, p_operation, coalesce(p_path, '/'), p_inode_id,
        nullif(p_details->>'before_version', '')::bigint,
        nullif(p_details->>'after_version', '')::bigint,
        coalesce(p_details, '{}'::jsonb));
    RETURN v_commit;
END;
$$;

CREATE FUNCTION _vexfs.begin_commit_batch(
    p_workspace_id bigint,
    p_operation text,
    p_path text DEFAULT NULL,
    p_inode_id bigint DEFAULT NULL,
    p_details jsonb DEFAULT '{}'::jsonb)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_existing xid8;
BEGIN
    SELECT context.transaction_id INTO v_existing
      FROM _vexfs.commit_batch_contexts AS context
     WHERE context.backend_pid = pg_catalog.pg_backend_pid()
       AND context.transaction_id = pg_catalog.pg_current_xact_id();
    IF FOUND THEN
        RAISE EXCEPTION 'VEXFS_INTERNAL: nested commit batch is not supported'
            USING ERRCODE = 'XX000';
    END IF;
    DELETE FROM _vexfs.commit_batch_contexts AS context
     WHERE context.backend_pid = pg_catalog.pg_backend_pid();
    INSERT INTO _vexfs.commit_batch_contexts(
        backend_pid, transaction_id, workspace_id,
        operation, path, inode_id, details)
    VALUES (
        pg_catalog.pg_backend_pid(), pg_catalog.pg_current_xact_id(),
        p_workspace_id, p_operation, p_path, p_inode_id,
        coalesce(p_details, '{}'::jsonb));
    RETURN 0;
END;
$$;

CREATE FUNCTION _vexfs.end_commit_batch(p_workspace_id bigint)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_removed integer;
BEGIN
    DELETE FROM _vexfs.commit_batch_contexts AS context
     WHERE context.backend_pid = pg_catalog.pg_backend_pid()
       AND context.transaction_id = pg_catalog.pg_current_xact_id()
       AND context.workspace_id = p_workspace_id;
    GET DIAGNOSTICS v_removed = ROW_COUNT;
    IF v_removed <> 1 THEN
        RAISE EXCEPTION 'VEXFS_INTERNAL: commit batch context is missing'
            USING ERRCODE = 'XX000';
    END IF;
    RETURN v_removed;
END;
$$;

CREATE FUNCTION _vexfs.audit(
    p_workspace_id bigint,
    p_commit_no bigint,
    p_operation text,
    p_path text DEFAULT NULL,
    p_inode_id bigint DEFAULT NULL,
    p_details jsonb DEFAULT '{}'::jsonb)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_event bigint;
    v_actor_oid oid;
    v_workspace_name text;
BEGIN
    SELECT r.oid INTO STRICT v_actor_oid
      FROM pg_catalog.pg_roles AS r
     WHERE r.rolname = session_user;
    SELECT w.name INTO STRICT v_workspace_name
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = p_workspace_id;
    INSERT INTO _vexfs.audit_events(
        workspace_id, workspace_name, commit_no, actor_oid, actor_role,
        operation, path, inode_id, details)
    VALUES (
        p_workspace_id, v_workspace_name, p_commit_no, v_actor_oid, session_user,
        p_operation, p_path, p_inode_id, coalesce(p_details, '{}'::jsonb))
    RETURNING event_id INTO v_event;
    RETURN v_event;
END;
$$;

CREATE FUNCTION _vexfs.inherited_acl_set_id(
    p_workspace_id bigint,
    p_parent_inode bigint)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_parent_set bigint;
    v_total integer;
    v_inherited jsonb;
BEGIN
    SELECT inode.acl_set_id INTO v_parent_set
      FROM _vexfs.inodes AS inode
     WHERE inode.workspace_id = p_workspace_id
       AND inode.inode_id = p_parent_inode;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_parent_inode
            USING ERRCODE = 'P0002';
    END IF;
    IF v_parent_set IS NULL THEN
        RETURN NULL;
    END IF;
    SELECT set.entry_count,
           coalesce(jsonb_agg(jsonb_build_object(
               'principal', entry.principal,
               'effect', entry.effect,
               'permissions', entry.permissions,
               'inherit', entry.inherit_flags)
               ORDER BY entry.principal, entry.effect)
               FILTER (WHERE entry.inherit_flags <> 0), '[]'::jsonb)
      INTO v_total, v_inherited
      FROM _vexfs.acl_sets AS set
      JOIN _vexfs.acl_set_entries AS entry
        ON entry.acl_set_id = set.acl_set_id
     WHERE set.workspace_id = p_workspace_id
       AND set.acl_set_id = v_parent_set
     GROUP BY set.entry_count;
    IF jsonb_array_length(v_inherited) = 0 THEN
        RETURN NULL;
    END IF;
    IF jsonb_array_length(v_inherited) = v_total THEN
        RETURN v_parent_set;
    END IF;
    RETURN _vexfs.get_or_create_acl_set(p_workspace_id, v_inherited);
END;
$$;

CREATE FUNCTION _vexfs.inherit_acl(
    p_workspace_id bigint,
    p_parent_inode bigint,
    p_child_inode bigint)
RETURNS void
LANGUAGE sql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
    UPDATE _vexfs.inodes AS child
       SET acl_set_id = _vexfs.inherited_acl_set_id(
           p_workspace_id, p_parent_inode)
     WHERE child.workspace_id = p_workspace_id
       AND child.inode_id = p_child_inode
$$;

CREATE FUNCTION _vexfs.random_token(p_prefix text)
RETURNS text
LANGUAGE sql
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT p_prefix || '-' || encode(pg_catalog.sha256(convert_to(
        clock_timestamp()::text || ':' || random()::text || ':' ||
        pg_catalog.pg_backend_pid()::text, 'UTF8')), 'hex')
$$;

CREATE FUNCTION _vexfs.request_digest(p_arguments jsonb)
RETURNS text
LANGUAGE sql
IMMUTABLE
STRICT
PARALLEL SAFE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT encode(pg_catalog.sha256(convert_to(p_arguments::text, 'UTF8')), 'hex')
$$;

CREATE FUNCTION _vexfs.lock_request(
    p_workspace_id bigint,
    p_request_id text)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
BEGIN
    IF p_request_id IS NOT NULL AND p_request_id <> '' THEN
        PERFORM pg_catalog.pg_advisory_xact_lock(
            pg_catalog.hashtextextended(
                p_workspace_id::text || ':' || session_user || ':' || p_request_id,
                1448239925));
    END IF;
END;
$$;

CREATE FUNCTION _vexfs.replay_get(
    p_workspace_id bigint,
    p_request_id text,
    p_operation text,
    p_argument_hash text)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_actor oid;
    v_operation text;
    v_hash text;
    v_result text;
BEGIN
    IF p_request_id IS NULL OR p_request_id = '' THEN
        RETURN NULL;
    END IF;
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    SELECT replay.operation, replay.argument_hash, replay.result_text
      INTO v_operation, v_hash, v_result
      FROM _vexfs.request_replays AS replay
     WHERE replay.workspace_id = p_workspace_id
       AND replay.owner_oid = v_actor
       AND replay.request_id = p_request_id;
    IF NOT FOUND THEN
        RETURN NULL;
    END IF;
    IF v_operation IS DISTINCT FROM p_operation OR v_hash IS DISTINCT FROM p_argument_hash THEN
        RAISE EXCEPTION 'VEXFS_REQUEST_CONFLICT: request id was reused with different arguments'
            USING ERRCODE = '40001';
    END IF;
    RETURN v_result;
END;
$$;

CREATE FUNCTION _vexfs.replay_put(
    p_workspace_id bigint,
    p_request_id text,
    p_operation text,
    p_argument_hash text,
    p_result text)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_actor oid;
BEGIN
    IF p_request_id IS NULL OR p_request_id = '' THEN
        RETURN;
    END IF;
    IF octet_length(p_request_id) > 255 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_REQUEST: request id is too long'
            USING ERRCODE = '22023';
    END IF;
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    INSERT INTO _vexfs.request_replays(
        workspace_id, owner_oid, request_id, operation, argument_hash, result_text)
    VALUES (
        p_workspace_id, v_actor, p_request_id, p_operation, p_argument_hash, p_result)
    ON CONFLICT (workspace_id, owner_oid, request_id) DO NOTHING;
    DELETE FROM _vexfs.request_replays AS replay
     WHERE replay.workspace_id = p_workspace_id
       AND replay.created_at < clock_timestamp() - interval '7 days';
END;
$$;

CREATE FUNCTION _vexfs.require_handle(
    p_handle text,
    p_permission text DEFAULT 'read')
RETURNS _vexfs.handles
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle _vexfs.handles%ROWTYPE;
    v_actor oid;
BEGIN
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    SELECT * INTO v_handle
      FROM _vexfs.handles AS handle
     WHERE handle.handle_id = p_handle
       AND handle.owner_oid = v_actor
       AND handle.state IN ('open', 'retained')
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_HANDLE_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    IF v_handle.inode_id IS NOT NULL THEN
        PERFORM _vexfs.require_inode_permission(
            v_handle.workspace_id, v_handle.inode_id, p_permission);
    END IF;
    UPDATE _vexfs.handles
       SET lease_until = clock_timestamp() + interval '30 seconds',
           updated_at = clock_timestamp(),
           state = 'open'
     WHERE handle_id = p_handle;
    v_handle.lease_until := clock_timestamp() + interval '30 seconds';
    v_handle.state := 'open';
    RETURN v_handle;
END;
$$;

CREATE FUNCTION _vexfs.reap_orphan_inodes(p_workspace_id bigint)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_count bigint;
BEGIN
    WITH reaped AS (
        UPDATE _vexfs.inodes AS inode
           SET live = false,
               changed_at = clock_timestamp()
         WHERE inode.workspace_id = p_workspace_id
           AND inode.live
           AND inode.kind <> 'directory'
           AND NOT EXISTS (
               SELECT 1
                 FROM _vexfs.dentries AS link
                WHERE link.workspace_id = inode.workspace_id
                  AND link.inode_id = inode.inode_id)
           AND NOT EXISTS (
               SELECT 1
                 FROM _vexfs.handles AS handle
                WHERE handle.workspace_id = inode.workspace_id
                  AND handle.inode_id = inode.inode_id
                  AND handle.state IN ('open', 'retained'))
        RETURNING 1)
    SELECT count(*) INTO v_count FROM reaped;
    RETURN v_count;
END;
$$;

CREATE FUNCTION _vexfs.overlay_bytes(
    p_base bytea,
    p_offset bigint,
    p_value bytea)
RETURNS bytea
LANGUAGE plpgsql
IMMUTABLE
STRICT
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_size bigint;
    v_end bigint;
    v_prefix bytea;
    v_padding bytea := ''::bytea;
    v_suffix bytea := ''::bytea;
BEGIN
    IF p_offset < 0 OR p_offset > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: offset is outside 0..134217728'
            USING ERRCODE = '22023';
    END IF;
    v_size := octet_length(p_base);
    v_end := p_offset + octet_length(p_value);
    IF v_end > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_FILE_TOO_LARGE: staged file exceeds 128 MiB'
            USING ERRCODE = '54000';
    END IF;
    v_prefix := substring(p_base FROM 1 FOR least(p_offset, v_size)::integer);
    IF p_offset > v_size THEN
        v_padding := decode(repeat('00', (p_offset - v_size)::integer), 'hex');
    END IF;
    IF v_end < v_size THEN
        v_suffix := substring(p_base FROM (v_end + 1)::integer);
    END IF;
    RETURN v_prefix || v_padding || p_value || v_suffix;
END;
$$;

CREATE FUNCTION _vexfs.read_staging_chunk(
    p_handle text,
    p_chunk_no integer,
    p_target_size integer)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_staging _vexfs.handle_staging%ROWTYPE;
    v_workspace_id bigint;
    v_chunk_start bigint;
    v_visible_size integer;
    v_content bytea;
    v_size integer;
    v_checksum text;
    v_chunk_workspace bigint;
BEGIN
    IF p_chunk_no < 0 OR p_target_size < 0 OR p_target_size > 65536 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: invalid staging chunk range'
            USING ERRCODE = '22023';
    END IF;
    IF p_target_size = 0 THEN
        RETURN ''::bytea;
    END IF;
    SELECT * INTO v_staging
      FROM _vexfs.handle_staging AS staging
     WHERE staging.handle_id = p_handle;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_STAGING_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    SELECT handle.workspace_id
      INTO STRICT v_workspace_id
      FROM _vexfs.handles AS handle
     WHERE handle.handle_id = p_handle;

    SELECT dirty.content INTO v_content
      FROM _vexfs.handle_staging_chunks AS dirty
     WHERE dirty.handle_id = p_handle
       AND dirty.chunk_no = p_chunk_no;
    IF NOT FOUND THEN
        v_content := ''::bytea;
        v_chunk_start := p_chunk_no::bigint * 65536;
        IF v_staging.base_manifest_id IS NOT NULL
           AND v_chunk_start < v_staging.base_visible_size THEN
            SELECT chunk.content,
                   chunk.size_bytes,
                   chunk.checksum,
                   chunk.workspace_id
              INTO v_content,
                   v_size,
                   v_checksum,
                   v_chunk_workspace
              FROM _vexfs.manifest_chunks AS entry
              JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
             WHERE entry.manifest_id = v_staging.base_manifest_id
               AND entry.chunk_no = p_chunk_no;
            IF NOT FOUND
               OR v_chunk_workspace <> v_workspace_id
               OR v_size <> octet_length(v_content)
               OR v_checksum <> encode(pg_catalog.sha256(v_content), 'hex') THEN
                RAISE EXCEPTION 'VEXFS_CHECKSUM_MISMATCH: staging base chunk is invalid'
                    USING ERRCODE = 'XX001';
            END IF;
            v_visible_size := least(
                v_size,
                v_staging.base_visible_size - v_chunk_start)::integer;
            v_content := substring(v_content FROM 1 FOR v_visible_size);
        END IF;
    END IF;

    IF octet_length(v_content) > p_target_size THEN
        v_content := substring(v_content FROM 1 FOR p_target_size);
    ELSIF octet_length(v_content) < p_target_size THEN
        v_content := v_content || decode(
            repeat('00', p_target_size - octet_length(v_content)), 'hex');
    END IF;
    RETURN v_content;
END;
$$;

CREATE FUNCTION _vexfs.read_staging_range(
    p_handle text,
    p_offset bigint,
    p_length bigint)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_staging _vexfs.handle_staging%ROWTYPE;
    v_end bigint;
    v_first_chunk integer;
    v_last_chunk integer;
    v_chunk_no integer;
    v_chunk_start bigint;
    v_chunk_size integer;
    v_copy_start integer;
    v_copy_length integer;
    v_chunk bytea;
    v_content bytea := ''::bytea;
BEGIN
    IF p_offset < 0 OR p_length < 0
       OR p_offset > 134217728 OR p_length > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: offset and length must be within 0..128 MiB'
            USING ERRCODE = '22023';
    END IF;
    SELECT * INTO v_staging
      FROM _vexfs.handle_staging AS staging
     WHERE staging.handle_id = p_handle;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_STAGING_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    IF p_length = 0 OR p_offset >= v_staging.logical_size THEN
        RETURN ''::bytea;
    END IF;

    v_end := least(v_staging.logical_size, p_offset + p_length);
    v_first_chunk := (p_offset / 65536)::integer;
    v_last_chunk := ((v_end - 1) / 65536)::integer;
    FOR v_chunk_no IN v_first_chunk..v_last_chunk LOOP
        v_chunk_start := v_chunk_no::bigint * 65536;
        v_chunk_size := least(
            65536, v_staging.logical_size - v_chunk_start)::integer;
        v_chunk := _vexfs.read_staging_chunk(
            p_handle, v_chunk_no, v_chunk_size);
        v_copy_start := (greatest(p_offset, v_chunk_start) - v_chunk_start)::integer;
        v_copy_length := (
            least(v_end, v_chunk_start + v_chunk_size)
            - greatest(p_offset, v_chunk_start))::integer;
        v_content := v_content || substring(
            v_chunk FROM v_copy_start + 1 FOR v_copy_length);
    END LOOP;
    RETURN v_content;
END;
$$;

CREATE FUNCTION _vexfs.read_staging_content(p_handle text)
RETURNS bytea
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT _vexfs.read_staging_range(
        p_handle, 0,
        (SELECT staging.logical_size
           FROM _vexfs.handle_staging AS staging
          WHERE staging.handle_id = p_handle))
$$;

CREATE FUNCTION _vexfs.resolve_version_storage(
    p_workspace_id bigint,
    p_inode_id bigint,
    p_version bigint)
RETURNS TABLE(
    manifest_id bigint,
    size_bytes bigint,
    checksum text,
    canonical_version bigint,
    chunk_count integer)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_version_manifest bigint;
    v_source_version bigint;
    v_source_manifest bigint;
    v_resolved_manifest bigint;
    v_source_size bigint;
    v_source_checksum text;
    v_manifest_workspace bigint;
    v_manifest_size bigint;
    v_manifest_chunk_size integer;
    v_manifest_checksum text;
BEGIN
    SELECT v.manifest_id,
           v.source_version_no,
           v.size_bytes,
           v.checksum,
           source.manifest_id,
           source.size_bytes,
           source.checksum,
           source.version_no
      INTO v_version_manifest,
           v_source_version,
           size_bytes,
           checksum,
           v_source_manifest,
           v_source_size,
           v_source_checksum,
           canonical_version
      FROM _vexfs.file_versions AS v
      LEFT JOIN _vexfs.file_versions AS source
        ON source.workspace_id = v.workspace_id
       AND source.inode_id = v.inode_id
       AND source.version_no = v.source_version_no
       AND source.source_version_no IS NULL
     WHERE v.workspace_id = p_workspace_id
       AND v.inode_id = p_inode_id
       AND v.version_no = p_version;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_VERSION_NOT_FOUND: %', p_version
            USING ERRCODE = 'P0002';
    END IF;

    IF v_source_version IS NULL THEN
        v_resolved_manifest := v_version_manifest;
        canonical_version := p_version;
    ELSE
        v_resolved_manifest := v_source_manifest;
        IF canonical_version IS NULL
           OR v_source_size IS DISTINCT FROM size_bytes
           OR v_source_checksum IS DISTINCT FROM checksum THEN
            RAISE EXCEPTION 'VEXFS_CORRUPT: version alias metadata is invalid'
                USING ERRCODE = 'XX001';
        END IF;
    END IF;
    IF v_resolved_manifest IS NULL THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: version manifest is missing'
            USING ERRCODE = 'XX001';
    END IF;

    SELECT m.workspace_id,
           m.file_size,
           m.chunk_size,
           m.chunk_count,
           m.checksum
      INTO v_manifest_workspace,
           v_manifest_size,
           v_manifest_chunk_size,
           chunk_count,
           v_manifest_checksum
      FROM _vexfs.manifests AS m
     WHERE m.manifest_id = v_resolved_manifest;
    IF NOT FOUND
       OR v_manifest_workspace <> p_workspace_id
       OR v_manifest_size <> size_bytes
       OR v_manifest_chunk_size <> 65536
       OR chunk_count <> ((size_bytes + 65535) / 65536)::integer
       OR v_manifest_checksum IS DISTINCT FROM checksum THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: version manifest metadata is invalid'
            USING ERRCODE = 'XX001';
    END IF;
    manifest_id := v_resolved_manifest;
    RETURN NEXT;
END;
$$;

-- A manifest checksum identifies the ordered chunk tree, not a second copy of
-- the complete file checksum.  Chunk checksums remain SHA-256 over their exact
-- 64 KiB payloads.  Publishing a small range can therefore reuse old chunk
-- hashes and keep memory proportional to manifest metadata instead of file
-- size.  The fixed format and version prefix make the root deterministic.
CREATE FUNCTION _vexfs.compute_manifest_checksum(p_manifest bigint)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_file_size bigint;
    v_chunk_size integer;
    v_chunk_count integer;
    v_entry_count bigint;
    v_total_bytes bigint;
    v_entries text;
    v_valid boolean;
BEGIN
    SELECT manifest.file_size, manifest.chunk_size, manifest.chunk_count
      INTO v_file_size, v_chunk_size, v_chunk_count
      FROM _vexfs.manifests AS manifest
     WHERE manifest.manifest_id = p_manifest;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_MANIFEST_NOT_FOUND: %', p_manifest
            USING ERRCODE = 'P0002';
    END IF;

    SELECT count(*),
           coalesce(sum(chunk.size_bytes), 0),
           coalesce(bool_and(
               entry.chunk_no >= 0
               AND entry.chunk_no < v_chunk_count
               AND chunk.size_bytes = least(
                   v_chunk_size,
                   v_file_size - entry.chunk_no::bigint * v_chunk_size)::integer),
               v_chunk_count = 0),
           coalesce(pg_catalog.string_agg(
               pg_catalog.format(
                   '%s:%s:%s',
                   entry.chunk_no, chunk.size_bytes, chunk.checksum),
               E'\n' ORDER BY entry.chunk_no), '')
      INTO v_entry_count, v_total_bytes, v_valid, v_entries
      FROM _vexfs.manifest_chunks AS entry
      JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
     WHERE entry.manifest_id = p_manifest;
    IF v_entry_count <> v_chunk_count
       OR v_total_bytes <> v_file_size
       OR NOT v_valid THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: manifest chunk metadata is invalid'
            USING ERRCODE = 'XX001';
    END IF;

    RETURN encode(pg_catalog.sha256(pg_catalog.convert_to(
        pg_catalog.format(
            'vexfs-manifest-v1:%s:%s:%s',
            v_chunk_size, v_file_size, v_chunk_count)
        || E'\n' || v_entries,
        'UTF8')), 'hex');
END;
$$;

CREATE FUNCTION _vexfs.store_manifest(
    p_workspace_id bigint,
    p_inode_id bigint,
    p_previous_manifest bigint,
    p_content bytea)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_manifest bigint;
    v_file_size bigint;
    v_chunk_count integer;
    v_chunk_no integer := 0;
    v_offset integer := 0;
    v_chunk bytea;
    v_chunk_size integer;
    v_chunk_checksum text;
    v_chunk_id bigint;
    v_empty_checksum text;
BEGIN
    IF p_content IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CONTENT: content cannot be null'
            USING ERRCODE = '22004';
    END IF;
    v_file_size := octet_length(p_content);
    IF v_file_size > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_FILE_TOO_LARGE: maximum file size is 128 MiB'
            USING ERRCODE = '54000';
    END IF;
    v_chunk_count := ((v_file_size + 65535) / 65536)::integer;

    IF v_file_size = 0 THEN
        v_empty_checksum := encode(pg_catalog.sha256(pg_catalog.convert_to(
            pg_catalog.format('vexfs-manifest-v1:%s:%s:%s', 65536, 0, 0)
            || E'\n', 'UTF8')), 'hex');
        INSERT INTO _vexfs.manifests(
            workspace_id, file_size, chunk_size, chunk_count, checksum)
        VALUES (p_workspace_id, 0, 65536, 0, v_empty_checksum)
        ON CONFLICT (workspace_id)
            WHERE file_size = 0 AND chunk_count = 0
            DO NOTHING
        RETURNING manifest_id INTO v_manifest;
        IF v_manifest IS NULL THEN
            SELECT manifest.manifest_id, manifest.checksum
              INTO STRICT v_manifest, v_chunk_checksum
              FROM _vexfs.manifests AS manifest
             WHERE manifest.workspace_id = p_workspace_id
               AND manifest.file_size = 0
               AND manifest.chunk_count = 0;
            IF v_chunk_checksum IS DISTINCT FROM v_empty_checksum THEN
                RAISE EXCEPTION 'VEXFS_CHECKSUM_MISMATCH: canonical empty manifest is invalid'
                    USING ERRCODE = 'XX001';
            END IF;
        END IF;
        RETURN v_manifest;
    END IF;

    INSERT INTO _vexfs.manifests(
        workspace_id, file_size, chunk_size, chunk_count, checksum)
    VALUES (
        p_workspace_id, v_file_size, 65536, v_chunk_count, repeat('0', 64))
    RETURNING manifest_id INTO v_manifest;

    WHILE v_offset < v_file_size LOOP
        v_chunk_size := least(65536, v_file_size - v_offset)::integer;
        v_chunk := substring(p_content FROM v_offset + 1 FOR v_chunk_size);
        v_chunk_checksum := encode(pg_catalog.sha256(v_chunk), 'hex');
        v_chunk_id := NULL;

        IF p_previous_manifest IS NOT NULL THEN
            SELECT c.chunk_id INTO v_chunk_id
              FROM _vexfs.manifest_chunks AS entry
              JOIN _vexfs.manifests AS previous
                ON previous.manifest_id = entry.manifest_id
              JOIN _vexfs.chunks AS c
                ON c.chunk_id = entry.chunk_id
             WHERE entry.manifest_id = p_previous_manifest
               AND entry.chunk_no = v_chunk_no
               AND previous.workspace_id = p_workspace_id
               AND c.workspace_id = p_workspace_id
               AND c.size_bytes = v_chunk_size
               AND c.checksum = v_chunk_checksum
               AND c.content = v_chunk;
        END IF;

        IF v_chunk_id IS NULL THEN
            SELECT c.chunk_id INTO v_chunk_id
              FROM _vexfs.manifest_chunks AS entry
              JOIN _vexfs.chunks AS c ON c.chunk_id = entry.chunk_id
             WHERE entry.manifest_id = v_manifest
               AND c.workspace_id = p_workspace_id
               AND c.size_bytes = v_chunk_size
               AND c.checksum = v_chunk_checksum
               AND c.content = v_chunk
             LIMIT 1;
        END IF;

        IF v_chunk_id IS NULL THEN
            INSERT INTO _vexfs.chunks(
                workspace_id, content, size_bytes, checksum)
            VALUES (
                p_workspace_id, v_chunk, v_chunk_size, v_chunk_checksum)
            RETURNING chunk_id INTO v_chunk_id;
        END IF;

        INSERT INTO _vexfs.manifest_chunks(manifest_id, chunk_no, chunk_id)
        VALUES (v_manifest, v_chunk_no, v_chunk_id);
        v_chunk_no := v_chunk_no + 1;
        v_offset := v_offset + v_chunk_size;
    END LOOP;
    UPDATE _vexfs.manifests
       SET checksum = _vexfs.compute_manifest_checksum(v_manifest)
     WHERE manifest_id = v_manifest;
    RETURN v_manifest;
END;
$$;

-- Build a copy-on-write manifest for a positional write. Untouched chunks are
-- linked into the new manifest in one statement; only overlapping chunks (and
-- zero-filled hole chunks) are materialized again. The manifest root is built
-- from ordered chunk hashes, so this path never aggregates the complete file.
CREATE FUNCTION _vexfs.store_manifest_range(
    p_workspace_id bigint,
    p_inode_id bigint,
    p_previous_manifest bigint,
    p_previous_size bigint,
    p_offset bigint,
    p_content bytea)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_manifest bigint;
    v_previous_chunk_count integer;
    v_previous_file_size bigint;
    v_new_size bigint;
    v_new_chunk_count integer;
    v_write_end bigint;
    v_chunk_no integer;
    v_chunk_start bigint;
    v_chunk_end bigint;
    v_chunk_size integer;
    v_chunk bytea;
    v_previous_chunk_id bigint;
    v_chunk_id bigint;
    v_chunk_checksum text;
    v_overlap_start bigint;
    v_overlap_end bigint;
    v_existing_count bigint;
    v_existing_bytes bigint;
    v_existing_valid boolean;
    v_final_count bigint;
    v_final_bytes bigint;
BEGIN
    IF p_content IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CONTENT: content cannot be null'
            USING ERRCODE = '22004';
    END IF;
    IF p_previous_manifest IS NULL OR p_previous_size < 0
       OR p_offset < 0 OR p_offset > 134217728
       OR p_offset + octet_length(p_content) > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: range manifest input is invalid'
            USING ERRCODE = '22023';
    END IF;

    SELECT manifest.file_size, manifest.chunk_count
      INTO v_previous_file_size, v_previous_chunk_count
     FROM _vexfs.manifests AS manifest
     WHERE manifest.manifest_id = p_previous_manifest
       AND manifest.workspace_id = p_workspace_id;
    IF NOT FOUND OR v_previous_file_size <> p_previous_size THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: previous range manifest is invalid'
            USING ERRCODE = 'XX001';
    END IF;

    -- Validate source metadata without re-reading every unchanged chunk. Reads
    -- and vexfs_check still validate each chunk payload against its SHA-256.
    SELECT count(*),
           coalesce(sum(chunk.size_bytes), 0),
           coalesce(bool_and(
               entry.chunk_no >= 0
               AND entry.chunk_no < v_previous_chunk_count
               AND chunk.workspace_id = p_workspace_id
               AND chunk.size_bytes = least(
                   65536,
                   p_previous_size - entry.chunk_no::bigint * 65536)::integer
               AND octet_length(chunk.content) = chunk.size_bytes),
               v_previous_chunk_count = 0)
      INTO v_existing_count, v_existing_bytes, v_existing_valid
      FROM _vexfs.manifest_chunks AS entry
      JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
     WHERE entry.manifest_id = p_previous_manifest;
    IF v_existing_count <> v_previous_chunk_count
       OR v_existing_bytes <> p_previous_size
       OR NOT v_existing_valid THEN
        RAISE EXCEPTION 'VEXFS_CHECKSUM_MISMATCH: previous manifest chunk is invalid'
            USING ERRCODE = 'XX001';
    END IF;

    v_write_end := p_offset + octet_length(p_content);
    v_new_size := greatest(p_previous_size, v_write_end);
    IF v_new_size = 0 THEN
        RETURN _vexfs.store_manifest(
            p_workspace_id, p_inode_id, p_previous_manifest, ''::bytea);
    END IF;
    v_new_chunk_count := ((v_new_size + 65535) / 65536)::integer;
    INSERT INTO _vexfs.manifests(
        workspace_id, file_size, chunk_size, chunk_count, checksum)
    VALUES (
        p_workspace_id, v_new_size, 65536, v_new_chunk_count, repeat('0', 64))
    RETURNING manifest_id INTO v_manifest;

    -- Copy chunks whose byte range and final size are unchanged. This is the
    -- common path for NFS writes and avoids one SQL round trip per old chunk.
    INSERT INTO _vexfs.manifest_chunks(manifest_id, chunk_no, chunk_id)
    SELECT v_manifest, entry.chunk_no, entry.chunk_id
      FROM _vexfs.manifest_chunks AS entry
      JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
     WHERE entry.manifest_id = p_previous_manifest
       AND entry.chunk_no < v_new_chunk_count
       AND chunk.size_bytes = least(
           65536,
           v_new_size - entry.chunk_no::bigint * 65536)::integer
       AND (entry.chunk_no::bigint * 65536 + chunk.size_bytes <= p_offset
            OR entry.chunk_no::bigint * 65536 >= v_write_end);

    FOR v_chunk_no IN 0..v_new_chunk_count - 1 LOOP
        IF EXISTS (
            SELECT 1 FROM _vexfs.manifest_chunks AS entry
             WHERE entry.manifest_id = v_manifest
               AND entry.chunk_no = v_chunk_no) THEN
            CONTINUE;
        END IF;

        v_chunk_start := v_chunk_no::bigint * 65536;
        v_chunk_end := least(v_new_size, v_chunk_start + 65536);
        v_chunk_size := (v_chunk_end - v_chunk_start)::integer;
        v_previous_chunk_id := NULL;
        v_chunk := ''::bytea;
        SELECT chunk.chunk_id, chunk.content
          INTO v_previous_chunk_id, v_chunk
          FROM _vexfs.manifest_chunks AS entry
          JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
         WHERE entry.manifest_id = p_previous_manifest
           AND entry.chunk_no = v_chunk_no;
        IF NOT FOUND THEN
            v_chunk := ''::bytea;
        END IF;
        IF v_previous_chunk_id IS NOT NULL AND NOT EXISTS (
            SELECT 1
              FROM _vexfs.chunks AS source
             WHERE source.chunk_id = v_previous_chunk_id
               AND source.checksum = encode(
                   pg_catalog.sha256(source.content), 'hex')) THEN
            RAISE EXCEPTION 'VEXFS_CHECKSUM_MISMATCH: previous range chunk is invalid'
                USING ERRCODE = 'XX001';
        END IF;
        IF octet_length(v_chunk) > v_chunk_size THEN
            v_chunk := substring(v_chunk FROM 1 FOR v_chunk_size);
        ELSIF octet_length(v_chunk) < v_chunk_size THEN
            v_chunk := v_chunk || decode(
                repeat('00', v_chunk_size - octet_length(v_chunk)), 'hex');
        END IF;

        v_overlap_start := greatest(v_chunk_start, p_offset);
        v_overlap_end := least(v_chunk_end, v_write_end);
        IF v_overlap_end > v_overlap_start THEN
            v_chunk := _vexfs.overlay_bytes(
                v_chunk,
                v_overlap_start - v_chunk_start,
                substring(
                    p_content
                    FROM (v_overlap_start - p_offset + 1)::integer
                    FOR (v_overlap_end - v_overlap_start)::integer));
        END IF;
        IF octet_length(v_chunk) <> v_chunk_size THEN
            RAISE EXCEPTION 'VEXFS_CORRUPT: range chunk has invalid size'
                USING ERRCODE = 'XX001';
        END IF;

        v_chunk_checksum := encode(pg_catalog.sha256(v_chunk), 'hex');
        v_chunk_id := NULL;
        IF v_previous_chunk_id IS NOT NULL THEN
            SELECT chunk.chunk_id INTO v_chunk_id
              FROM _vexfs.chunks AS chunk
             WHERE chunk.chunk_id = v_previous_chunk_id
               AND chunk.size_bytes = v_chunk_size
               AND chunk.checksum = v_chunk_checksum
               AND chunk.content = v_chunk;
        END IF;
        IF v_chunk_id IS NULL THEN
            SELECT chunk.chunk_id INTO v_chunk_id
              FROM _vexfs.manifest_chunks AS entry
              JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
             WHERE entry.manifest_id = v_manifest
               AND chunk.size_bytes = v_chunk_size
               AND chunk.checksum = v_chunk_checksum
               AND chunk.content = v_chunk
             LIMIT 1;
        END IF;
        IF v_chunk_id IS NULL THEN
            INSERT INTO _vexfs.chunks(
                workspace_id, content, size_bytes, checksum)
            VALUES (
                p_workspace_id, v_chunk, v_chunk_size, v_chunk_checksum)
            RETURNING chunk_id INTO v_chunk_id;
        END IF;
        INSERT INTO _vexfs.manifest_chunks(manifest_id, chunk_no, chunk_id)
        VALUES (v_manifest, v_chunk_no, v_chunk_id);
    END LOOP;

    SELECT count(*),
           coalesce(sum(chunk.size_bytes), 0)
      INTO v_final_count, v_final_bytes
      FROM _vexfs.manifest_chunks AS entry
      JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
     WHERE entry.manifest_id = v_manifest;
    IF v_final_count <> v_new_chunk_count OR v_final_bytes <> v_new_size THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: range manifest is incomplete'
            USING ERRCODE = 'XX001';
    END IF;
    UPDATE _vexfs.manifests
       SET checksum = _vexfs.compute_manifest_checksum(v_manifest)
     WHERE manifest_id = v_manifest;
    RETURN v_manifest;
END;
$$;

-- Publish a writable handle directly from its base manifest plus dirty chunks.
-- Fully visible, unchanged chunks are linked in one statement. Only dirty,
-- truncated, extended, or zero-filled edge chunks are materialized.
CREATE FUNCTION _vexfs.store_staging_manifest(
    p_handle text,
    p_inode_id bigint)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace_id bigint;
    v_handle_inode bigint;
    v_staging _vexfs.handle_staging%ROWTYPE;
    v_base_workspace bigint;
    v_base_size bigint;
    v_manifest bigint;
    v_chunk_count integer;
    v_chunk_no integer;
    v_chunk_size integer;
    v_chunk bytea;
    v_chunk_checksum text;
    v_chunk_id bigint;
    v_base_chunk_id bigint;
    v_final_count bigint;
    v_final_bytes bigint;
BEGIN
    IF p_inode_id IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_INODE: staging manifest requires an inode'
            USING ERRCODE = '22023';
    END IF;
    SELECT handle.workspace_id, handle.inode_id
      INTO v_workspace_id, v_handle_inode
      FROM _vexfs.handles AS handle
     WHERE handle.handle_id = p_handle;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_STAGING_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    SELECT * INTO v_staging
      FROM _vexfs.handle_staging AS staging
     WHERE staging.handle_id = p_handle;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_STAGING_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    IF v_handle_inode IS NOT NULL AND v_handle_inode <> p_inode_id THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: handle inode changed during publish'
            USING ERRCODE = 'XX001';
    END IF;
    IF v_staging.logical_size < 0 OR v_staging.logical_size > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_FILE_TOO_LARGE: maximum file size is 128 MiB'
            USING ERRCODE = '54000';
    END IF;
    IF v_staging.base_manifest_id IS NULL THEN
        IF v_staging.base_size <> 0 OR v_staging.base_visible_size <> 0 THEN
            RAISE EXCEPTION 'VEXFS_CORRUPT: staging base metadata is invalid'
                USING ERRCODE = 'XX001';
        END IF;
    ELSE
        SELECT base.workspace_id, base.file_size
          INTO v_base_workspace, v_base_size
          FROM _vexfs.manifests AS base
         WHERE base.manifest_id = v_staging.base_manifest_id;
        IF NOT FOUND
           OR v_base_workspace <> v_workspace_id
           OR v_base_size <> v_staging.base_size
           OR v_staging.base_visible_size > v_base_size THEN
            RAISE EXCEPTION 'VEXFS_CORRUPT: staging base manifest is invalid'
                USING ERRCODE = 'XX001';
        END IF;
    END IF;

    v_chunk_count := ((v_staging.logical_size + 65535) / 65536)::integer;
    IF v_staging.logical_size = 0 THEN
        RETURN _vexfs.store_manifest(
            v_workspace_id, p_inode_id, v_staging.base_manifest_id, ''::bytea);
    END IF;
    INSERT INTO _vexfs.manifests(
        workspace_id, file_size, chunk_size, chunk_count, checksum)
    VALUES (
        v_workspace_id, v_staging.logical_size, 65536,
        v_chunk_count, repeat('0', 64))
    RETURNING manifest_id INTO v_manifest;

    -- Reuse unchanged chunks without loading their payload. A partially visible
    -- base chunk is excluded so truncate-then-grow correctly inserts zeroes.
    IF v_staging.base_manifest_id IS NOT NULL AND v_chunk_count > 0 THEN
        INSERT INTO _vexfs.manifest_chunks(manifest_id, chunk_no, chunk_id)
        SELECT v_manifest, entry.chunk_no, entry.chunk_id
          FROM _vexfs.manifest_chunks AS entry
          JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
         WHERE entry.manifest_id = v_staging.base_manifest_id
           AND entry.chunk_no < v_chunk_count
           AND NOT EXISTS (
               SELECT 1
                 FROM _vexfs.handle_staging_chunks AS dirty
                WHERE dirty.handle_id = p_handle
                  AND dirty.chunk_no = entry.chunk_no)
           AND chunk.workspace_id = v_workspace_id
           AND chunk.size_bytes = least(
               65536,
               v_staging.logical_size
                   - entry.chunk_no::bigint * 65536)::integer
           AND entry.chunk_no::bigint * 65536 + chunk.size_bytes
               <= v_staging.base_visible_size;
    END IF;

    IF v_chunk_count > 0 THEN
        FOR v_chunk_no IN 0..v_chunk_count - 1 LOOP
            IF EXISTS (
                SELECT 1
                  FROM _vexfs.manifest_chunks AS entry
                 WHERE entry.manifest_id = v_manifest
                   AND entry.chunk_no = v_chunk_no) THEN
                CONTINUE;
            END IF;
            v_chunk_size := least(
                65536,
                v_staging.logical_size - v_chunk_no::bigint * 65536)::integer;
            v_chunk := _vexfs.read_staging_chunk(
                p_handle, v_chunk_no, v_chunk_size);
            IF octet_length(v_chunk) <> v_chunk_size THEN
                RAISE EXCEPTION 'VEXFS_CORRUPT: staging chunk has invalid size'
                    USING ERRCODE = 'XX001';
            END IF;
            v_chunk_checksum := encode(pg_catalog.sha256(v_chunk), 'hex');
            v_chunk_id := NULL;
            v_base_chunk_id := NULL;
            IF v_staging.base_manifest_id IS NOT NULL THEN
                SELECT entry.chunk_id INTO v_base_chunk_id
                  FROM _vexfs.manifest_chunks AS entry
                 WHERE entry.manifest_id = v_staging.base_manifest_id
                   AND entry.chunk_no = v_chunk_no;
            END IF;
            IF v_base_chunk_id IS NOT NULL THEN
                SELECT chunk.chunk_id INTO v_chunk_id
                 FROM _vexfs.chunks AS chunk
                 WHERE chunk.chunk_id = v_base_chunk_id
                   AND chunk.workspace_id = v_workspace_id
                   AND chunk.size_bytes = v_chunk_size
                   AND chunk.checksum = v_chunk_checksum
                   AND chunk.content = v_chunk;
            END IF;
            IF v_chunk_id IS NULL THEN
                SELECT chunk.chunk_id INTO v_chunk_id
                  FROM _vexfs.manifest_chunks AS entry
                  JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
                 WHERE entry.manifest_id = v_manifest
                   AND chunk.size_bytes = v_chunk_size
                   AND chunk.checksum = v_chunk_checksum
                   AND chunk.content = v_chunk
                 LIMIT 1;
            END IF;
            IF v_chunk_id IS NULL THEN
                INSERT INTO _vexfs.chunks(
                    workspace_id, content, size_bytes, checksum)
                VALUES (
                    v_workspace_id, v_chunk, v_chunk_size, v_chunk_checksum)
                RETURNING chunk_id INTO v_chunk_id;
            END IF;
            INSERT INTO _vexfs.manifest_chunks(manifest_id, chunk_no, chunk_id)
            VALUES (v_manifest, v_chunk_no, v_chunk_id);
        END LOOP;
    END IF;

    SELECT count(*), coalesce(sum(chunk.size_bytes), 0)
      INTO v_final_count, v_final_bytes
      FROM _vexfs.manifest_chunks AS entry
      JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
     WHERE entry.manifest_id = v_manifest;
    IF v_final_count <> v_chunk_count
       OR v_final_bytes <> v_staging.logical_size THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: staging manifest is incomplete'
            USING ERRCODE = 'XX001';
    END IF;
    UPDATE _vexfs.manifests
       SET checksum = _vexfs.compute_manifest_checksum(v_manifest)
     WHERE manifest_id = v_manifest;
    RETURN v_manifest;
END;
$$;

CREATE FUNCTION _vexfs.read_version_content(
    p_workspace_id bigint,
    p_inode_id bigint,
    p_version bigint)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_manifest bigint;
    v_size bigint;
    v_checksum text;
    v_canonical bigint;
    v_chunk_count integer;
    v_content bytea := ''::bytea;
    v_expected_chunk integer := 0;
    v_offset bigint := 0;
    v_expected_size integer;
    v_row record;
BEGIN
    SELECT storage.manifest_id,
           storage.size_bytes,
           storage.checksum,
           storage.canonical_version,
           storage.chunk_count
      INTO v_manifest,
           v_size,
           v_checksum,
           v_canonical,
           v_chunk_count
      FROM _vexfs.resolve_version_storage(
          p_workspace_id, p_inode_id, p_version) AS storage;

    IF v_size > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: file version exceeds 128 MiB'
            USING ERRCODE = 'XX001';
    END IF;
    FOR v_row IN
        SELECT entry.chunk_no,
               c.content,
               c.size_bytes,
               c.checksum,
               c.workspace_id
          FROM _vexfs.manifest_chunks AS entry
          JOIN _vexfs.chunks AS c ON c.chunk_id = entry.chunk_id
         WHERE entry.manifest_id = v_manifest
         ORDER BY entry.chunk_no
    LOOP
        v_expected_size := least(65536, v_size - v_offset)::integer;
        IF v_row.chunk_no <> v_expected_chunk
           OR v_expected_size <= 0
           OR v_row.size_bytes <> v_expected_size
           OR octet_length(v_row.content) <> v_expected_size
           OR v_row.workspace_id <> p_workspace_id
           OR v_row.checksum <> encode(pg_catalog.sha256(v_row.content), 'hex') THEN
            RAISE EXCEPTION 'VEXFS_CHECKSUM_MISMATCH: file manifest chunk is invalid'
                USING ERRCODE = 'XX001';
        END IF;
        v_content := v_content || v_row.content;
        v_offset := v_offset + v_row.size_bytes;
        v_expected_chunk := v_expected_chunk + 1;
    END LOOP;

    IF v_offset <> v_size
       OR v_expected_chunk <> v_chunk_count
       OR _vexfs.compute_manifest_checksum(v_manifest) <> v_checksum THEN
        RAISE EXCEPTION 'VEXFS_CHECKSUM_MISMATCH: file content does not match manifest'
            USING ERRCODE = 'XX001';
    END IF;
    RETURN v_content;
END;
$$;

CREATE FUNCTION _vexfs.read_version_range(
    p_workspace_id bigint,
    p_inode_id bigint,
    p_version bigint,
    p_offset bigint,
    p_length bigint)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_manifest bigint;
    v_size bigint;
    v_first_chunk integer;
    v_last_chunk integer;
    v_end bigint;
    v_expected_chunk integer;
    v_chunk_offset bigint;
    v_expected_size integer;
    v_copy_start integer;
    v_copy_length integer;
    v_content bytea := ''::bytea;
    v_row record;
BEGIN
    IF p_offset < 0 OR p_length < 0
       OR p_offset > 134217728 OR p_length > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: offset and length must be within 0..128 MiB'
            USING ERRCODE = '22023';
    END IF;
    SELECT storage.manifest_id, storage.size_bytes
      INTO v_manifest, v_size
      FROM _vexfs.resolve_version_storage(
          p_workspace_id, p_inode_id, p_version) AS storage;
    IF v_size > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: file version exceeds 128 MiB'
            USING ERRCODE = 'XX001';
    END IF;
    IF p_length = 0 OR p_offset >= v_size THEN
        RETURN ''::bytea;
    END IF;

    v_end := least(v_size, p_offset + p_length);
    v_first_chunk := (p_offset / 65536)::integer;
    v_last_chunk := ((v_end - 1) / 65536)::integer;
    v_expected_chunk := v_first_chunk;
    FOR v_row IN
        SELECT entry.chunk_no,
               chunk.content,
               chunk.size_bytes,
               chunk.checksum,
               chunk.workspace_id
          FROM _vexfs.manifest_chunks AS entry
          JOIN _vexfs.chunks AS chunk ON chunk.chunk_id = entry.chunk_id
         WHERE entry.manifest_id = v_manifest
           AND entry.chunk_no BETWEEN v_first_chunk AND v_last_chunk
         ORDER BY entry.chunk_no
    LOOP
        v_chunk_offset := v_row.chunk_no::bigint * 65536;
        v_expected_size := least(65536, v_size - v_chunk_offset)::integer;
        IF v_row.chunk_no <> v_expected_chunk
           OR v_expected_size <= 0
           OR v_row.size_bytes <> v_expected_size
           OR octet_length(v_row.content) <> v_expected_size
           OR v_row.workspace_id <> p_workspace_id
           OR v_row.checksum <> encode(pg_catalog.sha256(v_row.content), 'hex') THEN
            RAISE EXCEPTION 'VEXFS_CHECKSUM_MISMATCH: file manifest range chunk is invalid'
                USING ERRCODE = 'XX001';
        END IF;
        v_copy_start := (greatest(p_offset, v_chunk_offset) - v_chunk_offset)::integer;
        v_copy_length := (least(v_end, v_chunk_offset + v_expected_size) -
                          greatest(p_offset, v_chunk_offset))::integer;
        v_content := v_content || substring(
            v_row.content FROM v_copy_start + 1 FOR v_copy_length);
        v_expected_chunk := v_expected_chunk + 1;
    END LOOP;
    IF v_expected_chunk <> v_last_chunk + 1
       OR octet_length(v_content) <> v_end - p_offset THEN
        RAISE EXCEPTION 'VEXFS_CHECKSUM_MISMATCH: file manifest range is incomplete'
            USING ERRCODE = 'XX001';
    END IF;
    RETURN v_content;
END;
$$;

CREATE FUNCTION _vexfs.enforce_quota(
    p_workspace_id bigint,
    p_inode_id bigint,
    p_new_size bigint)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_max_bytes bigint;
    v_max_files bigint;
    v_max_file_bytes bigint;
    v_live_bytes bigint;
    v_live_files bigint;
    v_old_size bigint := 0;
    v_old_live boolean := false;
    v_new_file boolean;
    v_projected_bytes bigint;
BEGIN
    IF p_new_size < 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SIZE: file size must be non-negative'
            USING ERRCODE = '22023';
    END IF;
    SELECT w.quota_max_bytes,
           w.quota_max_files,
           w.quota_max_file_bytes,
           w.live_bytes,
           w.live_files
      INTO STRICT v_max_bytes,
                  v_max_files,
                  v_max_file_bytes,
                  v_live_bytes,
                  v_live_files
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = p_workspace_id;

    IF p_inode_id IS NOT NULL THEN
        SELECT i.size_bytes, i.live
          INTO v_old_size, v_old_live
         FROM _vexfs.inodes AS i
         WHERE i.workspace_id = p_workspace_id
           AND i.inode_id = p_inode_id
           AND i.kind <> 'directory';
        IF NOT FOUND THEN
            RAISE EXCEPTION 'VEXFS_CORRUPT: quota target inode is invalid'
                USING ERRCODE = 'XX001';
        END IF;
    END IF;
    v_new_file := p_inode_id IS NULL OR NOT v_old_live;
    v_projected_bytes := v_live_bytes
        - CASE WHEN v_old_live THEN v_old_size ELSE 0 END
        + p_new_size;

    IF v_max_file_bytes IS NOT NULL
       AND p_new_size > v_max_file_bytes
       AND (v_new_file OR p_new_size >= v_old_size) THEN
        RAISE EXCEPTION 'VEXFS_QUOTA_FILE_SIZE: maximum file size quota exceeded'
            USING ERRCODE = '53100';
    END IF;
    IF v_max_bytes IS NOT NULL
       AND v_projected_bytes > v_max_bytes
       AND v_projected_bytes >= v_live_bytes THEN
        RAISE EXCEPTION 'VEXFS_QUOTA_BYTES: workspace byte quota exceeded'
            USING ERRCODE = '53100';
    END IF;
    IF v_new_file
       AND v_max_files IS NOT NULL
       AND v_live_files + 1 > v_max_files THEN
        RAISE EXCEPTION 'VEXFS_QUOTA_FILES: workspace file quota exceeded'
            USING ERRCODE = '53100';
    END IF;
END;
$$;

CREATE FUNCTION _vexfs.enforce_snapshot_quota(
    p_workspace_id bigint,
    p_snapshot_id bigint)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_max_bytes bigint;
    v_max_files bigint;
    v_max_file_bytes bigint;
    v_live_bytes bigint;
    v_live_files bigint;
    v_live_largest bigint;
    v_target_bytes bigint;
    v_target_files bigint;
    v_target_largest bigint;
BEGIN
    SELECT w.quota_max_bytes,
           w.quota_max_files,
           w.quota_max_file_bytes,
           w.live_bytes,
           w.live_files,
           coalesce(max(i.size_bytes) FILTER (WHERE i.kind <> 'directory' AND i.live), 0)
      INTO STRICT v_max_bytes,
                  v_max_files,
                  v_max_file_bytes,
                  v_live_bytes,
                  v_live_files,
                  v_live_largest
      FROM _vexfs.workspaces AS w
      LEFT JOIN _vexfs.inodes AS i ON i.workspace_id = w.workspace_id
     WHERE w.workspace_id = p_workspace_id
     GROUP BY w.workspace_id;

    SELECT count(*) FILTER (WHERE s.kind <> 'directory'),
           coalesce(sum(s.size_bytes) FILTER (WHERE s.kind <> 'directory'), 0),
           coalesce(max(s.size_bytes) FILTER (WHERE s.kind <> 'directory'), 0)
      INTO v_target_files, v_target_bytes, v_target_largest
      FROM _vexfs.snapshot_inodes AS s
     WHERE s.snapshot_id = p_snapshot_id;

    IF v_max_file_bytes IS NOT NULL
       AND v_target_largest > v_max_file_bytes
       AND v_target_largest >= v_live_largest THEN
        RAISE EXCEPTION 'VEXFS_QUOTA_FILE_SIZE: maximum file size quota exceeded'
            USING ERRCODE = '53100';
    END IF;
    IF v_max_bytes IS NOT NULL
       AND v_target_bytes > v_max_bytes
       AND v_target_bytes >= v_live_bytes THEN
        RAISE EXCEPTION 'VEXFS_QUOTA_BYTES: workspace byte quota exceeded'
            USING ERRCODE = '53100';
    END IF;
    IF v_max_files IS NOT NULL
       AND v_target_files > v_max_files
       AND v_target_files >= v_live_files THEN
        RAISE EXCEPTION 'VEXFS_QUOTA_FILES: workspace file quota exceeded'
            USING ERRCODE = '53100';
    END IF;
END;
$$;

CREATE FUNCTION _vexfs.retention_keep_versions(p_workspace_id bigint)
RETURNS TABLE(inode_id bigint, version_no bigint)
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
WITH policy AS (
    SELECT w.retention_keep_versions AS keep_versions,
           w.retention_keep_days AS keep_days
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = p_workspace_id
),
ranked AS (
    SELECT f.inode_id,
           f.version_no,
           row_number() OVER (
               PARTITION BY f.inode_id ORDER BY f.version_no DESC) AS rank
      FROM _vexfs.file_versions AS f
     WHERE f.workspace_id = p_workspace_id
),
direct_keep AS (
    SELECT i.inode_id, i.current_version AS version_no
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = p_workspace_id
       AND i.kind <> 'directory'
       AND i.live
       AND i.current_version > 0
    UNION
    SELECT r.inode_id, r.version_no
      FROM ranked AS r
      CROSS JOIN policy AS p
     WHERE p.keep_versions > 0
       AND r.rank <= p.keep_versions
    UNION
    SELECT f.inode_id, f.version_no
      FROM _vexfs.file_versions AS f
      CROSS JOIN policy AS p
     WHERE f.workspace_id = p_workspace_id
       AND p.keep_days > 0
       AND f.created_at >= clock_timestamp() - make_interval(days => p.keep_days)
    UNION
    SELECT s.inode_id, s.current_version
      FROM _vexfs.snapshots AS snapshot
      JOIN _vexfs.snapshot_inodes AS s
        ON s.snapshot_id = snapshot.snapshot_id
     WHERE snapshot.workspace_id = p_workspace_id
       AND s.kind <> 'directory'
       AND s.current_version > 0
),
with_sources AS (
    SELECT keep.inode_id, keep.version_no
      FROM direct_keep AS keep
    UNION
    SELECT source.inode_id, source.source_version_no
      FROM direct_keep AS keep
      JOIN _vexfs.file_versions AS source
        ON source.workspace_id = p_workspace_id
       AND source.inode_id = keep.inode_id
       AND source.version_no = keep.version_no
     WHERE source.source_version_no IS NOT NULL
)
SELECT DISTINCT keep.inode_id, keep.version_no
  FROM with_sources AS keep
 WHERE keep.version_no IS NOT NULL
$$;

CREATE FUNCTION _vexfs.retention_status(
    p_workspace_id bigint,
    p_workspace_name text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_keep_versions integer;
    v_keep_days integer;
    v_gc_paused boolean;
    v_live_files bigint;
    v_live_bytes bigint;
    v_versions bigint;
    v_stored_bytes bigint;
    v_live_storage_bytes bigint;
    v_reclaimable_versions bigint;
    v_reclaimable_bytes bigint;
BEGIN
    SELECT w.retention_keep_versions,
           w.retention_keep_days,
           w.gc_paused,
           w.live_files,
           w.live_bytes
      INTO STRICT v_keep_versions,
                  v_keep_days,
                  v_gc_paused,
                  v_live_files,
                  v_live_bytes
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = p_workspace_id;

    SELECT count(*) INTO v_versions
      FROM _vexfs.file_versions AS f
     WHERE f.workspace_id = p_workspace_id;
    SELECT coalesce(sum(c.size_bytes), 0) INTO v_stored_bytes
      FROM _vexfs.chunks AS c
     WHERE c.workspace_id = p_workspace_id;

    WITH live_manifests AS (
        SELECT DISTINCT coalesce(current_version.manifest_id, source.manifest_id) AS manifest_id
          FROM _vexfs.inodes AS i
          JOIN _vexfs.file_versions AS current_version
            ON current_version.workspace_id = i.workspace_id
           AND current_version.inode_id = i.inode_id
           AND current_version.version_no = i.current_version
          LEFT JOIN _vexfs.file_versions AS source
            ON source.workspace_id = current_version.workspace_id
           AND source.inode_id = current_version.inode_id
           AND source.version_no = current_version.source_version_no
         WHERE i.workspace_id = p_workspace_id
           AND i.kind <> 'directory'
           AND i.live
    ),
    live_chunks AS (
        SELECT DISTINCT entry.chunk_id
          FROM live_manifests AS live
          JOIN _vexfs.manifest_chunks AS entry
            ON entry.manifest_id = live.manifest_id
    )
    SELECT coalesce(sum(c.size_bytes), 0) INTO v_live_storage_bytes
      FROM live_chunks AS live
      JOIN _vexfs.chunks AS c ON c.chunk_id = live.chunk_id;

    WITH keep AS MATERIALIZED (
        SELECT * FROM _vexfs.retention_keep_versions(p_workspace_id)
    ),
    reclaimable AS MATERIALIZED (
        SELECT f.inode_id, f.version_no, f.manifest_id
          FROM _vexfs.file_versions AS f
          LEFT JOIN keep
            ON keep.inode_id = f.inode_id
           AND keep.version_no = f.version_no
         WHERE f.workspace_id = p_workspace_id
           AND keep.inode_id IS NULL
    ),
    reclaimable_manifests AS MATERIALIZED (
        SELECT r.manifest_id
          FROM reclaimable AS r
         WHERE r.manifest_id IS NOT NULL
    ),
    reclaimable_chunks AS (
        SELECT DISTINCT entry.chunk_id
          FROM reclaimable_manifests AS candidate
          JOIN _vexfs.manifest_chunks AS entry
            ON entry.manifest_id = candidate.manifest_id
         WHERE NOT EXISTS (
             SELECT 1
               FROM _vexfs.manifest_chunks AS retained
              WHERE retained.chunk_id = entry.chunk_id
                AND NOT EXISTS (
                    SELECT 1
                      FROM reclaimable_manifests AS removed
                     WHERE removed.manifest_id = retained.manifest_id))
    )
    SELECT (SELECT count(*) FROM reclaimable),
           coalesce((
               SELECT sum(c.size_bytes)
                 FROM reclaimable_chunks AS item
                 JOIN _vexfs.chunks AS c ON c.chunk_id = item.chunk_id), 0)
      INTO v_reclaimable_versions, v_reclaimable_bytes;

    RETURN jsonb_build_object(
        'workspace', p_workspace_name,
        'keep_versions', v_keep_versions,
        'keep_days', v_keep_days,
        'gc_paused', v_gc_paused,
        'live_files', v_live_files,
        'live_bytes', v_live_bytes,
        'stored_versions', v_versions,
        'stored_version_bytes', v_stored_bytes,
        'retained_history_bytes', greatest(
            v_stored_bytes - v_live_storage_bytes, 0),
        'reclaimable_versions', v_reclaimable_versions,
        'reclaimable_bytes', v_reclaimable_bytes);
END;
$$;

CREATE FUNCTION _vexfs.quota_status(
    p_workspace_id bigint,
    p_workspace_name text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_largest bigint;
BEGIN
    SELECT * INTO STRICT v_workspace
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = p_workspace_id;
    SELECT coalesce(max(i.size_bytes), 0) INTO v_largest
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = p_workspace_id
       AND i.kind <> 'directory'
       AND i.live;
    RETURN jsonb_build_object(
        'workspace', p_workspace_name,
        'max_bytes', v_workspace.quota_max_bytes,
        'max_files', v_workspace.quota_max_files,
        'max_file_bytes', v_workspace.quota_max_file_bytes,
        'live_bytes', v_workspace.live_bytes,
        'live_files', v_workspace.live_files,
        'largest_file_bytes', v_largest,
        'over_quota',
            (v_workspace.quota_max_bytes IS NOT NULL
             AND v_workspace.live_bytes > v_workspace.quota_max_bytes)
            OR (v_workspace.quota_max_files IS NOT NULL
                AND v_workspace.live_files > v_workspace.quota_max_files)
            OR (v_workspace.quota_max_file_bytes IS NOT NULL
                AND v_largest > v_workspace.quota_max_file_bytes));
END;
$$;

CREATE FUNCTION public.vexfs_pg_adapter_version()
RETURNS text
LANGUAGE sql
IMMUTABLE
PARALLEL SAFE
AS $$ SELECT '0.4.0-alpha.1'::text $$;

-- These two functions are part of the database-neutral VexFS SQL contract.
-- PostgreSQL creates the private schema atomically in CREATE EXTENSION, so init
-- is an idempotent readiness probe rather than a schema migration entrypoint.
CREATE FUNCTION public.vexfs_init()
RETURNS integer
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $$ SELECT 1 $$;

CREATE FUNCTION public.vexfs_contract_version()
RETURNS text
LANGUAGE sql
IMMUTABLE
PARALLEL SAFE
AS $$ SELECT '0.9.0'::text $$;

CREATE FUNCTION public.vexfs_workspace_create(p_name text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace_id bigint;
    v_root_inode bigint;
    v_principal_oid oid;
BEGIN
    IF p_name IS NULL OR btrim(p_name) = '' OR p_name <> btrim(p_name) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_WORKSPACE: invalid workspace name'
            USING ERRCODE = '22023';
    END IF;

    SELECT r.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS r
     WHERE r.rolname = session_user;

    INSERT INTO _vexfs.workspaces(name, owner_oid, owner_role)
    VALUES (p_name, v_principal_oid, session_user)
    RETURNING workspace_id INTO v_workspace_id;

    INSERT INTO _vexfs.inodes(
        workspace_id, kind, mode, owner_oid, owner_role, owner_principal)
    VALUES (
        v_workspace_id, 'directory', 493,
        v_principal_oid, session_user, session_user)
    RETURNING inode_id INTO v_root_inode;

    UPDATE _vexfs.workspaces
       SET root_inode = v_root_inode
     WHERE workspace_id = v_workspace_id;
    PERFORM _vexfs.record_commit(
        v_workspace_id, 'workspace_create', '/', v_root_inode,
        jsonb_build_object('before_version', NULL, 'after_version', 0));
    RETURN v_workspace_id;
END;
$$;

CREATE FUNCTION public.vexfs_workspace_drop(p_name text, p_missing_ok boolean DEFAULT false)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    SELECT * INTO v_workspace
      FROM _vexfs.workspaces AS w
     WHERE w.name = p_name;
    IF NOT FOUND THEN
        IF p_missing_ok THEN
            RETURN 0;
        END IF;
        RAISE EXCEPTION 'VEXFS_WORKSPACE_NOT_FOUND: %', p_name
            USING ERRCODE = 'P0002';
    END IF;

    PERFORM _vexfs.require_workspace(p_name, 'admin');
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'workspace_drop', '/', v_workspace.root_inode,
        jsonb_build_object(
            'before_version', v_workspace.head_commit,
            'after_version', NULL));
    -- Staging deliberately keeps its base manifest alive during normal GC.
    -- On an explicit workspace drop, remove handles first so those references
    -- disappear before the workspace cascade reaches manifests.
    DELETE FROM _vexfs.handles
     WHERE workspace_id = v_workspace.workspace_id;
    DELETE FROM _vexfs.workspaces WHERE workspace_id = v_workspace.workspace_id;
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_mkdir(p_workspace text, p_path text, p_recursive boolean DEFAULT true)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_parts text[];
    v_part text;
    v_count integer;
    v_index integer := 0;
    v_parent bigint;
    v_next bigint;
    v_kind text;
    v_created boolean := false;
    v_principal_oid oid;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT r.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS r
     WHERE r.rolname = session_user;
    PERFORM 1 FROM _vexfs.workspaces WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    v_parts := _vexfs.path_parts(p_path);
    v_count := coalesce(array_length(v_parts, 1), 0);
    v_parent := v_workspace.root_inode;
    IF v_count = 0 THEN
        RETURN v_parent;
    END IF;

    FOREACH v_part IN ARRAY v_parts LOOP
        v_index := v_index + 1;
        PERFORM _vexfs.require_inode_permission(
            v_workspace.workspace_id, v_parent, 'execute');
        SELECT d.inode_id INTO v_next
          FROM _vexfs.dentries AS d
         WHERE d.workspace_id = v_workspace.workspace_id
           AND d.parent_inode = v_parent
           AND d.name = v_part;
        IF FOUND THEN
            SELECT i.kind INTO v_kind FROM _vexfs.inodes AS i WHERE i.inode_id = v_next;
            IF v_kind <> 'directory' THEN
                RAISE EXCEPTION 'VEXFS_NOT_DIRECTORY: %', p_path
                    USING ERRCODE = '42809';
            END IF;
        ELSE
            PERFORM _vexfs.require_inode_permission(
                v_workspace.workspace_id, v_parent, 'write');
            IF NOT p_recursive AND v_index < v_count THEN
                RAISE EXCEPTION 'VEXFS_PATH_NOT_FOUND: parent directory is missing'
                    USING ERRCODE = 'P0002';
            END IF;
            INSERT INTO _vexfs.inodes(
                workspace_id, kind, mode, owner_oid, owner_role, owner_principal)
            VALUES (
                v_workspace.workspace_id, 'directory', 493,
                v_principal_oid, session_user, session_user)
            RETURNING inode_id INTO v_next;
            INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
            VALUES (v_workspace.workspace_id, v_parent, v_part, v_next);
            PERFORM _vexfs.inherit_acl(
                v_workspace.workspace_id, v_parent, v_next);
            UPDATE _vexfs.inodes
               SET modified_at = clock_timestamp(),
                   changed_at = clock_timestamp()
             WHERE inode_id = v_parent;
            v_created := true;
        END IF;
        v_parent := v_next;
    END LOOP;

    IF v_created THEN
        PERFORM _vexfs.record_commit(
            v_workspace.workspace_id, 'mkdir', p_path, v_parent,
            jsonb_build_object('before_version', NULL, 'after_version', 0));
    END IF;
    RETURN v_parent;
END;
$$;

CREATE FUNCTION public.vexfs_write(p_workspace text, p_path text, p_content bytea)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_parts text[];
    v_count integer;
    v_name text;
    v_parent_path text;
    v_parent bigint;
    v_inode bigint;
    v_kind text;
    v_current_version bigint;
    v_version bigint;
    v_commit bigint;
    v_previous_manifest bigint;
    v_manifest bigint;
    v_checksum text;
    v_principal_oid oid;
BEGIN
    IF p_content IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CONTENT: content cannot be null'
            USING ERRCODE = '22004';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT r.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS r
     WHERE r.rolname = session_user;
    PERFORM 1 FROM _vexfs.workspaces WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    v_parts := _vexfs.path_parts(p_path);
    v_count := coalesce(array_length(v_parts, 1), 0);
    IF v_count = 0 THEN
        RAISE EXCEPTION 'VEXFS_IS_DIRECTORY: cannot write workspace root'
            USING ERRCODE = '42809';
    END IF;

    v_name := v_parts[v_count];
    IF v_count = 1 THEN
        v_parent_path := '/';
    ELSE
        v_parent_path := '/' || array_to_string(v_parts[1:v_count - 1], '/');
    END IF;
    v_parent := _vexfs.resolve_path(v_workspace.workspace_id, v_parent_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');
    SELECT i.kind INTO v_kind FROM _vexfs.inodes AS i WHERE i.inode_id = v_parent;
    IF v_kind <> 'directory' THEN
        RAISE EXCEPTION 'VEXFS_NOT_DIRECTORY: %', v_parent_path
            USING ERRCODE = '42809';
    END IF;

    SELECT d.inode_id INTO v_inode
      FROM _vexfs.dentries AS d
     WHERE d.workspace_id = v_workspace.workspace_id
       AND d.parent_inode = v_parent
       AND d.name = v_name;
    IF FOUND THEN
        SELECT i.kind, i.current_version
          INTO v_kind, v_current_version
          FROM _vexfs.inodes AS i
         WHERE i.inode_id = v_inode
         FOR UPDATE;
        IF v_kind <> 'file' THEN
            RAISE EXCEPTION 'VEXFS_NOT_REGULAR_FILE: %', p_path
                USING ERRCODE = '42809';
        END IF;
        PERFORM _vexfs.require_inode_permission(
            v_workspace.workspace_id, v_inode, 'write');
        PERFORM _vexfs.enforce_quota(
            v_workspace.workspace_id, v_inode, octet_length(p_content));
        SELECT coalesce(max(f.version_no), 0) + 1 INTO v_version
          FROM _vexfs.file_versions AS f
         WHERE f.workspace_id = v_workspace.workspace_id
           AND f.inode_id = v_inode;
        IF v_current_version > 0 THEN
            SELECT storage.manifest_id INTO v_previous_manifest
              FROM _vexfs.resolve_version_storage(
                  v_workspace.workspace_id, v_inode, v_current_version) AS storage;
        ELSE
            v_previous_manifest := NULL;
        END IF;
        UPDATE _vexfs.inodes
           SET current_version = v_version,
               size_bytes = octet_length(p_content),
               modified_at = clock_timestamp()
         WHERE inode_id = v_inode;
    ELSE
        PERFORM _vexfs.enforce_quota(
            v_workspace.workspace_id, NULL, octet_length(p_content));
        v_version := 1;
        INSERT INTO _vexfs.inodes(
            workspace_id, kind, mode, owner_oid, owner_role, owner_principal,
            current_version, size_bytes)
        VALUES (
            v_workspace.workspace_id, 'file', 420,
            v_principal_oid, session_user, session_user,
            v_version, octet_length(p_content))
        RETURNING inode_id INTO v_inode;
        INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
        VALUES (v_workspace.workspace_id, v_parent, v_name, v_inode);
        PERFORM _vexfs.inherit_acl(
            v_workspace.workspace_id, v_parent, v_inode);
    END IF;

    v_manifest := _vexfs.store_manifest(
        v_workspace.workspace_id, v_inode, v_previous_manifest, p_content);
    SELECT m.checksum INTO STRICT v_checksum
      FROM _vexfs.manifests AS m
     WHERE m.manifest_id = v_manifest;
    v_commit := _vexfs.record_commit(
        v_workspace.workspace_id, 'write', p_path, v_inode,
        jsonb_build_object(
            'before_version', v_current_version,
            'after_version', v_version));
    INSERT INTO _vexfs.file_versions(
        workspace_id, inode_id, version_no, commit_no, manifest_id,
        size_bytes, checksum, created_by_oid, created_by)
    VALUES (
        v_workspace.workspace_id, v_inode, v_version, v_commit, v_manifest,
        octet_length(p_content), v_checksum, v_principal_oid, session_user);
    UPDATE _vexfs.inodes
       SET changed_at = clock_timestamp()
     WHERE inode_id = v_inode;
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE inode_id = v_parent;
    RETURN v_version;
END;
$$;

CREATE FUNCTION public.vexfs_read(p_workspace text, p_path text)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_inode bigint;
    v_kind text;
    v_version bigint;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'read');
    SELECT i.kind, i.current_version INTO v_kind, v_version
      FROM _vexfs.inodes AS i WHERE i.inode_id = v_inode;
    IF v_kind <> 'file' THEN
        RAISE EXCEPTION 'VEXFS_NOT_REGULAR_FILE: %', p_path
            USING ERRCODE = '42809';
    END IF;
    IF v_version = 0 THEN
        RETURN ''::bytea;
    END IF;
    RETURN _vexfs.read_version_content(
        v_workspace.workspace_id, v_inode, v_version);
END;
$$;

CREATE FUNCTION public.vexfs_read_range(
    p_workspace text,
    p_path text,
    p_offset bigint,
    p_length bigint)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_inode bigint;
    v_kind text;
    v_version bigint;
BEGIN
    IF p_offset < 0 OR p_length < 0
       OR p_offset > 134217728 OR p_length > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: offset and length must be within 0..128 MiB'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'read');
    SELECT inode.kind, inode.current_version INTO v_kind, v_version
      FROM _vexfs.inodes AS inode WHERE inode.inode_id = v_inode;
    IF v_kind <> 'file' THEN
        RAISE EXCEPTION 'VEXFS_NOT_REGULAR_FILE: %', p_path
            USING ERRCODE = '42809';
    END IF;
    IF v_version = 0 THEN
        RETURN ''::bytea;
    END IF;
    RETURN _vexfs.read_version_range(
        v_workspace.workspace_id, v_inode, v_version, p_offset, p_length);
END;
$$;

CREATE FUNCTION public.vexfs_list(p_workspace text, p_path text)
RETURNS TABLE(name text, inode bigint, kind text, size bigint, version bigint)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_parent bigint;
    v_kind text;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    v_parent := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'read');
    SELECT i.kind INTO v_kind FROM _vexfs.inodes AS i WHERE i.inode_id = v_parent;
    IF v_kind <> 'directory' THEN
        RAISE EXCEPTION 'VEXFS_NOT_DIRECTORY: %', p_path
            USING ERRCODE = '42809';
    END IF;

    RETURN QUERY
    SELECT d.name,
           i.inode_id,
           i.kind,
           coalesce(f.size_bytes, 0)::bigint,
           i.current_version
      FROM _vexfs.dentries AS d
      JOIN _vexfs.inodes AS i ON i.inode_id = d.inode_id
      LEFT JOIN _vexfs.file_versions AS f
        ON f.workspace_id = i.workspace_id
       AND f.inode_id = i.inode_id
       AND f.version_no = i.current_version
     WHERE d.workspace_id = v_workspace.workspace_id
       AND d.parent_inode = v_parent
     ORDER BY d.name;
END;
$$;

CREATE FUNCTION public.vexfs_list_json(p_workspace text, p_path text)
RETURNS jsonb
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT coalesce(jsonb_agg(jsonb_build_object(
               'name', item.name,
               'inode', item.inode,
               'kind', item.kind,
               'size', item.size,
               'version', item.version,
               'mode', inode.mode,
               'created_at', (extract(epoch FROM inode.created_at) * 1000)::bigint,
               'accessed_at', (extract(epoch FROM inode.accessed_at) * 1000)::bigint,
               'updated_at', (extract(epoch FROM inode.modified_at) * 1000)::bigint,
               'changed_at', (extract(epoch FROM inode.changed_at) * 1000)::bigint,
               'uid', inode.uid,
               'gid', inode.gid,
               'link_count', CASE WHEN inode.kind = 'directory' THEN 2 + (
                   SELECT count(*) FROM _vexfs.dentries AS child_entry
                   JOIN _vexfs.inodes AS child ON child.inode_id = child_entry.inode_id
                   WHERE child_entry.workspace_id = inode.workspace_id
                     AND child_entry.parent_inode = inode.inode_id
                     AND child.kind = 'directory' AND child.live)
                   ELSE (SELECT count(*) FROM _vexfs.dentries AS link
                         WHERE link.workspace_id = inode.workspace_id
                           AND link.inode_id = inode.inode_id) END)
               ORDER BY item.name), '[]'::jsonb)
      FROM public.vexfs_list(p_workspace, p_path) AS item
      JOIN _vexfs.inodes AS inode ON inode.inode_id = item.inode
$$;

CREATE FUNCTION _vexfs.glob_regex(p_pattern text)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
STRICT
PARALLEL SAFE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_index integer;
    v_character text;
    v_regex text := '^';
BEGIN
    FOR v_index IN 1..char_length(p_pattern) LOOP
        v_character := substr(p_pattern, v_index, 1);
        IF v_character = '*' THEN
            v_regex := v_regex || '.*';
        ELSIF v_character = '?' THEN
            v_regex := v_regex || '.';
        ELSIF strpos('.+()[]{}^$|', v_character) > 0 OR v_character = chr(92) THEN
            v_regex := v_regex || chr(92) || v_character;
        ELSE
            v_regex := v_regex || v_character;
        END IF;
    END LOOP;
    RETURN v_regex || '$';
END;
$$;

CREATE FUNCTION public.vexfs_find(
    p_workspace text,
    p_path text,
    p_name_pattern text,
    p_kind text,
    p_min_size bigint,
    p_max_size bigint,
    p_modified_after_ms bigint,
    p_modified_before_ms bigint,
    p_after_path text,
    p_limit integer)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_root bigint;
    v_parts text[];
    v_root_name text;
    v_name_regex text;
    v_actor_oid oid;
    v_superuser boolean;
    v_unrestricted boolean;
    v_entries jsonb;
    v_next_cursor text;
BEGIN
    IF p_name_pattern IS NOT NULL AND
       (p_name_pattern = '' OR octet_length(p_name_pattern) > 255) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_FIND: name pattern must be 1..255 bytes'
            USING ERRCODE = '22023';
    END IF;
    IF p_kind IS NOT NULL AND p_kind NOT IN ('file', 'directory', 'symlink') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_FIND: kind must be file, directory or symlink'
            USING ERRCODE = '22023';
    END IF;
    IF p_limit NOT BETWEEN 1 AND 1000 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_FIND: limit must be between 1 and 1000'
            USING ERRCODE = '22023';
    END IF;
    IF coalesce(p_min_size, 0) < 0 OR coalesce(p_max_size, 0) < 0 OR
       coalesce(p_modified_after_ms, 0) < 0 OR
       coalesce(p_modified_before_ms, 0) < 0 OR
       coalesce(p_modified_after_ms, 0) > 253402300799999 OR
       coalesce(p_modified_before_ms, 0) > 253402300799999 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_FIND: numeric filters are outside the supported range'
            USING ERRCODE = '22023';
    END IF;
    IF p_min_size IS NOT NULL AND p_max_size IS NOT NULL AND
       p_min_size > p_max_size THEN
        RAISE EXCEPTION 'VEXFS_INVALID_FIND: minimum size exceeds maximum size'
            USING ERRCODE = '22023';
    END IF;
    IF p_modified_after_ms IS NOT NULL AND p_modified_before_ms IS NOT NULL AND
       p_modified_after_ms > p_modified_before_ms THEN
        RAISE EXCEPTION 'VEXFS_INVALID_FIND: modified-after exceeds modified-before'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    v_parts := _vexfs.path_parts(p_path);
    v_root := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_root, 'read');
    SELECT role.oid, role.rolsuper
      INTO STRICT v_actor_oid, v_superuser
      FROM pg_catalog.pg_roles AS role
     WHERE role.rolname = session_user;
    v_unrestricted := v_superuser OR v_workspace.owner_oid = v_actor_oid;
    IF p_after_path IS NOT NULL THEN
        PERFORM _vexfs.path_parts(p_after_path);
    END IF;
    v_root_name := CASE WHEN cardinality(v_parts) = 0 THEN '/'
                        ELSE v_parts[cardinality(v_parts)] END;
    IF p_name_pattern IS NOT NULL THEN
        v_name_regex := _vexfs.glob_regex(p_name_pattern);
    END IF;

    WITH RECURSIVE directories(path, inode_id) AS (
        SELECT p_path, inode.inode_id
          FROM _vexfs.inodes AS inode
         WHERE inode.workspace_id = v_workspace.workspace_id
           AND inode.inode_id = v_root
           AND inode.kind = 'directory'
        UNION ALL
        SELECT CASE WHEN directories.path = '/' THEN '/' || dentry.name
                    ELSE directories.path || '/' || dentry.name END,
               inode.inode_id
          FROM directories
          JOIN _vexfs.dentries AS dentry
            ON dentry.workspace_id = v_workspace.workspace_id
           AND dentry.parent_inode = directories.inode_id
          JOIN _vexfs.inodes AS inode
            ON inode.workspace_id = v_workspace.workspace_id
           AND inode.inode_id = dentry.inode_id
           AND inode.live
         WHERE inode.kind = 'directory'
           AND (v_unrestricted OR _vexfs.has_acl_permission(
                   v_workspace.workspace_id, inode.inode_id, 'read'))),
    tree(path, name, inode_id, kind, size_bytes, version_no, modified_at) AS (
        SELECT p_path,
               v_root_name,
               inode.inode_id,
               inode.kind,
               inode.size_bytes,
               inode.current_version,
               inode.modified_at
          FROM _vexfs.inodes AS inode
         WHERE inode.workspace_id = v_workspace.workspace_id
           AND inode.inode_id = v_root
        UNION ALL
        SELECT CASE WHEN directories.path = '/' THEN '/' || dentry.name
                    ELSE directories.path || '/' || dentry.name END,
               dentry.name,
               inode.inode_id,
               inode.kind,
               inode.size_bytes,
               inode.current_version,
               inode.modified_at
          FROM directories
          JOIN _vexfs.dentries AS dentry
            ON dentry.workspace_id = v_workspace.workspace_id
           AND dentry.parent_inode = directories.inode_id
          JOIN _vexfs.inodes AS inode
            ON inode.workspace_id = v_workspace.workspace_id
           AND inode.inode_id = dentry.inode_id
           AND inode.live
         WHERE v_unrestricted OR _vexfs.has_acl_permission(
                   v_workspace.workspace_id, inode.inode_id, 'read')),
    limited AS MATERIALIZED (
        SELECT tree.*
          FROM tree
         WHERE (p_after_path IS NULL OR
                tree.path COLLATE "C" > p_after_path COLLATE "C")
           AND (v_name_regex IS NULL OR tree.name ~ v_name_regex)
           AND (p_kind IS NULL OR tree.kind = p_kind)
           AND (p_min_size IS NULL OR tree.size_bytes >= p_min_size)
           AND (p_max_size IS NULL OR tree.size_bytes <= p_max_size)
           AND (p_modified_after_ms IS NULL OR
                tree.modified_at >= pg_catalog.to_timestamp(p_modified_after_ms / 1000.0))
           AND (p_modified_before_ms IS NULL OR
                tree.modified_at <= pg_catalog.to_timestamp(p_modified_before_ms / 1000.0))
         ORDER BY tree.path COLLATE "C"
         LIMIT p_limit + 1),
    numbered AS (
        SELECT limited.*,
               row_number() OVER (ORDER BY limited.path COLLATE "C") AS row_no,
               count(*) OVER () AS total
          FROM limited)
    SELECT coalesce(jsonb_agg(jsonb_build_object(
               'path', numbered.path,
               'name', numbered.name,
               'inode', numbered.inode_id,
               'kind', numbered.kind,
               'size', numbered.size_bytes,
               'version', numbered.version_no,
               'modified_at',
                   (extract(epoch FROM numbered.modified_at) * 1000)::bigint)
               ORDER BY numbered.path COLLATE "C")
               FILTER (WHERE numbered.row_no <= p_limit), '[]'::jsonb),
           CASE WHEN coalesce(max(numbered.total), 0) > p_limit THEN
               max(numbered.path) FILTER (WHERE numbered.row_no = p_limit)
           END
      INTO v_entries, v_next_cursor
      FROM numbered;
    RETURN jsonb_build_object(
        'root', p_path,
        'entries', v_entries,
        'next_cursor', v_next_cursor);
END;
$$;

CREATE FUNCTION _vexfs.try_utf8(p_content bytea)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
STRICT
PARALLEL SAFE
SET search_path = pg_catalog, _vexfs
AS $$
BEGIN
    RETURN convert_from(p_content, 'UTF8');
EXCEPTION WHEN character_not_in_repertoire OR untranslatable_character THEN
    RETURN NULL;
END;
$$;

CREATE FUNCTION _vexfs.grep_index_available()
RETURNS boolean
LANGUAGE sql
STABLE
PARALLEL SAFE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT EXISTS (
        SELECT 1 FROM pg_catalog.pg_extension WHERE extname = 'pg_trgm')
$$;

CREATE FUNCTION _vexfs.literal_like_pattern(p_pattern text)
RETURNS text
LANGUAGE sql
IMMUTABLE
STRICT
PARALLEL SAFE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT '%' || replace(
        replace(
            replace(p_pattern, chr(92), chr(92) || chr(92)),
            '%', chr(92) || '%'),
        '_', chr(92) || '_') || '%'
$$;

CREATE FUNCTION _vexfs.ensure_grep_trgm_index()
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_schema name;
BEGIN
    SELECT n.nspname INTO v_schema
      FROM pg_catalog.pg_extension AS e
      JOIN pg_catalog.pg_namespace AS n ON n.oid = e.extnamespace
     WHERE e.extname = 'pg_trgm';
    IF v_schema IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INDEX_UNAVAILABLE: install pg_trgm before enabling the index'
            USING ERRCODE = '55000';
    END IF;
    EXECUTE format(
        'CREATE INDEX IF NOT EXISTS vexfs_grep_documents_content_trgm_idx '
        'ON _vexfs.grep_documents USING gin (content %I.gin_trgm_ops)',
        v_schema);
END;
$$;

CREATE FUNCTION _vexfs.refresh_grep_document(
    p_workspace_id bigint,
    p_inode_id bigint)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_inode _vexfs.inodes%ROWTYPE;
    v_content bytea;
    v_text text;
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM _vexfs.workspaces AS w
         WHERE w.workspace_id = p_workspace_id AND w.grep_index_enabled) THEN
        RETURN;
    END IF;
    DELETE FROM _vexfs.grep_documents AS document
     WHERE document.inode_id = p_inode_id;
    SELECT * INTO v_inode
      FROM _vexfs.inodes AS inode
     WHERE inode.workspace_id = p_workspace_id
       AND inode.inode_id = p_inode_id;
    IF NOT FOUND OR NOT v_inode.live OR v_inode.kind <> 'file'
       OR v_inode.current_version < 1
       OR NOT EXISTS (
           SELECT 1 FROM _vexfs.file_versions AS version
            WHERE version.workspace_id = p_workspace_id
              AND version.inode_id = p_inode_id
              AND version.version_no = v_inode.current_version) THEN
        RETURN;
    END IF;
    v_content := _vexfs.read_version_content(
        p_workspace_id, p_inode_id, v_inode.current_version);
    IF position(decode('00', 'hex') IN v_content) > 0 THEN
        RETURN;
    END IF;
    v_text := _vexfs.try_utf8(v_content);
    IF v_text IS NULL THEN
        RETURN;
    END IF;
    INSERT INTO _vexfs.grep_documents(
        inode_id, workspace_id, version_no, content)
    VALUES (p_inode_id, p_workspace_id, v_inode.current_version, v_text)
    ON CONFLICT (inode_id) DO UPDATE
       SET workspace_id = EXCLUDED.workspace_id,
           version_no = EXCLUDED.version_no,
           content = EXCLUDED.content,
           indexed_at = clock_timestamp();
EXCEPTION WHEN OTHERS THEN
    UPDATE _vexfs.workspaces
       SET grep_index_dirty = true
     WHERE workspace_id = p_workspace_id;
END;
$$;

CREATE FUNCTION _vexfs.refresh_grep_documents_after_inode_insert()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_inode record;
BEGIN
    FOR v_inode IN
        SELECT inserted.workspace_id, inserted.inode_id
          FROM inserted_inodes AS inserted
          JOIN _vexfs.workspaces AS workspace
            ON workspace.workspace_id = inserted.workspace_id
           AND workspace.grep_index_enabled
    LOOP
        PERFORM _vexfs.refresh_grep_document(
            v_inode.workspace_id, v_inode.inode_id);
    END LOOP;
    RETURN NULL;
END;
$$;

CREATE TRIGGER vexfs_grep_document_inode_insert
AFTER INSERT ON _vexfs.inodes
REFERENCING NEW TABLE AS inserted_inodes
FOR EACH STATEMENT EXECUTE FUNCTION _vexfs.refresh_grep_documents_after_inode_insert();

CREATE FUNCTION _vexfs.refresh_grep_documents_after_inode_update()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_inode record;
BEGIN
    FOR v_inode IN
        SELECT updated.workspace_id, updated.inode_id
          FROM updated_inodes AS updated
          JOIN previous_inodes AS previous USING (inode_id)
          JOIN _vexfs.workspaces AS workspace
            ON workspace.workspace_id = updated.workspace_id
           AND workspace.grep_index_enabled
         WHERE updated.workspace_id IS DISTINCT FROM previous.workspace_id
            OR updated.current_version IS DISTINCT FROM previous.current_version
            OR updated.live IS DISTINCT FROM previous.live
            OR updated.kind IS DISTINCT FROM previous.kind
    LOOP
        PERFORM _vexfs.refresh_grep_document(
            v_inode.workspace_id, v_inode.inode_id);
    END LOOP;
    RETURN NULL;
END;
$$;

CREATE TRIGGER vexfs_grep_document_inode_update
AFTER UPDATE ON _vexfs.inodes
REFERENCING OLD TABLE AS previous_inodes NEW TABLE AS updated_inodes
FOR EACH STATEMENT EXECUTE FUNCTION _vexfs.refresh_grep_documents_after_inode_update();

CREATE FUNCTION _vexfs.refresh_grep_documents_after_version_insert()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_inode record;
BEGIN
    FOR v_inode IN
        SELECT DISTINCT inserted.workspace_id, inserted.inode_id
          FROM inserted_versions AS inserted
          JOIN _vexfs.workspaces AS workspace
            ON workspace.workspace_id = inserted.workspace_id
           AND workspace.grep_index_enabled
    LOOP
        PERFORM _vexfs.refresh_grep_document(
            v_inode.workspace_id, v_inode.inode_id);
    END LOOP;
    RETURN NULL;
END;
$$;

CREATE TRIGGER vexfs_grep_document_version_insert
AFTER INSERT ON _vexfs.file_versions
REFERENCING NEW TABLE AS inserted_versions
FOR EACH STATEMENT EXECUTE FUNCTION _vexfs.refresh_grep_documents_after_version_insert();

CREATE FUNCTION _vexfs.rebuild_grep_index(p_workspace_id bigint)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_inode record;
    v_content bytea;
    v_text text;
    v_count bigint := 0;
BEGIN
    DELETE FROM _vexfs.grep_documents AS document
     WHERE document.workspace_id = p_workspace_id;
    FOR v_inode IN
        SELECT inode.inode_id, inode.current_version
          FROM _vexfs.inodes AS inode
         WHERE inode.workspace_id = p_workspace_id
           AND inode.live AND inode.kind = 'file'
           AND inode.current_version > 0
         ORDER BY inode.inode_id
    LOOP
        v_content := _vexfs.read_version_content(
            p_workspace_id, v_inode.inode_id, v_inode.current_version);
        IF position(decode('00', 'hex') IN v_content) > 0 THEN
            CONTINUE;
        END IF;
        v_text := _vexfs.try_utf8(v_content);
        IF v_text IS NULL THEN
            CONTINUE;
        END IF;
        INSERT INTO _vexfs.grep_documents(
            inode_id, workspace_id, version_no, content)
        VALUES (
            v_inode.inode_id, p_workspace_id,
            v_inode.current_version, v_text);
        v_count := v_count + 1;
    END LOOP;
    UPDATE _vexfs.workspaces
       SET grep_index_dirty = false
     WHERE workspace_id = p_workspace_id;
    -- The first indexed grep happens immediately after enable/rebuild. Refresh
    -- planner statistics now so PostgreSQL does not scan the derived table
    -- using stale pre-build row counts instead of the trigram GIN index.
    EXECUTE 'ANALYZE _vexfs.grep_documents';
    RETURN v_count;
END;
$$;

CREATE FUNCTION _vexfs.grep_indexed_files(
    p_workspace_id bigint,
    p_root_inode bigint,
    p_root_path text,
    p_pattern text,
    p_ignore_case boolean)
RETURNS TABLE(
    path text,
    inode_id bigint,
    current_version bigint,
    size_bytes bigint)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
SET enable_seqscan = off
AS $$
DECLARE
    v_like text := _vexfs.literal_like_pattern(p_pattern);
BEGIN
    IF p_ignore_case THEN
        RETURN QUERY
        WITH RECURSIVE
        candidates AS MATERIALIZED (
            SELECT document.inode_id, document.workspace_id, document.version_no
              FROM _vexfs.grep_documents AS document
             WHERE document.content ILIKE v_like ESCAPE E'\\'
        ),
        tree(inode_id, path) AS (
            SELECT p_root_inode, p_root_path
            UNION ALL
            SELECT d.inode_id,
                   CASE WHEN tree.path = '/' THEN '/' || d.name
                        ELSE tree.path || '/' || d.name END
              FROM tree
              JOIN _vexfs.inodes AS parent
                ON parent.inode_id = tree.inode_id
               AND parent.kind = 'directory' AND parent.live
              JOIN _vexfs.dentries AS d
                ON d.workspace_id = p_workspace_id
               AND d.parent_inode = tree.inode_id
        )
        SELECT tree.path, inode.inode_id,
               inode.current_version, inode.size_bytes
          FROM candidates AS candidate
          JOIN _vexfs.inodes AS inode
            ON inode.inode_id = candidate.inode_id
           AND inode.workspace_id = p_workspace_id
           AND inode.live AND inode.kind = 'file'
           AND candidate.workspace_id = p_workspace_id
           AND candidate.version_no = inode.current_version
          JOIN tree ON tree.inode_id = inode.inode_id
         ORDER BY tree.path;
    ELSE
        RETURN QUERY
        WITH RECURSIVE
        candidates AS MATERIALIZED (
            SELECT document.inode_id, document.workspace_id, document.version_no
              FROM _vexfs.grep_documents AS document
             WHERE document.content LIKE v_like ESCAPE E'\\'
        ),
        tree(inode_id, path) AS (
            SELECT p_root_inode, p_root_path
            UNION ALL
            SELECT d.inode_id,
                   CASE WHEN tree.path = '/' THEN '/' || d.name
                        ELSE tree.path || '/' || d.name END
              FROM tree
              JOIN _vexfs.inodes AS parent
                ON parent.inode_id = tree.inode_id
               AND parent.kind = 'directory' AND parent.live
              JOIN _vexfs.dentries AS d
                ON d.workspace_id = p_workspace_id
               AND d.parent_inode = tree.inode_id
        )
        SELECT tree.path, inode.inode_id,
               inode.current_version, inode.size_bytes
          FROM candidates AS candidate
          JOIN _vexfs.inodes AS inode
            ON inode.inode_id = candidate.inode_id
           AND inode.workspace_id = p_workspace_id
           AND inode.live AND inode.kind = 'file'
           AND candidate.workspace_id = p_workspace_id
           AND candidate.version_no = inode.current_version
          JOIN tree ON tree.inode_id = inode.inode_id
         ORDER BY tree.path;
    END IF;
END;
$$;

CREATE FUNCTION _vexfs.grep_files(
    p_workspace_id bigint,
    p_root_inode bigint,
    p_root_path text,
    p_pattern text,
    p_ignore_case boolean,
    p_use_index boolean)
RETURNS TABLE(
    path text,
    inode_id bigint,
    current_version bigint,
    size_bytes bigint)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
BEGIN
    IF NOT p_use_index THEN
        RETURN QUERY
        WITH RECURSIVE tree(inode_id, path) AS (
            SELECT p_root_inode, p_root_path
            UNION ALL
            SELECT d.inode_id,
                   CASE WHEN tree.path = '/' THEN '/' || d.name
                        ELSE tree.path || '/' || d.name END
              FROM tree
              JOIN _vexfs.inodes AS parent
                ON parent.inode_id = tree.inode_id
               AND parent.kind = 'directory' AND parent.live
              JOIN _vexfs.dentries AS d
                ON d.workspace_id = p_workspace_id
               AND d.parent_inode = tree.inode_id
        )
        SELECT tree.path, inode.inode_id,
               inode.current_version, inode.size_bytes
          FROM tree
          JOIN _vexfs.inodes AS inode
            ON inode.inode_id = tree.inode_id
           AND inode.live AND inode.kind = 'file'
         ORDER BY tree.path;
        RETURN;
    END IF;
    RETURN QUERY
    SELECT indexed.path, indexed.inode_id,
           indexed.current_version, indexed.size_bytes
      FROM _vexfs.grep_indexed_files(
          p_workspace_id, p_root_inode, p_root_path,
          p_pattern, p_ignore_case) AS indexed;
END;
$$;

CREATE FUNCTION public.vexfs_grep(
    p_workspace text,
    p_path text,
    p_pattern text,
    p_flags integer,
    p_limit integer)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_root bigint;
    v_root_path text;
    v_file record;
    v_line record;
    v_content bytea;
    v_text text;
    v_matches jsonb := '[]'::jsonb;
    v_match_count bigint := 0;
    v_files_scanned bigint := 0;
    v_bytes_scanned bigint := 0;
    v_binary_skipped bigint := 0;
    v_truncated boolean := false;
    v_ignore_case boolean;
    v_files_only boolean;
    v_index_used boolean;
BEGIN
    IF p_pattern IS NULL OR p_pattern = '' OR octet_length(p_pattern) > 4096 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_GREP: pattern must be 1..4096 bytes'
            USING ERRCODE = '22023';
    END IF;
    IF p_flags < 0 OR (p_flags & ~3) <> 0 OR p_limit < 1 OR p_limit > 10240 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_GREP: flags or result limit is invalid'
            USING ERRCODE = '22023';
    END IF;
    v_ignore_case := (p_flags & 1) <> 0;
    v_files_only := (p_flags & 2) <> 0;
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    v_root := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_root, 'read');
    v_root_path := CASE WHEN p_path = '/' THEN '/'
                        ELSE rtrim(p_path, '/') END;
    v_index_used := v_workspace.grep_index_enabled
        AND NOT v_workspace.grep_index_dirty
        AND _vexfs.grep_index_available()
        AND char_length(p_pattern) >= 3;
    FOR v_file IN
        SELECT * FROM _vexfs.grep_files(
            v_workspace.workspace_id, v_root, v_root_path,
            p_pattern, v_ignore_case, v_index_used)
    LOOP
        PERFORM _vexfs.require_inode_permission(
            v_workspace.workspace_id, v_file.inode_id, 'read');
        v_content := _vexfs.read_version_content(
            v_workspace.workspace_id, v_file.inode_id, v_file.current_version);
        v_files_scanned := v_files_scanned + 1;
        v_bytes_scanned := v_bytes_scanned + octet_length(v_content);
        IF position(decode('00', 'hex') IN v_content) > 0 THEN
            v_binary_skipped := v_binary_skipped + 1;
            CONTINUE;
        END IF;
        v_text := _vexfs.try_utf8(v_content);
        IF v_text IS NULL THEN
            v_binary_skipped := v_binary_skipped + 1;
            CONTINUE;
        END IF;
        FOR v_line IN
            SELECT line.text, line.ordinality
              FROM regexp_split_to_table(v_text, E'\\n')
                   WITH ORDINALITY AS line(text, ordinality)
        LOOP
            IF (v_ignore_case AND position(lower(p_pattern) IN lower(v_line.text)) > 0)
               OR (NOT v_ignore_case AND position(p_pattern IN v_line.text) > 0) THEN
                v_matches := v_matches || jsonb_build_array(jsonb_build_object(
                    'path', v_file.path,
                    'line', v_line.ordinality,
                    'text', v_line.text));
                v_match_count := v_match_count + 1;
                IF v_match_count >= p_limit THEN
                    v_truncated := true;
                    EXIT;
                END IF;
                IF v_files_only THEN
                    EXIT;
                END IF;
            END IF;
        END LOOP;
        EXIT WHEN v_truncated;
    END LOOP;
    RETURN jsonb_build_object(
        'matches', v_matches,
        'match_count', v_match_count,
        'files_scanned', v_files_scanned,
        'bytes_scanned', v_bytes_scanned,
        'binary_files_skipped', v_binary_skipped,
        'index_used', v_index_used,
        'truncated', v_truncated);
END;
$$;

-- SQLite exposes a database-wide one-argument form. PostgreSQL settings are
-- workspace-scoped, so the one-argument form is only a server capability probe.
CREATE FUNCTION public.vexfs_grep_index(p_action text)
RETURNS jsonb
LANGUAGE plpgsql
STABLE
PARALLEL SAFE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_available boolean;
BEGIN
    IF p_action IS NULL OR p_action NOT IN ('status', 'enable', 'rebuild', 'disable') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_INDEX_ACTION: use status, enable, rebuild, or disable'
            USING ERRCODE = '22023';
    END IF;
    IF p_action <> 'status' THEN
        RAISE EXCEPTION 'VEXFS_WORKSPACE_REQUIRED: use vexfs_grep_index(workspace, action)'
            USING ERRCODE = '22023';
    END IF;
    v_available := _vexfs.grep_index_available();
    RETURN jsonb_build_object(
        'enabled', false,
        'available', v_available,
        'dirty', false,
        'backend', CASE WHEN v_available THEN 'pg-trgm' ELSE 'postgresql-scan' END,
        'indexed_files', 0,
        'requested_action', p_action);
END;
$$;

CREATE FUNCTION public.vexfs_grep_index(p_workspace text, p_action text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_available boolean;
    v_enabled boolean;
    v_dirty boolean;
    v_indexed_files bigint;
BEGIN
    IF p_action IS NULL OR p_action NOT IN ('status', 'enable', 'rebuild', 'disable') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_INDEX_ACTION: use status, enable, rebuild, or disable'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(
        p_workspace, CASE WHEN p_action = 'status' THEN 'read' ELSE 'admin' END);
    v_available := _vexfs.grep_index_available();
    IF p_action = 'enable' THEN
        IF NOT v_available THEN
            RAISE EXCEPTION 'VEXFS_INDEX_UNAVAILABLE: install pg_trgm before enabling the index'
                USING ERRCODE = '55000';
        END IF;
        PERFORM _vexfs.ensure_grep_trgm_index();
        UPDATE _vexfs.workspaces
           SET grep_index_enabled = true,
               grep_index_dirty = true
         WHERE workspace_id = v_workspace.workspace_id;
        PERFORM _vexfs.rebuild_grep_index(v_workspace.workspace_id);
    ELSIF p_action = 'rebuild' THEN
        IF NOT v_workspace.grep_index_enabled THEN
            RAISE EXCEPTION 'VEXFS_INDEX_DISABLED: enable the workspace index first'
                USING ERRCODE = '55000';
        END IF;
        IF NOT v_available THEN
            RAISE EXCEPTION 'VEXFS_INDEX_UNAVAILABLE: install pg_trgm before rebuilding the index'
                USING ERRCODE = '55000';
        END IF;
        PERFORM _vexfs.ensure_grep_trgm_index();
        UPDATE _vexfs.workspaces
           SET grep_index_dirty = true
         WHERE workspace_id = v_workspace.workspace_id;
        PERFORM _vexfs.rebuild_grep_index(v_workspace.workspace_id);
    ELSIF p_action = 'disable' THEN
        DELETE FROM _vexfs.grep_documents AS document
         WHERE document.workspace_id = v_workspace.workspace_id;
        UPDATE _vexfs.workspaces
           SET grep_index_enabled = false,
               grep_index_dirty = false
         WHERE workspace_id = v_workspace.workspace_id;
    END IF;
    SELECT workspace.grep_index_enabled, workspace.grep_index_dirty
      INTO v_enabled, v_dirty
      FROM _vexfs.workspaces AS workspace
     WHERE workspace.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_indexed_files
      FROM _vexfs.grep_documents AS document
     WHERE document.workspace_id = v_workspace.workspace_id;
    RETURN jsonb_build_object(
        'enabled', v_enabled,
        'available', v_available,
        'dirty', v_dirty,
        'backend', CASE WHEN v_available THEN 'pg-trgm' ELSE 'postgresql-scan' END,
        'indexed_files', v_indexed_files,
        'requested_action', p_action,
        'message', CASE
            WHEN NOT v_available THEN 'install pg_trgm to enable the optional substring index'
            WHEN NOT v_enabled THEN 'substring index is available but disabled for this workspace'
            WHEN v_dirty THEN 'substring index is dirty; grep falls back to PostgreSQL scan'
            ELSE 'substring index is ready'
        END);
END;
$$;

CREATE FUNCTION public.vexfs_stat(p_workspace text, p_path text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_inode bigint;
    v_result jsonb;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'read');
    SELECT jsonb_build_object(
               'path', _vexfs.path_for_inode(v_workspace.workspace_id, i.inode_id),
               'inode', i.inode_id,
               'kind', i.kind,
               'mode', i.mode,
               'owner', coalesce(r.rolname, i.owner_role),
               'owner_principal', i.owner_principal,
               'uid', i.uid,
               'gid', i.gid,
               'version', i.current_version,
               'size', i.size_bytes,
               'checksum', f.checksum,
               'created_at', (extract(epoch FROM i.created_at) * 1000)::bigint,
               'accessed_at', (extract(epoch FROM i.accessed_at) * 1000)::bigint,
               'updated_at', (extract(epoch FROM i.modified_at) * 1000)::bigint,
               'modified_at', (extract(epoch FROM i.modified_at) * 1000)::bigint,
               'changed_at', (extract(epoch FROM i.changed_at) * 1000)::bigint,
               'link_count', CASE
                   WHEN i.kind = 'directory' THEN 2 + (
                       SELECT count(*)
                         FROM _vexfs.dentries AS child_entry
                         JOIN _vexfs.inodes AS child
                           ON child.inode_id = child_entry.inode_id
                        WHERE child_entry.workspace_id = i.workspace_id
                          AND child_entry.parent_inode = i.inode_id
                          AND child.kind = 'directory'
                          AND child.live)
                   ELSE (
                       SELECT count(*)
                         FROM _vexfs.dentries AS link
                        WHERE link.workspace_id = i.workspace_id
                          AND link.inode_id = i.inode_id)
               END,
               'workspace_head', v_workspace.head_commit)
      INTO v_result
      FROM _vexfs.inodes AS i
      LEFT JOIN pg_catalog.pg_roles AS r ON r.oid = i.owner_oid
      LEFT JOIN _vexfs.file_versions AS f
        ON f.workspace_id = i.workspace_id
       AND f.inode_id = i.inode_id
       AND f.version_no = i.current_version
     WHERE i.inode_id = v_inode;
    RETURN v_result;
END;
$$;

CREATE FUNCTION public.vexfs_list_versioned_json(p_workspace text, p_path text)
RETURNS jsonb
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT jsonb_build_object(
        'version', (public.vexfs_stat(p_workspace, p_path)->>'workspace_head')::bigint,
        'entries', public.vexfs_list_json(p_workspace, p_path))
$$;

CREATE FUNCTION public.vexfs_path(p_workspace text, p_inode bigint)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_path text;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'read');
    WITH RECURSIVE paths(inode_id, path, visited) AS (
        SELECT v_workspace.root_inode, '/'::text, ARRAY[v_workspace.root_inode]::bigint[]
        UNION ALL
        SELECT d.inode_id,
               CASE WHEN paths.path = '/' THEN '/' || d.name
                    ELSE paths.path || '/' || d.name END,
               paths.visited || d.inode_id
          FROM paths
          JOIN _vexfs.dentries AS d
            ON d.workspace_id = v_workspace.workspace_id
           AND d.parent_inode = paths.inode_id
         WHERE NOT d.inode_id = ANY(paths.visited)
           AND octet_length(paths.path) + octet_length(d.name) + 1 <= 4096)
    SELECT paths.path INTO v_path
      FROM paths
     WHERE paths.inode_id = p_inode
     ORDER BY paths.path
     LIMIT 1;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_inode
            USING ERRCODE = 'P0002';
    END IF;
    RETURN v_path;
END;
$$;

CREATE FUNCTION public.vexfs_append(
    p_workspace text,
    p_inode bigint,
    p_content bytea)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_path text;
    v_current bytea;
BEGIN
    IF p_content IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CONTENT: content cannot be null'
            USING ERRCODE = '22004';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'write');
    v_path := public.vexfs_path(p_workspace, p_inode);
    v_current := public.vexfs_read(p_workspace, v_path);
    RETURN public.vexfs_write(p_workspace, v_path, v_current || p_content);
END;
$$;

CREATE FUNCTION public.vexfs_mount_session_start(
    p_workspace text,
    p_session_id text)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
BEGIN
    IF p_session_id IS NULL OR p_session_id = '' OR octet_length(p_session_id) > 255 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SESSION: session id must be 1..255 bytes'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    -- Serialize session registration with workspace restore. A new gateway may
    -- start after a restore commits, but it cannot appear between the restore
    -- lease check and the tree replacement.
    PERFORM 1 FROM _vexfs.workspaces AS workspace
     WHERE workspace.workspace_id = v_workspace.workspace_id
     FOR UPDATE;
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;

    UPDATE _vexfs.handles AS handle
       SET state = CASE
               WHEN handle.dirty_generation > handle.published_generation
               THEN 'retained' ELSE 'closed' END,
           lease_until = CASE
               WHEN handle.dirty_generation > handle.published_generation
               THEN clock_timestamp() + interval '24 hours'
               ELSE clock_timestamp() END,
           updated_at = clock_timestamp()
     WHERE handle.workspace_id = v_workspace.workspace_id
       AND handle.state = 'open'
       AND handle.lease_until <= clock_timestamp()
       AND NOT EXISTS (
           SELECT 1 FROM _vexfs.mount_sessions AS mounted
            WHERE mounted.workspace_id = handle.workspace_id
              AND mounted.session_id = handle.session_id
              AND mounted.owner_oid = handle.owner_oid
              AND mounted.lease_until > clock_timestamp());
    PERFORM _vexfs.reap_orphan_inodes(v_workspace.workspace_id);

    INSERT INTO _vexfs.mount_sessions(
        workspace_id, session_id, owner_oid, owner_role, lease_until)
    VALUES (
        v_workspace.workspace_id, p_session_id, v_actor, session_user,
        clock_timestamp() + interval '30 seconds')
    ON CONFLICT (workspace_id, session_id) DO UPDATE
       SET lease_until = EXCLUDED.lease_until,
           heartbeat_at = clock_timestamp(),
           owner_role = session_user
     WHERE _vexfs.mount_sessions.owner_oid = v_actor;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_SESSION_CONFLICT: session id belongs to another role'
            USING ERRCODE = '55006';
    END IF;

    UPDATE _vexfs.handles
       SET state = 'open',
           session_id = p_session_id,
           lease_until = clock_timestamp() + interval '30 seconds',
           updated_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND owner_oid = v_actor
       AND state = 'retained'
       AND dirty_generation > published_generation
       AND NOT EXISTS (
           SELECT 1 FROM _vexfs.mount_sessions AS active
            WHERE active.workspace_id = v_workspace.workspace_id
              AND active.session_id = _vexfs.handles.session_id
              AND active.session_id <> p_session_id
              AND active.owner_oid = v_actor
              AND active.lease_until > clock_timestamp());
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_mount_session_heartbeat(
    p_workspace text,
    p_session_id text)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    -- Do not let an expired gateway revive while restore is using the same
    -- workspace lock as its global write fence.
    PERFORM 1 FROM _vexfs.workspaces AS workspace
     WHERE workspace.workspace_id = v_workspace.workspace_id
     FOR UPDATE;
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    UPDATE _vexfs.mount_sessions
       SET lease_until = clock_timestamp() + interval '30 seconds',
           heartbeat_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND session_id = p_session_id
       AND owner_oid = v_actor
       AND lease_until > clock_timestamp();
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: mount session is not active'
            USING ERRCODE = '55006';
    END IF;
    UPDATE _vexfs.handles
       SET lease_until = clock_timestamp() + interval '30 seconds',
           updated_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND session_id = p_session_id
       AND owner_oid = v_actor
       AND state = 'open';
    DELETE FROM _vexfs.handle_staging AS staging
     USING _vexfs.handles AS handle
     WHERE staging.handle_id = handle.handle_id
       AND handle.workspace_id = v_workspace.workspace_id
       AND handle.session_id = p_session_id
       AND handle.owner_oid = v_actor
       AND handle.state = 'closed';
    UPDATE _vexfs.file_locks
       SET lease_until = clock_timestamp() + interval '30 seconds'
     WHERE workspace_id = v_workspace.workspace_id
       AND session_id = p_session_id
       AND owner_oid = v_actor;
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_mount_session_end(
    p_workspace text,
    p_session_id text)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    UPDATE _vexfs.handles
       SET state = CASE WHEN dirty_generation > published_generation
                        THEN 'retained' ELSE 'closed' END,
           lease_until = CASE WHEN dirty_generation > published_generation
                              THEN clock_timestamp() + interval '24 hours'
                              ELSE clock_timestamp() END,
           updated_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND session_id = p_session_id
       AND owner_oid = v_actor
       AND state = 'open';
    PERFORM _vexfs.reap_orphan_inodes(v_workspace.workspace_id);
    DELETE FROM _vexfs.file_locks
     WHERE workspace_id = v_workspace.workspace_id
       AND session_id = p_session_id
       AND owner_oid = v_actor;
    DELETE FROM _vexfs.mount_sessions
     WHERE workspace_id = v_workspace.workspace_id
       AND session_id = p_session_id
       AND owner_oid = v_actor;
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_handle_open(
    p_workspace text,
    p_path text,
    p_flags text,
    p_request_id text,
    p_session_id text DEFAULT NULL)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_inode bigint;
    v_kind text;
    v_version bigint;
    v_size bigint := 0;
    v_manifest bigint;
    v_handle text;
    v_hash text;
    v_cached text;
    v_create boolean;
    v_truncate boolean;
    v_writable boolean;
    v_parent bigint;
    v_name text;
BEGIN
    IF p_flags IS NULL OR p_flags !~ '^[rwct]+$'
       OR (position('r' IN p_flags) = 0 AND position('w' IN p_flags) = 0) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_FLAGS: flags must use r,w,c,t and contain r or w'
            USING ERRCODE = '22023';
    END IF;
    v_writable := position('w' IN p_flags) > 0;
    v_create := position('c' IN p_flags) > 0;
    v_truncate := position('t' IN p_flags) > 0;
    IF (v_create OR v_truncate) AND NOT v_writable THEN
        RAISE EXCEPTION 'VEXFS_INVALID_FLAGS: create and truncate require write access'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(
        p_workspace, CASE WHEN v_writable THEN 'write' ELSE 'read' END);
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    IF p_session_id IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM _vexfs.mount_sessions AS mounted
         WHERE mounted.workspace_id = v_workspace.workspace_id
           AND mounted.session_id = p_session_id
           AND mounted.owner_oid = v_actor
           AND mounted.lease_until > clock_timestamp()) THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: mount session is not active'
            USING ERRCODE = '55006';
    END IF;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'workspace', p_workspace, 'path', p_path, 'flags', p_flags,
        'session', p_session_id));
    PERFORM _vexfs.lock_request(v_workspace.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_workspace.workspace_id, p_request_id, 'handle_open', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached;
    END IF;

    BEGIN
        v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
        SELECT i.kind, i.current_version, i.size_bytes
          INTO STRICT v_kind, v_version, v_size
          FROM _vexfs.inodes AS i
         WHERE i.workspace_id = v_workspace.workspace_id
           AND i.inode_id = v_inode AND i.live;
        IF v_kind <> 'file' THEN
            RAISE EXCEPTION 'VEXFS_NOT_REGULAR_FILE: %', p_path
                USING ERRCODE = '42809';
        END IF;
        PERFORM _vexfs.require_inode_permission(
            v_workspace.workspace_id, v_inode,
            CASE WHEN v_writable THEN 'write' ELSE 'read' END);
        IF v_version > 0 THEN
            SELECT storage.manifest_id INTO v_manifest
              FROM _vexfs.resolve_version_storage(
                  v_workspace.workspace_id, v_inode, v_version) AS storage;
        END IF;
    EXCEPTION WHEN SQLSTATE 'P0002' THEN
        IF NOT v_create THEN
            RAISE;
        END IF;
        SELECT rp.parent_inode, rp.entry_name INTO v_parent, v_name
          FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_path) AS rp;
        PERFORM _vexfs.require_inode_permission(
            v_workspace.workspace_id, v_parent, 'write');
        v_inode := NULL;
        v_version := 0;
        v_size := 0;
        v_manifest := NULL;
    END;
    v_handle := _vexfs.random_token('handle');
    INSERT INTO _vexfs.handles(
        handle_id, workspace_id, inode_id, path, flags, create_mode,
        owner_oid, owner_role, session_id, expected_version,
        dirty_generation)
    VALUES (
        v_handle, v_workspace.workspace_id, v_inode, p_path, p_flags,
        CASE WHEN v_inode IS NULL THEN 420 ELSE NULL END,
        v_actor, session_user, p_session_id, v_version,
        CASE WHEN v_truncate OR v_inode IS NULL THEN 1 ELSE 0 END);
    IF v_writable THEN
        INSERT INTO _vexfs.handle_staging(
            handle_id, generation, base_manifest_id, base_size,
            base_visible_size, logical_size, dirty_bytes)
        VALUES (
            v_handle,
            CASE WHEN v_truncate OR v_inode IS NULL THEN 1 ELSE 0 END,
            v_manifest,
            v_size,
            CASE WHEN v_truncate THEN 0 ELSE v_size END,
            CASE WHEN v_truncate THEN 0 ELSE v_size END,
            0);
    END IF;
    PERFORM _vexfs.replay_put(
        v_workspace.workspace_id, p_request_id, 'handle_open', v_hash, v_handle);
    RETURN v_handle;
END;
$$;

CREATE FUNCTION public.vexfs_handle_create(
    p_workspace text,
    p_path text,
    p_mode integer,
    p_request_id text,
    p_session_id text DEFAULT NULL)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_parent bigint;
    v_name text;
    v_handle text;
    v_hash text;
    v_cached text;
BEGIN
    IF p_mode < 0 OR p_mode > 4095 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_MODE: %', p_mode USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    IF p_session_id IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM _vexfs.mount_sessions AS mounted
         WHERE mounted.workspace_id = v_workspace.workspace_id
           AND mounted.session_id = p_session_id
           AND mounted.owner_oid = v_actor
           AND mounted.lease_until > clock_timestamp()) THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: mount session is not active'
            USING ERRCODE = '55006';
    END IF;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'workspace', p_workspace, 'path', p_path, 'mode', p_mode,
        'session', p_session_id));
    PERFORM _vexfs.lock_request(v_workspace.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_workspace.workspace_id, p_request_id, 'handle_create', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached;
    END IF;
    SELECT rp.parent_inode, rp.entry_name INTO v_parent, v_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_path) AS rp;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');
    IF EXISTS (
        SELECT 1 FROM _vexfs.dentries AS d
         WHERE d.workspace_id = v_workspace.workspace_id
           AND d.parent_inode = v_parent AND d.name = v_name) THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_EXISTS: %', p_path
            USING ERRCODE = '23505';
    END IF;
    PERFORM _vexfs.enforce_quota(v_workspace.workspace_id, NULL, 0);
    v_handle := _vexfs.random_token('handle');
    INSERT INTO _vexfs.handles(
        handle_id, workspace_id, path, flags, create_mode,
        owner_oid, owner_role, session_id, expected_version,
        dirty_generation)
    VALUES (
        v_handle, v_workspace.workspace_id, p_path, 'rwct', p_mode,
        v_actor, session_user, p_session_id, 0, 1);
    INSERT INTO _vexfs.handle_staging(
        handle_id, generation, base_manifest_id, base_size,
        base_visible_size, logical_size, dirty_bytes)
    VALUES (v_handle, 1, NULL, 0, 0, 0, 0);
    PERFORM _vexfs.replay_put(
        v_workspace.workspace_id, p_request_id, 'handle_create', v_hash, v_handle);
    RETURN v_handle;
END;
$$;

-- Filesystem gateways must return a usable inode from CREATE before the first
-- WRITE arrives.  Keep the older five-argument handle API isolated until
-- publish, but let this owned form expose a version-0 empty inode immediately.
-- The first publish records version 1 and the workspace commit atomically.
CREATE FUNCTION public.vexfs_handle_create(
    p_workspace text,
    p_path text,
    p_mode integer,
    p_uid bigint,
    p_gid bigint,
    p_request_id text,
    p_session_id text)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_parent bigint;
    v_name text;
    v_inode bigint;
    v_handle text;
    v_hash text;
    v_cached text;
BEGIN
    IF p_mode < 0 OR p_mode > 4095 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_MODE: %', p_mode
            USING ERRCODE = '22023';
    END IF;
    IF p_uid < 0 OR p_uid > 4294967295
       OR p_gid < 0 OR p_gid > 4294967295 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_OWNER: uid and gid must be within 0..4294967295'
            USING ERRCODE = '22023';
    END IF;
    IF p_session_id IS NULL OR p_session_id = '' THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SESSION: session id is required'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT role.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS role
     WHERE role.rolname = session_user;
    IF NOT EXISTS (
        SELECT 1 FROM _vexfs.mount_sessions AS mounted
         WHERE mounted.workspace_id = v_workspace.workspace_id
           AND mounted.session_id = p_session_id
           AND mounted.owner_oid = v_actor
           AND mounted.lease_until > clock_timestamp()) THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: mount session is not active'
            USING ERRCODE = '55006';
    END IF;

    v_hash := _vexfs.request_digest(jsonb_build_object(
        'workspace', p_workspace,
        'path', p_path,
        'mode', p_mode,
        'uid', p_uid,
        'gid', p_gid,
        'session', p_session_id));
    PERFORM _vexfs.lock_request(v_workspace.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_workspace.workspace_id, p_request_id, 'handle_create', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached;
    END IF;

    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    SELECT parent.parent_inode, parent.entry_name INTO v_parent, v_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_path) AS parent;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');
    IF EXISTS (
        SELECT 1 FROM _vexfs.dentries AS dentry
         WHERE dentry.workspace_id = v_workspace.workspace_id
           AND dentry.parent_inode = v_parent
           AND dentry.name = v_name) THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_EXISTS: %', p_path
            USING ERRCODE = '23505';
    END IF;
    PERFORM _vexfs.enforce_quota(v_workspace.workspace_id, NULL, 0);

    INSERT INTO _vexfs.inodes(
        workspace_id, kind, mode, owner_oid, owner_role, owner_principal,
        uid, gid, current_version, size_bytes)
    VALUES (
        v_workspace.workspace_id, 'file', p_mode,
        v_actor, session_user, session_user,
        p_uid, p_gid, 0, 0)
    RETURNING inode_id INTO v_inode;
    INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
    VALUES (v_workspace.workspace_id, v_parent, v_name, v_inode);
    PERFORM _vexfs.inherit_acl(v_workspace.workspace_id, v_parent, v_inode);
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = v_parent;

    v_handle := _vexfs.random_token('handle');
    INSERT INTO _vexfs.handles(
        handle_id, workspace_id, inode_id, path, flags, create_mode,
        owner_oid, owner_role, session_id, expected_version,
        dirty_generation)
    VALUES (
        v_handle, v_workspace.workspace_id, v_inode, p_path, 'rwct', p_mode,
        v_actor, session_user, p_session_id, 0, 1);
    INSERT INTO _vexfs.handle_staging(
        handle_id, generation, base_manifest_id, base_size,
        base_visible_size, logical_size, dirty_bytes)
    VALUES (v_handle, 1, NULL, 0, 0, 0, 0);
    PERFORM _vexfs.replay_put(
        v_workspace.workspace_id, p_request_id, 'handle_create', v_hash, v_handle);
    RETURN v_handle;
END;
$$;

-- NFS CREATE needs both a writable handle and attributes before it can reply.
-- Keeping these as two PL/pgSQL statements makes the STABLE stat query see the
-- new inode, while the client pays for only one database round trip.
CREATE FUNCTION public.vexfs_handle_create_stat(
    p_workspace text,
    p_path text,
    p_mode integer,
    p_uid bigint,
    p_gid bigint,
    p_request_id text,
    p_session_id text)
RETURNS TABLE(handle text, stat jsonb)
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle text;
BEGIN
    v_handle := public.vexfs_handle_create(
        p_workspace, p_path, p_mode, p_uid, p_gid,
        p_request_id, p_session_id);
    RETURN QUERY
    SELECT v_handle, public.vexfs_stat(p_workspace, p_path);
END;
$$;

CREATE FUNCTION public.vexfs_handle_stage_write(
    p_handle text,
    p_offset bigint,
    p_content bytea,
    p_request_id text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle _vexfs.handles%ROWTYPE;
    v_staging _vexfs.handle_staging%ROWTYPE;
    v_hash text;
    v_cached text;
    v_generation bigint;
    v_logical_size bigint;
    v_patch_offset integer := 0;
    v_absolute bigint;
    v_chunk_no integer;
    v_chunk_start bigint;
    v_within integer;
    v_write_size integer;
    v_target_size integer;
    v_chunk bytea;
BEGIN
    IF p_content IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CONTENT: content cannot be null'
            USING ERRCODE = '22004';
    END IF;
    IF p_offset < 0 OR p_offset > 134217728
       OR p_offset + octet_length(p_content) > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: staged write is outside 0..128 MiB'
            USING ERRCODE = '22023';
    END IF;
    SELECT * INTO v_handle FROM _vexfs.require_handle(p_handle, 'write');
    IF position('w' IN v_handle.flags) = 0 THEN
        RAISE EXCEPTION 'VEXFS_READ_ONLY_HANDLE: %', p_handle
            USING ERRCODE = '25006';
    END IF;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'handle', p_handle, 'offset', p_offset,
        'content_size', octet_length(p_content),
        'content_checksum', encode(pg_catalog.sha256(p_content), 'hex')));
    PERFORM _vexfs.lock_request(v_handle.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_handle.workspace_id, p_request_id, 'handle_stage_write', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::bigint;
    END IF;
    SELECT * INTO v_staging
      FROM _vexfs.handle_staging AS staging
     WHERE staging.handle_id = p_handle
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_STAGING_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    IF octet_length(p_content) = 0 THEN
        PERFORM _vexfs.replay_put(
            v_handle.workspace_id, p_request_id, 'handle_stage_write',
            v_hash, v_handle.dirty_generation::text);
        RETURN v_handle.dirty_generation;
    END IF;
    v_logical_size := greatest(
        v_staging.logical_size, p_offset + octet_length(p_content));
    PERFORM _vexfs.enforce_quota(
        v_handle.workspace_id, v_handle.inode_id, v_logical_size);

    WHILE v_patch_offset < octet_length(p_content) LOOP
        v_absolute := p_offset + v_patch_offset;
        v_chunk_no := (v_absolute / 65536)::integer;
        v_chunk_start := v_chunk_no::bigint * 65536;
        v_within := (v_absolute - v_chunk_start)::integer;
        v_write_size := least(
            octet_length(p_content) - v_patch_offset,
            65536 - v_within);
        v_target_size := least(
            65536, v_logical_size - v_chunk_start)::integer;
        IF v_within = 0 AND v_write_size = v_target_size THEN
            v_chunk := substring(
                p_content FROM v_patch_offset + 1 FOR v_write_size);
        ELSE
            v_chunk := _vexfs.read_staging_chunk(
                p_handle, v_chunk_no, v_target_size);
            v_chunk := _vexfs.overlay_bytes(
                v_chunk,
                v_within,
                substring(p_content FROM v_patch_offset + 1 FOR v_write_size));
        END IF;
        INSERT INTO _vexfs.handle_staging_chunks(handle_id, chunk_no, content)
        VALUES (p_handle, v_chunk_no, v_chunk)
        ON CONFLICT (handle_id, chunk_no) DO UPDATE
            SET content = excluded.content;
        v_patch_offset := v_patch_offset + v_write_size;
    END LOOP;

    v_generation := v_handle.dirty_generation + 1;
    UPDATE _vexfs.handle_staging
       SET generation = v_generation,
           logical_size = v_logical_size,
           dirty_bytes = (
               SELECT coalesce(sum(octet_length(dirty.content)), 0)
                 FROM _vexfs.handle_staging_chunks AS dirty
                WHERE dirty.handle_id = p_handle),
           updated_at = clock_timestamp()
     WHERE handle_id = p_handle;
    UPDATE _vexfs.handles
       SET dirty_generation = v_generation,
           lease_until = clock_timestamp() + interval '30 seconds',
           updated_at = clock_timestamp()
     WHERE handle_id = p_handle;
    PERFORM _vexfs.replay_put(
        v_handle.workspace_id, p_request_id, 'handle_stage_write',
        v_hash, v_generation::text);
    RETURN v_generation;
END;
$$;

CREATE FUNCTION public.vexfs_handle_truncate(
    p_handle text,
    p_size bigint,
    p_request_id text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle _vexfs.handles%ROWTYPE;
    v_staging _vexfs.handle_staging%ROWTYPE;
    v_hash text;
    v_cached text;
    v_generation bigint;
    v_remainder integer;
    v_boundary_chunk integer;
    v_current_chunk_size integer;
    v_chunk bytea;
BEGIN
    IF p_size < 0 OR p_size > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SIZE: staged file must be 0..128 MiB'
            USING ERRCODE = '22023';
    END IF;
    SELECT * INTO v_handle FROM _vexfs.require_handle(p_handle, 'write');
    IF position('w' IN v_handle.flags) = 0 THEN
        RAISE EXCEPTION 'VEXFS_READ_ONLY_HANDLE: %', p_handle
            USING ERRCODE = '25006';
    END IF;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'handle', p_handle, 'size', p_size));
    PERFORM _vexfs.lock_request(v_handle.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_handle.workspace_id, p_request_id, 'handle_truncate', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::bigint;
    END IF;
    SELECT * INTO v_staging
      FROM _vexfs.handle_staging AS staging
     WHERE staging.handle_id = p_handle
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_STAGING_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    IF p_size = v_staging.logical_size THEN
        PERFORM _vexfs.replay_put(
            v_handle.workspace_id, p_request_id, 'handle_truncate',
            v_hash, v_handle.dirty_generation::text);
        RETURN v_handle.dirty_generation;
    END IF;
    PERFORM _vexfs.enforce_quota(
        v_handle.workspace_id, v_handle.inode_id, p_size);

    IF p_size < v_staging.logical_size THEN
        v_remainder := (p_size % 65536)::integer;
        v_boundary_chunk := (p_size / 65536)::integer;
        IF v_remainder > 0 THEN
            v_current_chunk_size := least(
                65536,
                v_staging.logical_size - v_boundary_chunk::bigint * 65536)::integer;
            v_chunk := _vexfs.read_staging_chunk(
                p_handle, v_boundary_chunk, v_current_chunk_size);
            v_chunk := substring(v_chunk FROM 1 FOR v_remainder);
            INSERT INTO _vexfs.handle_staging_chunks(handle_id, chunk_no, content)
            VALUES (p_handle, v_boundary_chunk, v_chunk)
            ON CONFLICT (handle_id, chunk_no) DO UPDATE
                SET content = excluded.content;
        END IF;
        DELETE FROM _vexfs.handle_staging_chunks AS dirty
         WHERE dirty.handle_id = p_handle
           AND dirty.chunk_no >= ((p_size + 65535) / 65536)::integer;
    END IF;

    v_generation := v_handle.dirty_generation + 1;
    UPDATE _vexfs.handle_staging
       SET generation = v_generation,
           base_visible_size = least(base_visible_size, p_size),
           logical_size = p_size,
           dirty_bytes = (
               SELECT coalesce(sum(octet_length(dirty.content)), 0)
                 FROM _vexfs.handle_staging_chunks AS dirty
                WHERE dirty.handle_id = p_handle),
           updated_at = clock_timestamp()
     WHERE handle_id = p_handle;
    UPDATE _vexfs.handles
       SET dirty_generation = v_generation,
           lease_until = clock_timestamp() + interval '30 seconds',
           updated_at = clock_timestamp()
     WHERE handle_id = p_handle;
    PERFORM _vexfs.replay_put(
        v_handle.workspace_id, p_request_id, 'handle_truncate',
        v_hash, v_generation::text);
    RETURN v_generation;
END;
$$;

CREATE FUNCTION public.vexfs_handle_read(
    p_handle text,
    p_offset bigint,
    p_length bigint)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle _vexfs.handles%ROWTYPE;
    v_version bigint;
BEGIN
    IF p_offset < 0 OR p_length < 0
       OR p_offset > 134217728 OR p_length > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: offset and length must be within 128 MiB'
            USING ERRCODE = '22023';
    END IF;
    SELECT * INTO v_handle FROM _vexfs.require_handle(p_handle, 'read');
    IF position('r' IN v_handle.flags) = 0 THEN
        RAISE EXCEPTION 'VEXFS_WRITE_ONLY_HANDLE: %', p_handle
            USING ERRCODE = '42501';
    END IF;
    IF position('w' IN v_handle.flags) = 0 AND v_handle.inode_id IS NOT NULL THEN
        SELECT i.current_version INTO STRICT v_version
          FROM _vexfs.inodes AS i
         WHERE i.workspace_id = v_handle.workspace_id
           AND i.inode_id = v_handle.inode_id;
        IF v_version = 0 THEN
            RETURN ''::bytea;
        END IF;
        RETURN _vexfs.read_version_range(
            v_handle.workspace_id, v_handle.inode_id, v_version,
            p_offset, p_length);
    END IF;
    RETURN _vexfs.read_staging_range(p_handle, p_offset, p_length);
END;
$$;

CREATE FUNCTION public.vexfs_handle_publish(
    p_handle text,
    p_generation bigint,
    p_durability text,
    p_request_id text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle _vexfs.handles%ROWTYPE;
    v_staging _vexfs.handle_staging%ROWTYPE;
    v_hash text;
    v_cached text;
    v_current_version bigint;
    v_path text;
    v_version bigint;
    v_inode bigint;
    v_parent bigint;
    v_name text;
    v_manifest bigint;
    v_checksum text;
    v_commit bigint;
    v_principal_oid oid;
    v_operation text;
BEGIN
    IF p_durability NOT IN ('none', 'data', 'full') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_DURABILITY: use none, data, or full'
            USING ERRCODE = '22023';
    END IF;
    SELECT * INTO v_handle FROM _vexfs.require_handle(p_handle, 'write');
    IF position('w' IN v_handle.flags) = 0 THEN
        RAISE EXCEPTION 'VEXFS_READ_ONLY_HANDLE: %', p_handle
            USING ERRCODE = '25006';
    END IF;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'handle', p_handle, 'generation', p_generation,
        'durability', p_durability));
    PERFORM _vexfs.lock_request(v_handle.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_handle.workspace_id, p_request_id, 'handle_publish', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::bigint;
    END IF;
    IF p_generation <= v_handle.published_generation THEN
        v_version := v_handle.expected_version;
        PERFORM _vexfs.replay_put(
            v_handle.workspace_id, p_request_id, 'handle_publish',
            v_hash, v_version::text);
        RETURN v_version;
    END IF;
    IF p_generation <> v_handle.dirty_generation THEN
        RAISE EXCEPTION 'VEXFS_STALE_GENERATION: expected %, received %',
            v_handle.dirty_generation, p_generation
            USING ERRCODE = '40001';
    END IF;
    SELECT * INTO v_staging
      FROM _vexfs.handle_staging AS staging
     WHERE staging.handle_id = p_handle
     FOR UPDATE;
    IF NOT FOUND OR v_staging.generation <> p_generation THEN
        RAISE EXCEPTION 'VEXFS_STALE_GENERATION: staging generation does not match %',
            p_generation USING ERRCODE = '40001';
    END IF;
    -- Existing/eager-created inodes can build their manifest before taking the
    -- workspace row lock. The staging lock freezes this exact generation while
    -- unchanged chunks are linked and only dirty chunks are materialized.
    IF v_handle.inode_id IS NOT NULL THEN
        v_manifest := _vexfs.store_staging_manifest(
            p_handle, v_handle.inode_id);
    END IF;
    PERFORM pg_catalog.set_config(
        'synchronous_commit',
        CASE p_durability WHEN 'none' THEN 'off'
                          WHEN 'data' THEN 'local'
                          ELSE 'on' END,
        true);
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_handle.workspace_id FOR UPDATE;
    SELECT role.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS role
     WHERE role.rolname = session_user;
    IF v_handle.inode_id IS NULL THEN
        SELECT rp.parent_inode, rp.entry_name INTO v_parent, v_name
          FROM _vexfs.resolve_parent(
              v_handle.workspace_id, v_handle.path) AS rp;
        PERFORM _vexfs.require_inode_permission(
            v_handle.workspace_id, v_parent, 'write');
        IF EXISTS (
            SELECT 1
              FROM _vexfs.dentries AS d
             WHERE d.workspace_id = v_handle.workspace_id
               AND d.parent_inode = v_parent
               AND d.name = v_name) THEN
            RAISE EXCEPTION 'VEXFS_VERSION_CONFLICT: create destination now exists'
                USING ERRCODE = '40001';
        END IF;
        PERFORM _vexfs.enforce_quota(
            v_handle.workspace_id, NULL, v_staging.logical_size);
        v_current_version := 0;
        v_version := 1;
        v_path := v_handle.path;
        INSERT INTO _vexfs.inodes(
            workspace_id, kind, mode, owner_oid, owner_role, owner_principal,
            current_version, size_bytes)
        VALUES (
            v_handle.workspace_id, 'file', coalesce(v_handle.create_mode, 420),
            v_principal_oid, session_user, session_user,
            v_version, v_staging.logical_size)
        RETURNING inode_id INTO v_inode;
        INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
        VALUES (v_handle.workspace_id, v_parent, v_name, v_inode);
        PERFORM _vexfs.inherit_acl(v_handle.workspace_id, v_parent, v_inode);
        UPDATE _vexfs.inodes
           SET modified_at = clock_timestamp(),
               changed_at = clock_timestamp()
         WHERE workspace_id = v_handle.workspace_id
           AND inode_id = v_parent;
        v_manifest := _vexfs.store_staging_manifest(p_handle, v_inode);
    ELSE
        v_inode := v_handle.inode_id;
        SELECT i.current_version INTO v_current_version
          FROM _vexfs.inodes AS i
         WHERE i.workspace_id = v_handle.workspace_id
           AND i.inode_id = v_inode
           AND i.live AND i.kind = 'file'
         FOR UPDATE;
        IF NOT FOUND OR v_current_version <> v_handle.expected_version THEN
            RAISE EXCEPTION 'VEXFS_VERSION_CONFLICT: file changed after handle open'
                USING ERRCODE = '40001';
        END IF;
        PERFORM _vexfs.require_inode_permission(
            v_handle.workspace_id, v_inode, 'write');
        PERFORM _vexfs.enforce_quota(
            v_handle.workspace_id, v_inode, v_staging.logical_size);
        SELECT coalesce(max(version.version_no), 0) + 1
          INTO v_version
          FROM _vexfs.file_versions AS version
         WHERE version.workspace_id = v_handle.workspace_id
           AND version.inode_id = v_inode;
        v_path := _vexfs.path_for_inode(v_handle.workspace_id, v_inode);
        UPDATE _vexfs.inodes
           SET current_version = v_version,
               size_bytes = v_staging.logical_size,
               modified_at = clock_timestamp(),
               changed_at = clock_timestamp()
         WHERE workspace_id = v_handle.workspace_id
           AND inode_id = v_inode;
        IF v_path IS NOT NULL THEN
            SELECT parent.parent_inode INTO v_parent
              FROM _vexfs.resolve_parent(
                  v_handle.workspace_id, v_path) AS parent;
            UPDATE _vexfs.inodes
               SET modified_at = clock_timestamp(),
                   changed_at = clock_timestamp()
             WHERE workspace_id = v_handle.workspace_id
               AND inode_id = v_parent;
        END IF;
    END IF;
    SELECT manifest.checksum INTO STRICT v_checksum
      FROM _vexfs.manifests AS manifest
     WHERE manifest.manifest_id = v_manifest;
    v_operation := CASE WHEN v_path IS NULL
        THEN 'handle_write_orphan' ELSE 'write' END;
    v_commit := _vexfs.record_commit(
        v_handle.workspace_id, v_operation,
        coalesce(v_path, v_handle.path), v_inode,
        jsonb_build_object(
            'before_version', v_current_version,
            'after_version', v_version,
            'unlinked', v_path IS NULL));
    INSERT INTO _vexfs.file_versions(
        workspace_id, inode_id, version_no, commit_no, manifest_id,
        size_bytes, checksum, created_by_oid, created_by)
    VALUES (
        v_handle.workspace_id, v_inode, v_version, v_commit, v_manifest,
        v_staging.logical_size, v_checksum, v_principal_oid, session_user);
    UPDATE _vexfs.handles
       SET inode_id = v_inode,
           path = coalesce(v_path, _vexfs.handles.path),
           expected_version = v_version,
           published_generation = p_generation,
           lease_until = clock_timestamp() + interval '30 seconds',
           updated_at = clock_timestamp()
     WHERE handle_id = p_handle;
    UPDATE _vexfs.handle_staging
       SET generation = p_generation,
           base_manifest_id = v_manifest,
           base_size = v_staging.logical_size,
           base_visible_size = v_staging.logical_size,
           logical_size = v_staging.logical_size,
           dirty_bytes = 0,
           updated_at = clock_timestamp()
     WHERE handle_id = p_handle;
    DELETE FROM _vexfs.handle_staging_chunks
     WHERE handle_id = p_handle;
    PERFORM _vexfs.replay_put(
        v_handle.workspace_id, p_request_id, 'handle_publish',
        v_hash, v_version::text);
    RETURN v_version;
END;
$$;

CREATE FUNCTION public.vexfs_handle_append(
    p_handle text,
    p_content bytea,
    p_request_id text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle _vexfs.handles%ROWTYPE;
    v_staging _vexfs.handle_staging%ROWTYPE;
    v_hash text;
    v_cached text;
    v_current_version bigint;
    v_current_size bigint;
    v_manifest bigint;
    v_generation bigint;
    v_version bigint;
    v_publish_request text;
BEGIN
    IF p_content IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CONTENT: content cannot be null'
            USING ERRCODE = '22004';
    END IF;
    SELECT * INTO v_handle FROM _vexfs.require_handle(p_handle, 'write');
    IF position('w' IN v_handle.flags) = 0 THEN
        RAISE EXCEPTION 'VEXFS_READ_ONLY_HANDLE: %', p_handle
            USING ERRCODE = '25006';
    END IF;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'handle', p_handle,
        'content_size', octet_length(p_content),
        'content_checksum', encode(pg_catalog.sha256(p_content), 'hex')));
    PERFORM _vexfs.lock_request(v_handle.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_handle.workspace_id, p_request_id, 'handle_append', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::bigint;
    END IF;
    SELECT * INTO v_staging
      FROM _vexfs.handle_staging AS staging
     WHERE staging.handle_id = p_handle
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_STAGING_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    IF v_handle.inode_id IS NULL THEN
        v_current_size := v_staging.logical_size;
    ELSE
        IF v_handle.dirty_generation <> v_handle.published_generation THEN
            RAISE EXCEPTION 'VEXFS_DIRTY_HANDLE: append requires a clean handle'
                USING ERRCODE = '55006';
        END IF;
        SELECT i.current_version, i.size_bytes
          INTO v_current_version, v_current_size
          FROM _vexfs.inodes AS i
         WHERE i.workspace_id = v_handle.workspace_id
           AND i.inode_id = v_handle.inode_id
           AND i.live AND i.kind = 'file'
         FOR UPDATE;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'VEXFS_VERSION_CONFLICT: open inode is unavailable'
                USING ERRCODE = '40001';
        END IF;
        IF v_current_version > 0 THEN
            SELECT storage.manifest_id INTO v_manifest
              FROM _vexfs.resolve_version_storage(
                  v_handle.workspace_id, v_handle.inode_id,
                  v_current_version) AS storage;
        ELSE
            v_manifest := NULL;
        END IF;
        UPDATE _vexfs.handles
           SET expected_version = v_current_version,
               updated_at = clock_timestamp()
         WHERE handle_id = p_handle;
        DELETE FROM _vexfs.handle_staging_chunks
         WHERE handle_id = p_handle;
        UPDATE _vexfs.handle_staging
           SET generation = v_handle.published_generation,
               base_manifest_id = v_manifest,
               base_size = v_current_size,
               base_visible_size = v_current_size,
               logical_size = v_current_size,
               dirty_bytes = 0,
               updated_at = clock_timestamp()
         WHERE handle_id = p_handle;
    END IF;
    v_generation := public.vexfs_handle_stage_write(
        p_handle,
        v_current_size,
        p_content,
        'append-stage:' || coalesce(
            nullif(p_request_id, ''), _vexfs.random_token('request')));
    v_publish_request := 'append-publish:' || coalesce(
        nullif(p_request_id, ''), _vexfs.random_token('request'));
    v_version := public.vexfs_handle_publish(
        p_handle, v_generation, 'data', v_publish_request);
    PERFORM _vexfs.replay_put(
        v_handle.workspace_id, p_request_id, 'handle_append',
        v_hash, v_version::text);
    RETURN v_version;
END;
$$;

CREATE FUNCTION public.vexfs_handle_publish_close(
    p_handle text,
    p_generation bigint,
    p_durability text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle _vexfs.handles%ROWTYPE;
    v_actor oid;
    v_version bigint;
BEGIN
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    SELECT * INTO v_handle
      FROM _vexfs.handles AS handle
     WHERE handle.handle_id = p_handle
       AND handle.owner_oid = v_actor
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_HANDLE_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    IF v_handle.state = 'closed' THEN
        IF p_generation <= v_handle.published_generation THEN
            RETURN v_handle.expected_version;
        END IF;
        RAISE EXCEPTION 'VEXFS_HANDLE_CLOSED: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    v_version := public.vexfs_handle_publish(
        p_handle, p_generation, p_durability,
        'publish-close:' || p_handle || ':' || p_generation::text || ':' || p_durability);
    UPDATE _vexfs.handles
       SET state = 'closed',
           lease_until = clock_timestamp(),
           updated_at = clock_timestamp()
     WHERE handle_id = p_handle;
    DELETE FROM _vexfs.handle_staging
     WHERE handle_id = p_handle;
    PERFORM _vexfs.reap_orphan_inodes(v_handle.workspace_id);
    RETURN v_version;
END;
$$;

-- Publish every dirty handle owned by one mount session in a single database
-- transaction.  The NFS idle flusher uses this to turn many small UNSTABLE
-- writes into durable file versions without one libpq round trip per file.
CREATE FUNCTION public.vexfs_mount_publish_close_all(
    p_workspace text,
    p_session_id text,
    p_durability text DEFAULT 'none',
    p_limit bigint DEFAULT 0)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_handle record;
    v_count bigint := 0;
    v_pending bigint;
BEGIN
    IF p_durability NOT IN ('none', 'data', 'full') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_DURABILITY: use none, data, or full'
            USING ERRCODE = '22023';
    END IF;
    IF p_session_id IS NULL OR p_session_id = '' THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SESSION: session id is required'
            USING ERRCODE = '22023';
    END IF;
    IF p_limit < 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_LIMIT: limit must be zero or positive'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT role.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS role
     WHERE role.rolname = session_user;
    IF NOT EXISTS (
        SELECT 1 FROM _vexfs.mount_sessions AS mounted
         WHERE mounted.workspace_id = v_workspace.workspace_id
           AND mounted.session_id = p_session_id
           AND mounted.owner_oid = v_actor
           AND mounted.lease_until > clock_timestamp()) THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: mount session is not active'
            USING ERRCODE = '55006';
    END IF;

    SELECT count(*) INTO v_pending
      FROM (
          SELECT 1
            FROM _vexfs.handles AS handle
           WHERE handle.workspace_id = v_workspace.workspace_id
             AND handle.owner_oid = v_actor
             AND handle.session_id = p_session_id
             AND handle.state = 'open'
             AND handle.dirty_generation > handle.published_generation
           ORDER BY handle.handle_id
           LIMIT NULLIF(p_limit, 0)) AS pending;
    IF v_pending = 0 THEN
        RETURN 0;
    END IF;
    PERFORM _vexfs.begin_commit_batch(
        v_workspace.workspace_id,
        'publish_batch',
        '/',
        v_workspace.root_inode,
        jsonb_build_object(
            'before_version', NULL,
            'after_version', NULL,
            'item_count', v_pending,
            'session', p_session_id,
            'durability', p_durability));

    FOR v_handle IN
        SELECT handle.handle_id, handle.dirty_generation
          FROM _vexfs.handles AS handle
         WHERE handle.workspace_id = v_workspace.workspace_id
           AND handle.owner_oid = v_actor
           AND handle.session_id = p_session_id
           AND handle.state = 'open'
           AND handle.dirty_generation > handle.published_generation
         ORDER BY handle.handle_id
         LIMIT NULLIF(p_limit, 0)
    LOOP
        PERFORM public.vexfs_handle_publish_close(
            v_handle.handle_id, v_handle.dirty_generation, p_durability);
        v_count := v_count + 1;
    END LOOP;
    PERFORM _vexfs.end_commit_batch(v_workspace.workspace_id);
    RETURN v_count;
END;
$$;

-- Publish an exact generation snapshot selected by a SQL caller. This helper
-- remains an atomic all-or-nothing batch for direct SQL compatibility. The NFS
-- gateway uses the dedicated single-file publisher connection path instead,
-- so each file releases the workspace row lock before the next publish begins.
CREATE FUNCTION public.vexfs_mount_publish_close_claimed(
    p_workspace text,
    p_session_id text,
    p_durability text,
    p_claims jsonb)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_claim jsonb;
    v_handle text;
    v_generation bigint;
    v_version bigint;
    v_owned _vexfs.handles%ROWTYPE;
    v_seen text[] := ARRAY[]::text[];
    v_handles text[] := ARRAY[]::text[];
    v_generations bigint[] := ARRAY[]::bigint[];
    v_versions bigint[] := ARRAY[]::bigint[];
    v_dirty boolean[] := ARRAY[]::boolean[];
    v_dirty_count integer := 0;
    v_index integer;
    v_result jsonb := '[]'::jsonb;
BEGIN
    IF p_durability NOT IN ('none', 'data', 'full') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_DURABILITY: use none, data, or full'
            USING ERRCODE = '22023';
    END IF;
    IF p_session_id IS NULL OR p_session_id = '' THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SESSION: session id is required'
            USING ERRCODE = '22023';
    END IF;
    IF p_claims IS NULL OR jsonb_typeof(p_claims) <> 'array'
       OR jsonb_array_length(p_claims) < 1
       OR jsonb_array_length(p_claims) > 64 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_BATCH: claims must contain 1..64 handles'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT role.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS role
     WHERE role.rolname = session_user;
    IF NOT EXISTS (
        SELECT 1 FROM _vexfs.mount_sessions AS mounted
         WHERE mounted.workspace_id = v_workspace.workspace_id
           AND mounted.session_id = p_session_id
           AND mounted.owner_oid = v_actor
           AND mounted.lease_until > clock_timestamp()) THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: mount session is not active'
            USING ERRCODE = '55006';
    END IF;

    FOR v_claim IN SELECT value FROM jsonb_array_elements(p_claims) LOOP
        IF jsonb_typeof(v_claim) <> 'object'
           OR NOT v_claim ? 'handle'
           OR NOT v_claim ? 'generation' THEN
            RAISE EXCEPTION 'VEXFS_INVALID_BATCH: claim requires handle and generation'
                USING ERRCODE = '22023';
        END IF;
        v_handle := v_claim->>'handle';
        BEGIN
            v_generation := (v_claim->>'generation')::bigint;
        EXCEPTION WHEN invalid_text_representation OR numeric_value_out_of_range THEN
            RAISE EXCEPTION 'VEXFS_INVALID_BATCH: claim generation is invalid'
                USING ERRCODE = '22023';
        END;
        IF v_handle IS NULL OR v_handle = '' OR v_generation < 0
           OR v_handle = ANY(v_seen) THEN
            RAISE EXCEPTION 'VEXFS_INVALID_BATCH: claim is empty or duplicated'
                USING ERRCODE = '22023';
        END IF;
        SELECT * INTO v_owned
          FROM _vexfs.handles AS handle
         WHERE handle.handle_id = v_handle
           AND handle.workspace_id = v_workspace.workspace_id
           AND handle.owner_oid = v_actor
           AND handle.session_id = p_session_id;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'VEXFS_HANDLE_NOT_FOUND: claimed handle is unavailable'
                USING ERRCODE = 'P0002';
        END IF;
        IF v_owned.state = 'closed'
           AND v_generation <= v_owned.published_generation THEN
            v_seen := array_append(v_seen, v_handle);
            v_handles := array_append(v_handles, v_handle);
            v_generations := array_append(v_generations, v_generation);
            v_versions := array_append(v_versions, v_owned.expected_version);
            v_dirty := array_append(v_dirty, false);
            CONTINUE;
        END IF;
        IF v_owned.state <> 'open'
           OR v_owned.dirty_generation <> v_generation
           OR v_owned.dirty_generation <= v_owned.published_generation THEN
            RAISE EXCEPTION 'VEXFS_STALE_GENERATION: claimed handle changed before publish'
                USING ERRCODE = '40001';
        END IF;
        v_seen := array_append(v_seen, v_handle);
        v_handles := array_append(v_handles, v_handle);
        v_generations := array_append(v_generations, v_generation);
        v_versions := array_append(v_versions, NULL::bigint);
        v_dirty := array_append(v_dirty, true);
        v_dirty_count := v_dirty_count + 1;
    END LOOP;

    IF v_dirty_count > 0 THEN
        PERFORM _vexfs.begin_commit_batch(
            v_workspace.workspace_id,
            'publish_batch',
            '/',
            v_workspace.root_inode,
            jsonb_build_object(
                'before_version', NULL,
                'after_version', NULL,
                'item_count', v_dirty_count,
                'session', p_session_id,
                'durability', p_durability));
    END IF;
    FOR v_index IN 1..coalesce(array_length(v_handles, 1), 0) LOOP
        IF v_dirty[v_index] THEN
            v_versions[v_index] := public.vexfs_handle_publish_close(
                v_handles[v_index], v_generations[v_index], p_durability);
        END IF;
        v_result := v_result || jsonb_build_array(jsonb_build_object(
            'handle', v_handles[v_index],
            'generation', v_generations[v_index],
            'version', v_versions[v_index]));
    END LOOP;
    IF v_dirty_count > 0 THEN
        PERFORM _vexfs.end_commit_batch(v_workspace.workspace_id);
    END IF;
    RETURN v_result;
END;
$$;

-- NFS FILE_SYNC writes must be durable before the RPC returns. Apply the range
-- directly to the current manifest so the remote mount pays for one libpq
-- round trip and PG only materializes the affected 64 KiB chunks.
CREATE FUNCTION public.vexfs_write_range(
    p_workspace text,
    p_path text,
    p_offset bigint,
    p_content bytea,
    p_request_id text,
    p_session_id text,
    p_durability text DEFAULT 'full')
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_hash text;
    v_cached text;
    v_inode bigint;
    v_kind text;
    v_current_version bigint;
    v_version bigint;
    v_previous_manifest bigint;
    v_previous_size bigint;
    v_manifest bigint;
    v_checksum text;
    v_new_size bigint;
    v_commit bigint;
    v_parent bigint;
BEGIN
    IF p_content IS NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CONTENT: content cannot be null'
            USING ERRCODE = '22004';
    END IF;
    IF p_offset < 0 OR p_offset > 134217728
       OR p_offset + octet_length(p_content) > 134217728 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RANGE: write exceeds 128 MiB'
            USING ERRCODE = '22023';
    END IF;
    IF p_durability NOT IN ('none', 'data', 'full') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_DURABILITY: use none, data, or full'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT role.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS role
     WHERE role.rolname = session_user;
    IF p_session_id IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM _vexfs.mount_sessions AS mounted
         WHERE mounted.workspace_id = v_workspace.workspace_id
           AND mounted.session_id = p_session_id
           AND mounted.owner_oid = v_actor
           AND mounted.lease_until > clock_timestamp()) THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: mount session is not active'
            USING ERRCODE = '55006';
    END IF;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'path', p_path,
        'offset', p_offset,
        'content_size', octet_length(p_content),
        'content_checksum', encode(pg_catalog.sha256(p_content), 'hex'),
        'session', p_session_id,
        'durability', p_durability));
    PERFORM _vexfs.lock_request(v_workspace.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_workspace.workspace_id, p_request_id, 'write_range', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::bigint;
    END IF;

    PERFORM pg_catalog.set_config(
        'synchronous_commit',
        CASE p_durability WHEN 'none' THEN 'off'
                          WHEN 'data' THEN 'local'
                          ELSE 'on' END,
        true);
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    SELECT inode.kind, inode.current_version
      INTO STRICT v_kind, v_current_version
      FROM _vexfs.inodes AS inode
     WHERE inode.workspace_id = v_workspace.workspace_id
       AND inode.inode_id = v_inode
       AND inode.live
     FOR UPDATE;
    IF v_kind <> 'file' THEN
        RAISE EXCEPTION 'VEXFS_NOT_REGULAR_FILE: %', p_path
            USING ERRCODE = '42809';
    END IF;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'write');
    SELECT parent.parent_inode INTO v_parent
      FROM _vexfs.resolve_parent(
          v_workspace.workspace_id, p_path) AS parent;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');

    -- POSIX zero-byte writes do not change file contents or create a version.
    IF octet_length(p_content) = 0 THEN
        PERFORM _vexfs.replay_put(
            v_workspace.workspace_id, p_request_id, 'write_range', v_hash,
            v_current_version::text);
        RETURN v_current_version;
    END IF;

    SELECT storage.manifest_id, storage.size_bytes
      INTO v_previous_manifest, v_previous_size
      FROM _vexfs.resolve_version_storage(
          v_workspace.workspace_id, v_inode, v_current_version) AS storage;
    v_new_size := greatest(v_previous_size, p_offset + octet_length(p_content));
    PERFORM _vexfs.enforce_quota(
        v_workspace.workspace_id, v_inode, v_new_size);
    SELECT coalesce(max(version.version_no), 0) + 1
      INTO v_version
      FROM _vexfs.file_versions AS version
     WHERE version.workspace_id = v_workspace.workspace_id
       AND version.inode_id = v_inode;
    v_manifest := _vexfs.store_manifest_range(
        v_workspace.workspace_id, v_inode, v_previous_manifest,
        v_previous_size, p_offset, p_content);
    SELECT manifest.checksum INTO STRICT v_checksum
      FROM _vexfs.manifests AS manifest
     WHERE manifest.manifest_id = v_manifest;
    v_commit := _vexfs.record_commit(
        v_workspace.workspace_id, 'write_range', p_path, v_inode,
        jsonb_build_object(
            'before_version', v_current_version,
            'after_version', v_version,
            'offset', p_offset,
            'length', octet_length(p_content)));
    INSERT INTO _vexfs.file_versions(
        workspace_id, inode_id, version_no, commit_no, manifest_id,
        size_bytes, checksum, created_by_oid, created_by)
    VALUES (
        v_workspace.workspace_id, v_inode, v_version, v_commit, v_manifest,
        v_new_size, v_checksum, v_actor, session_user);
    UPDATE _vexfs.inodes
       SET current_version = v_version,
           size_bytes = v_new_size,
           modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = v_inode;
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = v_parent;
    PERFORM _vexfs.replay_put(
        v_workspace.workspace_id, p_request_id, 'write_range', v_hash,
        v_version::text);
    RETURN v_version;
END;
$$;

CREATE FUNCTION public.vexfs_handle_close(
    p_handle text,
    p_retain_unpublished boolean,
    p_request_id text)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_handle _vexfs.handles%ROWTYPE;
    v_actor oid;
    v_hash text;
    v_cached text;
    v_state text;
BEGIN
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    SELECT * INTO v_handle
      FROM _vexfs.handles AS handle
     WHERE handle.handle_id = p_handle
       AND handle.owner_oid = v_actor
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_HANDLE_NOT_FOUND: %', p_handle
            USING ERRCODE = 'P0002';
    END IF;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'handle', p_handle, 'retain', p_retain_unpublished));
    PERFORM _vexfs.lock_request(v_handle.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_handle.workspace_id, p_request_id, 'handle_close', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached;
    END IF;
    v_state := CASE
        WHEN p_retain_unpublished
         AND v_handle.dirty_generation > v_handle.published_generation
        THEN 'retained' ELSE 'closed' END;
    UPDATE _vexfs.handles
       SET state = v_state,
           lease_until = CASE WHEN v_state = 'retained'
               THEN clock_timestamp() + interval '24 hours'
               ELSE clock_timestamp() END,
           updated_at = clock_timestamp()
     WHERE handle_id = p_handle;
    IF v_state = 'closed' THEN
        DELETE FROM _vexfs.handle_staging
         WHERE handle_id = p_handle;
    END IF;
    PERFORM _vexfs.reap_orphan_inodes(v_handle.workspace_id);
    PERFORM _vexfs.replay_put(
        v_handle.workspace_id, p_request_id, 'handle_close', v_hash, v_state);
    RETURN v_state;
END;
$$;

CREATE FUNCTION public.vexfs_mount_synchronize(
    p_workspace text,
    p_request_id text,
    p_session_id text DEFAULT NULL)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_hash text;
    v_cached text;
    v_handle record;
    v_count bigint := 0;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'workspace', p_workspace, 'session', p_session_id));
    PERFORM _vexfs.lock_request(v_workspace.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_workspace.workspace_id, p_request_id, 'mount_synchronize', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::bigint;
    END IF;
    FOR v_handle IN
        SELECT handle.handle_id, handle.dirty_generation
          FROM _vexfs.handles AS handle
         WHERE handle.workspace_id = v_workspace.workspace_id
           AND handle.owner_oid = v_actor
           AND handle.state IN ('open', 'retained')
           AND handle.dirty_generation > handle.published_generation
           AND ((p_session_id IS NULL AND handle.session_id IS NULL)
                OR handle.session_id = p_session_id)
         ORDER BY handle.handle_id
    LOOP
        PERFORM public.vexfs_handle_publish(
            v_handle.handle_id, v_handle.dirty_generation, 'full',
            'synchronize:' || coalesce(nullif(p_request_id, ''), 'anonymous') ||
            ':' || v_handle.handle_id || ':' || v_handle.dirty_generation::text);
        v_count := v_count + 1;
    END LOOP;
    PERFORM _vexfs.replay_put(
        v_workspace.workspace_id, p_request_id, 'mount_synchronize',
        v_hash, v_count::text);
    RETURN v_count;
END;
$$;

CREATE FUNCTION public.vexfs_item_reclaim(
    p_workspace text,
    p_request_id text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_hash text;
    v_cached text;
    v_count bigint;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    v_hash := _vexfs.request_digest(jsonb_build_object('workspace', p_workspace));
    PERFORM _vexfs.lock_request(v_workspace.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_workspace.workspace_id, p_request_id, 'item_reclaim', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::bigint;
    END IF;
    DELETE FROM _vexfs.file_locks
     WHERE workspace_id = v_workspace.workspace_id
       AND lease_until <= clock_timestamp();
    DELETE FROM _vexfs.mount_sessions
     WHERE workspace_id = v_workspace.workspace_id
       AND lease_until <= clock_timestamp();
    WITH removed AS (
        DELETE FROM _vexfs.handles
         WHERE workspace_id = v_workspace.workspace_id
           AND (state = 'closed'
                OR (state = 'retained' AND lease_until <= clock_timestamp()))
        RETURNING 1)
    SELECT count(*) INTO v_count FROM removed;
    PERFORM _vexfs.reap_orphan_inodes(v_workspace.workspace_id);
    DELETE FROM _vexfs.request_replays AS replay
     WHERE replay.workspace_id = v_workspace.workspace_id
       AND replay.created_at < clock_timestamp() - interval '7 days';
    PERFORM _vexfs.replay_put(
        v_workspace.workspace_id, p_request_id, 'item_reclaim',
        v_hash, v_count::text);
    RETURN v_count;
END;
$$;

CREATE FUNCTION public.vexfs_lock_acquire(
    p_workspace text,
    p_inode bigint,
    p_session_id text,
    p_lock_kind text,
    p_request_id text)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_hash text;
    v_cached text;
BEGIN
    IF p_lock_kind NOT IN ('shared', 'exclusive') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_LOCK: lock kind must be shared or exclusive'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(
        p_workspace, CASE WHEN p_lock_kind = 'exclusive' THEN 'write' ELSE 'read' END);
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    IF NOT EXISTS (
        SELECT 1 FROM _vexfs.mount_sessions AS mounted
         WHERE mounted.workspace_id = v_workspace.workspace_id
           AND mounted.session_id = p_session_id
           AND mounted.owner_oid = v_actor
           AND mounted.lease_until > clock_timestamp()) THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: mount session is not active'
            USING ERRCODE = '55006';
    END IF;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode,
        CASE WHEN p_lock_kind = 'exclusive' THEN 'write' ELSE 'read' END);
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'workspace', p_workspace, 'inode', p_inode,
        'session', p_session_id, 'kind', p_lock_kind));
    PERFORM _vexfs.lock_request(v_workspace.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_workspace.workspace_id, p_request_id, 'lock_acquire', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::integer;
    END IF;
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    DELETE FROM _vexfs.file_locks
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = p_inode
       AND lease_until <= clock_timestamp();
    IF EXISTS (
        SELECT 1 FROM _vexfs.file_locks AS held
         WHERE held.workspace_id = v_workspace.workspace_id
           AND held.inode_id = p_inode
           AND NOT (held.owner_oid = v_actor AND held.session_id = p_session_id)
           AND (p_lock_kind = 'exclusive' OR held.lock_kind = 'exclusive')) THEN
        RAISE EXCEPTION 'VEXFS_LOCK_CONFLICT: inode % is already locked', p_inode
            USING ERRCODE = '55P03';
    END IF;
    INSERT INTO _vexfs.file_locks(
        workspace_id, inode_id, owner_oid, session_id, lock_kind, lease_until)
    VALUES (
        v_workspace.workspace_id, p_inode, v_actor, p_session_id,
        p_lock_kind, clock_timestamp() + interval '30 seconds')
    ON CONFLICT (workspace_id, inode_id, owner_oid, session_id) DO UPDATE
       SET lock_kind = EXCLUDED.lock_kind,
           lease_until = EXCLUDED.lease_until;
    PERFORM _vexfs.replay_put(
        v_workspace.workspace_id, p_request_id, 'lock_acquire', v_hash, '1');
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_lock_release(
    p_workspace text,
    p_inode bigint,
    p_session_id text,
    p_request_id text)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_actor oid;
    v_hash text;
    v_cached text;
    v_count integer;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    v_hash := _vexfs.request_digest(jsonb_build_object(
        'workspace', p_workspace, 'inode', p_inode, 'session', p_session_id));
    PERFORM _vexfs.lock_request(v_workspace.workspace_id, p_request_id);
    v_cached := _vexfs.replay_get(
        v_workspace.workspace_id, p_request_id, 'lock_release', v_hash);
    IF v_cached IS NOT NULL THEN
        RETURN v_cached::integer;
    END IF;
    DELETE FROM _vexfs.file_locks
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = p_inode
       AND owner_oid = v_actor
       AND session_id = p_session_id;
    GET DIAGNOSTICS v_count = ROW_COUNT;
    PERFORM _vexfs.replay_put(
        v_workspace.workspace_id, p_request_id, 'lock_release',
        v_hash, v_count::text);
    RETURN v_count;
END;
$$;

CREATE FUNCTION public.vexfs_visibility(p_workspace text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    RETURN jsonb_build_object(
        'workspace_head', v_workspace.head_commit,
        'cache_generation', v_workspace.cache_generation);
END;
$$;

CREATE FUNCTION public.vexfs_workspace_head(p_workspace text)
RETURNS bigint
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT (public.vexfs_visibility(p_workspace)->>'workspace_head')::bigint
$$;

CREATE FUNCTION public.vexfs_cache_generation(p_workspace text)
RETURNS bigint
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT (public.vexfs_visibility(p_workspace)->>'cache_generation')::bigint
$$;

CREATE FUNCTION public.vexfs_diagnostics(p_workspace text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_pending bigint;
    v_retained bigint;
    v_staging_bytes bigint;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    SELECT count(*) FILTER (
               WHERE handle.dirty_generation > handle.published_generation),
           count(*) FILTER (WHERE handle.state = 'retained'),
           coalesce(sum(staging.dirty_bytes) FILTER (
               WHERE handle.state IN ('open', 'retained')), 0)
      INTO v_pending, v_retained, v_staging_bytes
      FROM _vexfs.handles AS handle
      LEFT JOIN _vexfs.handle_staging AS staging USING (handle_id)
     WHERE handle.workspace_id = v_workspace.workspace_id;
    RETURN jsonb_build_object(
        'schema_version', public.vexfs_contract_version(),
        'adapter_version', public.vexfs_pg_adapter_version(),
        'schema_ready', public.vexfs_contract_version() = '0.9.0',
        'backend', 'postgresql',
        'workspace', p_workspace,
        'workspace_exists', 1,
        'security_mode', 'database-role',
        'principal', session_user,
        'backend_pid', pg_backend_pid(),
        'server_version', current_setting('server_version'),
        'synchronous_commit', current_setting('synchronous_commit'),
        'workspace_head', v_workspace.head_commit,
        'cache_generation', v_workspace.cache_generation,
        'pending_handles', v_pending,
        'retained_handles', v_retained,
        'staging_bytes', v_staging_bytes);
END;
$$;

CREATE FUNCTION public.vexfs_compare_versions(
    p_workspace text,
    p_path text,
    p_from_version bigint,
    p_to_version bigint)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_from bytea;
    v_to bytea;
BEGIN
    IF p_from_version <= 0 OR p_to_version <= 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_VERSION: versions must be positive'
            USING ERRCODE = '22023';
    END IF;
    v_from := public.vexfs_read_version(p_workspace, p_path, p_from_version);
    v_to := public.vexfs_read_version(p_workspace, p_path, p_to_version);
    RETURN jsonb_build_object(
        'path', p_path,
        'from', p_from_version,
        'to', p_to_version,
        'changed', v_from IS DISTINCT FROM v_to,
        'binary', position(decode('00', 'hex') IN v_from) > 0
                  OR position(decode('00', 'hex') IN v_to) > 0,
        'from_size', octet_length(v_from),
        'to_size', octet_length(v_to));
END;
$$;

CREATE FUNCTION public.vexfs_symlink(
    p_workspace text,
    p_path text,
    p_target bytea)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_parent bigint;
    v_name text;
    v_inode bigint;
    v_manifest bigint;
    v_checksum text;
    v_commit bigint;
    v_principal_oid oid;
BEGIN
    IF p_target IS NULL OR octet_length(p_target) = 0
       OR octet_length(p_target) > 4096
       OR position(decode('00', 'hex') IN p_target) > 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SYMLINK: target must be 1..4096 bytes without NUL'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    SELECT rp.parent_inode, rp.entry_name INTO v_parent, v_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_path) AS rp;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');
    IF EXISTS (
        SELECT 1 FROM _vexfs.dentries AS d
         WHERE d.workspace_id = v_workspace.workspace_id
           AND d.parent_inode = v_parent AND d.name = v_name) THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_EXISTS: %', p_path
            USING ERRCODE = '23505';
    END IF;
    PERFORM _vexfs.enforce_quota(
        v_workspace.workspace_id, NULL, octet_length(p_target));
    SELECT r.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    INSERT INTO _vexfs.inodes(
        workspace_id, kind, mode, owner_oid, owner_role, owner_principal,
        current_version, size_bytes)
    VALUES (
        v_workspace.workspace_id, 'symlink', 511,
        v_principal_oid, session_user, session_user, 1, octet_length(p_target))
    RETURNING inode_id INTO v_inode;
    INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
    VALUES (v_workspace.workspace_id, v_parent, v_name, v_inode);
    PERFORM _vexfs.inherit_acl(
        v_workspace.workspace_id, v_parent, v_inode);
    v_manifest := _vexfs.store_manifest(
        v_workspace.workspace_id, v_inode, NULL, p_target);
    SELECT m.checksum INTO STRICT v_checksum
      FROM _vexfs.manifests AS m WHERE m.manifest_id = v_manifest;
    v_commit := _vexfs.record_commit(
        v_workspace.workspace_id, 'symlink', p_path, v_inode,
        jsonb_build_object('before_version', NULL, 'after_version', 1));
    INSERT INTO _vexfs.file_versions(
        workspace_id, inode_id, version_no, commit_no, manifest_id,
        size_bytes, checksum, created_by_oid, created_by)
    VALUES (
        v_workspace.workspace_id, v_inode, 1, v_commit, v_manifest,
        octet_length(p_target), v_checksum, v_principal_oid, session_user);
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(), changed_at = clock_timestamp()
     WHERE inode_id = v_parent;
    RETURN v_inode;
END;
$$;

CREATE FUNCTION public.vexfs_link(
    p_workspace text,
    p_source text,
    p_target text)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_source_inode bigint;
    v_source_kind text;
    v_source_version bigint;
    v_parent bigint;
    v_name text;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    v_source_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_source);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_source_inode, 'read');
    SELECT i.kind, i.current_version INTO STRICT v_source_kind, v_source_version
      FROM _vexfs.inodes AS i WHERE i.inode_id = v_source_inode;
    IF v_source_kind <> 'file' THEN
        RAISE EXCEPTION 'VEXFS_INVALID_HARDLINK: source must be a regular file'
            USING ERRCODE = '22023';
    END IF;
    SELECT rp.parent_inode, rp.entry_name INTO v_parent, v_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_target) AS rp;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');
    IF EXISTS (
        SELECT 1 FROM _vexfs.dentries AS d
         WHERE d.workspace_id = v_workspace.workspace_id
           AND d.parent_inode = v_parent AND d.name = v_name) THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_EXISTS: %', p_target
            USING ERRCODE = '23505';
    END IF;
    INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
    VALUES (v_workspace.workspace_id, v_parent, v_name, v_source_inode);
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(), changed_at = clock_timestamp()
     WHERE inode_id = v_parent;
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'link', p_target, v_source_inode,
        jsonb_build_object(
            'before_version', v_source_version,
            'after_version', v_source_version,
            'source_path', p_source,
            'target_path', p_target));
    RETURN v_source_inode;
END;
$$;

CREATE FUNCTION public.vexfs_readlink(
    p_workspace text,
    p_inode bigint)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_version bigint;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'read');
    SELECT i.current_version INTO v_version
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode
       AND i.kind = 'symlink'
       AND i.live;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_NOT_SYMLINK: %', p_inode
            USING ERRCODE = '42809';
    END IF;
    RETURN _vexfs.read_version_content(
        v_workspace.workspace_id, p_inode, v_version);
END;
$$;

CREATE FUNCTION public.vexfs_set_mode(
    p_workspace text,
    p_inode bigint,
    p_mode integer)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_before_mode integer;
    v_version bigint;
    v_path text;
BEGIN
    IF p_mode < 0 OR p_mode > 4095 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_MODE: %', p_mode USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'metadata');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'metadata');
    SELECT i.mode, i.current_version
      INTO v_before_mode, v_version
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode AND i.live AND i.kind <> 'symlink'
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND_OR_SYMLINK: %', p_inode
            USING ERRCODE = 'P0002';
    END IF;
    v_path := _vexfs.path_for_inode(v_workspace.workspace_id, p_inode);
    UPDATE _vexfs.inodes
       SET mode = p_mode, changed_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = p_inode AND live AND kind <> 'symlink';
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'chmod', v_path, p_inode,
        jsonb_build_object(
            'before_version', v_version,
            'after_version', v_version,
            'before_mode', v_before_mode,
            'after_mode', p_mode));
    RETURN p_mode;
END;
$$;

CREATE FUNCTION public.vexfs_set_times(
    p_workspace text,
    p_inode bigint,
    p_accessed_at_ms bigint,
    p_modified_at_ms bigint,
    p_mask integer)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_version bigint;
    v_path text;
BEGIN
    IF p_accessed_at_ms < 0 OR p_modified_at_ms < 0 OR p_mask NOT IN (1, 2, 3) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_TIME: invalid millisecond timestamp or mask'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'metadata');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'metadata');
    SELECT i.current_version INTO v_version
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode AND i.live
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_inode USING ERRCODE = 'P0002';
    END IF;
    v_path := _vexfs.path_for_inode(v_workspace.workspace_id, p_inode);
    UPDATE _vexfs.inodes
       SET accessed_at = CASE WHEN (p_mask & 1) <> 0
                              THEN to_timestamp(p_accessed_at_ms / 1000.0)
                              ELSE accessed_at END,
           modified_at = CASE WHEN (p_mask & 2) <> 0
                              THEN to_timestamp(p_modified_at_ms / 1000.0)
                              ELSE modified_at END,
           changed_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = p_inode AND live;
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'set_times', v_path, p_inode,
        jsonb_build_object(
            'before_version', v_version,
            'after_version', v_version,
            'mask', p_mask));
    RETURN p_mask;
END;
$$;

CREATE FUNCTION public.vexfs_chown(
    p_workspace text,
    p_inode bigint,
    p_uid bigint,
    p_gid bigint)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_result jsonb;
    v_before_uid bigint;
    v_before_gid bigint;
    v_version bigint;
    v_path text;
BEGIN
    IF p_uid < -1 OR p_uid > 4294967295 OR p_gid < -1 OR p_gid > 4294967295 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_OWNER: uid/gid must be -1 or 0..4294967295'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'metadata');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'metadata');
    SELECT i.uid, i.gid, i.current_version
      INTO v_before_uid, v_before_gid, v_version
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode AND i.live
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_inode USING ERRCODE = 'P0002';
    END IF;
    v_path := _vexfs.path_for_inode(v_workspace.workspace_id, p_inode);
    UPDATE _vexfs.inodes
       SET uid = CASE WHEN p_uid = -1 THEN uid ELSE p_uid END,
           gid = CASE WHEN p_gid = -1 THEN gid ELSE p_gid END,
           changed_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = p_inode AND live
    RETURNING jsonb_build_object('uid', uid, 'gid', gid) INTO v_result;
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'chown', v_path, p_inode,
        jsonb_build_object(
            'before_version', v_version,
            'after_version', v_version,
            'before_uid', v_before_uid,
            'before_gid', v_before_gid,
            'after_uid', v_result->'uid',
            'after_gid', v_result->'gid'));
    RETURN v_result;
END;
$$;

-- owner_principal is portable archive metadata. PostgreSQL authorization keeps
-- using owner_oid/session_user, so changing this value cannot grant a database
-- role access or impersonate another authenticated principal.
CREATE FUNCTION public.vexfs_owner_set(
    p_workspace text,
    p_path text,
    p_owner_principal text)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_inode bigint;
    v_before_owner text;
    v_version bigint;
BEGIN
    IF p_owner_principal IS NULL OR p_owner_principal = ''
       OR octet_length(p_owner_principal) > 255 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_OWNER: owner principal must be 1..255 bytes'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'admin');
    SELECT i.owner_principal, i.current_version
      INTO v_before_owner, v_version
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = v_inode AND i.live
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', v_inode USING ERRCODE = 'P0002';
    END IF;
    UPDATE _vexfs.inodes
       SET owner_principal = p_owner_principal,
           changed_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = v_inode AND live;
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'set_owner_principal', p_path, v_inode,
        jsonb_build_object(
            'before_version', v_version,
            'after_version', v_version,
            'before_owner', v_before_owner,
            'after_owner', p_owner_principal));
    RETURN p_owner_principal;
END;
$$;

CREATE FUNCTION public.vexfs_xattr_get(
    p_workspace text,
    p_inode bigint,
    p_name text)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_value bytea;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'read');
    SELECT x.value INTO v_value
      FROM _vexfs.xattrs AS x
     WHERE x.workspace_id = v_workspace.workspace_id
       AND x.inode_id = p_inode AND x.name = p_name;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_XATTR_NOT_FOUND: %', p_name USING ERRCODE = 'P0002';
    END IF;
    RETURN v_value;
END;
$$;

CREATE FUNCTION public.vexfs_xattr_list(
    p_workspace text,
    p_inode bigint)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_result jsonb;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'read');
    SELECT coalesce(jsonb_agg(x.name ORDER BY x.name), '[]'::jsonb)
      INTO v_result
      FROM _vexfs.xattrs AS x
     WHERE x.workspace_id = v_workspace.workspace_id
       AND x.inode_id = p_inode;
    RETURN v_result;
END;
$$;

CREATE FUNCTION public.vexfs_xattr_set(
    p_workspace text,
    p_inode bigint,
    p_name text,
    p_value bytea,
    p_policy integer DEFAULT 0)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_exists boolean;
    v_version bigint;
    v_path text;
BEGIN
    IF p_name IS NULL OR p_name = '' OR octet_length(p_name) > 255
       OR p_policy NOT BETWEEN 0 AND 3
       OR (p_policy <> 3 AND (p_value IS NULL OR octet_length(p_value) > 65536)) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_XATTR: invalid name, value, or policy'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'metadata');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'metadata');
    SELECT i.current_version INTO v_version
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode AND i.live
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_inode USING ERRCODE = 'P0002';
    END IF;
    v_path := _vexfs.path_for_inode(v_workspace.workspace_id, p_inode);
    SELECT EXISTS (
        SELECT 1 FROM _vexfs.xattrs AS x
         WHERE x.workspace_id = v_workspace.workspace_id
           AND x.inode_id = p_inode AND x.name = p_name) INTO v_exists;
    IF p_policy = 1 AND v_exists THEN
        RAISE EXCEPTION 'VEXFS_XATTR_EXISTS: %', p_name USING ERRCODE = '23505';
    END IF;
    IF p_policy IN (2, 3) AND NOT v_exists THEN
        RAISE EXCEPTION 'VEXFS_XATTR_NOT_FOUND: %', p_name USING ERRCODE = 'P0002';
    END IF;
    IF p_policy = 3 THEN
        DELETE FROM _vexfs.xattrs
         WHERE workspace_id = v_workspace.workspace_id
           AND inode_id = p_inode AND name = p_name;
    ELSE
        INSERT INTO _vexfs.xattrs(workspace_id, inode_id, name, value)
        VALUES (v_workspace.workspace_id, p_inode, p_name, p_value)
        ON CONFLICT (workspace_id, inode_id, name) DO UPDATE
          SET value = EXCLUDED.value, updated_at = clock_timestamp();
    END IF;
    UPDATE _vexfs.inodes SET changed_at = clock_timestamp()
     WHERE inode_id = p_inode;
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id,
        CASE WHEN p_policy = 3 THEN 'remove_xattr' ELSE 'set_xattr' END,
        v_path, p_inode,
        jsonb_build_object(
            'before_version', v_version,
            'after_version', v_version,
            'xattr_name', p_name,
            'existed_before', v_exists,
            'exists_after', p_policy <> 3));
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_acl_get(
    p_workspace text,
    p_inode bigint)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_result jsonb;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'read');
    SELECT coalesce(jsonb_agg(jsonb_build_object(
               'principal', acl.principal,
               'effect', acl.effect,
               'permissions', acl.permissions,
               'inherit', acl.inherit_flags)
               ORDER BY acl.principal, acl.effect), '[]'::jsonb)
      INTO v_result
      FROM _vexfs.acl_entries AS acl
     WHERE acl.workspace_id = v_workspace.workspace_id
       AND acl.inode_id = p_inode;
    RETURN v_result;
END;
$$;

CREATE FUNCTION public.vexfs_acl_list(
    p_workspace text,
    p_inode bigint)
RETURNS jsonb
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$ SELECT public.vexfs_acl_get(p_workspace, p_inode) $$;

CREATE FUNCTION public.vexfs_acl_set(
    p_workspace text,
    p_inode bigint,
    p_acl jsonb)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_canonical jsonb;
    v_before_set bigint;
    v_after_set bigint;
    v_count integer;
    v_before_count integer;
    v_version bigint;
    v_path text;
BEGIN
    v_canonical := _vexfs.canonical_acl(p_acl);
    v_count := jsonb_array_length(v_canonical);
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'admin');
    SELECT i.current_version, i.acl_set_id INTO v_version, v_before_set
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode AND i.live
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_inode USING ERRCODE = 'P0002';
    END IF;
    v_path := _vexfs.path_for_inode(v_workspace.workspace_id, p_inode);
    SELECT coalesce(set.entry_count, 0) INTO v_before_count
      FROM _vexfs.inodes AS inode
      LEFT JOIN _vexfs.acl_sets AS set ON set.acl_set_id = inode.acl_set_id
     WHERE inode.workspace_id = v_workspace.workspace_id
       AND inode.inode_id = p_inode;
    v_after_set := _vexfs.get_or_create_acl_set(
        v_workspace.workspace_id, v_canonical);
    IF v_after_set IS NOT DISTINCT FROM v_before_set THEN
        RETURN v_count;
    END IF;
    UPDATE _vexfs.inodes SET changed_at = clock_timestamp()
         , acl_set_id = v_after_set
     WHERE workspace_id = v_workspace.workspace_id AND inode_id = p_inode;
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'set_acl', v_path, p_inode,
        jsonb_build_object(
            'before_version', v_version,
            'after_version', v_version,
            'before_entries', v_before_count,
            'after_entries', v_count,
            'before_acl_set', v_before_set,
            'after_acl_set', v_after_set));
    RETURN v_count;
END;
$$;

CREATE FUNCTION public.vexfs_acl_grant(
    p_workspace text,
    p_inode bigint,
    p_principal text,
    p_permissions text,
    p_effect text DEFAULT 'allow',
    p_inherit integer DEFAULT 0)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_before_acl jsonb;
    v_after_acl jsonb;
    v_before_set bigint;
    v_after_set bigint;
    v_version bigint;
    v_path text;
BEGIN
    IF p_principal IS NULL OR p_principal = '' OR octet_length(p_principal) > 255
       OR p_permissions IS NULL OR p_permissions = '' OR octet_length(p_permissions) > 1024
       OR p_effect NOT IN ('allow', 'deny') OR p_inherit NOT BETWEEN 0 AND 255 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ACL: invalid grant' USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'admin');
    SELECT i.current_version, i.acl_set_id INTO v_version, v_before_set
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode AND i.live
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_inode USING ERRCODE = 'P0002';
    END IF;
    v_path := _vexfs.path_for_inode(v_workspace.workspace_id, p_inode);
    v_before_acl := _vexfs.acl_json_for_inode(
        v_workspace.workspace_id, p_inode);
    SELECT coalesce(jsonb_agg(item.value ORDER BY item.value->>'principal',
                                             item.value->>'effect'), '[]'::jsonb)
      INTO v_after_acl
      FROM jsonb_array_elements(v_before_acl) AS item(value)
     WHERE item.value->>'principal' <> p_principal
        OR item.value->>'effect' <> p_effect;
    v_after_acl := v_after_acl || jsonb_build_array(jsonb_build_object(
        'principal', p_principal,
        'effect', p_effect,
        'permissions', p_permissions,
        'inherit', p_inherit));
    v_after_set := _vexfs.get_or_create_acl_set(
        v_workspace.workspace_id, v_after_acl);
    IF v_after_set IS NOT DISTINCT FROM v_before_set THEN
        RETURN 1;
    END IF;
    UPDATE _vexfs.inodes SET changed_at = clock_timestamp()
         , acl_set_id = v_after_set
     WHERE workspace_id = v_workspace.workspace_id AND inode_id = p_inode;
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'grant_acl', v_path, p_inode,
        jsonb_build_object(
            'before_version', v_version,
            'after_version', v_version,
            'principal', p_principal,
            'effect', p_effect,
            'permissions', p_permissions,
            'inherit', p_inherit,
            'before_acl_set', v_before_set,
            'after_acl_set', v_after_set));
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_acl_revoke(
    p_workspace text,
    p_inode bigint,
    p_principal text,
    p_effect text DEFAULT NULL)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_before_acl jsonb;
    v_after_acl jsonb;
    v_before_set bigint;
    v_after_set bigint;
    v_removed integer;
    v_version bigint;
    v_path text;
BEGIN
    IF p_effect IS NOT NULL AND p_effect NOT IN ('allow', 'deny') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ACL: invalid effect' USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'admin');
    SELECT i.current_version, i.acl_set_id INTO v_version, v_before_set
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode AND i.live
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_inode USING ERRCODE = 'P0002';
    END IF;
    v_path := _vexfs.path_for_inode(v_workspace.workspace_id, p_inode);
    v_before_acl := _vexfs.acl_json_for_inode(
        v_workspace.workspace_id, p_inode);
    SELECT coalesce(jsonb_agg(item.value ORDER BY item.value->>'principal',
                                             item.value->>'effect') FILTER (
               WHERE item.value->>'principal' <> p_principal
                  OR (p_effect IS NOT NULL
                      AND item.value->>'effect' <> p_effect)), '[]'::jsonb),
           count(*) FILTER (
               WHERE item.value->>'principal' = p_principal
                 AND (p_effect IS NULL OR item.value->>'effect' = p_effect))
      INTO v_after_acl, v_removed
      FROM jsonb_array_elements(v_before_acl) AS item(value);
    IF v_removed > 0 THEN
        v_after_set := _vexfs.get_or_create_acl_set(
            v_workspace.workspace_id, v_after_acl);
        UPDATE _vexfs.inodes SET changed_at = clock_timestamp()
             , acl_set_id = v_after_set
         WHERE workspace_id = v_workspace.workspace_id AND inode_id = p_inode;
        PERFORM _vexfs.record_commit(
            v_workspace.workspace_id, 'revoke_acl', v_path, p_inode,
            jsonb_build_object(
                'before_version', v_version,
                'after_version', v_version,
                'principal', p_principal,
                'effect', p_effect,
                'removed_entries', v_removed,
                'before_acl_set', v_before_set,
                'after_acl_set', v_after_set));
    END IF;
    RETURN v_removed;
END;
$$;

CREATE FUNCTION public.vexfs_acl_delete(
    p_workspace text,
    p_inode bigint)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_before_set bigint;
    v_removed integer;
    v_version bigint;
    v_path text;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, p_inode, 'admin');
    SELECT i.current_version, i.acl_set_id INTO v_version, v_before_set
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = p_inode AND i.live
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INODE_NOT_FOUND: %', p_inode USING ERRCODE = 'P0002';
    END IF;
    v_path := _vexfs.path_for_inode(v_workspace.workspace_id, p_inode);
    SELECT coalesce(set.entry_count, 0) INTO v_removed
      FROM _vexfs.inodes AS inode
      LEFT JOIN _vexfs.acl_sets AS set ON set.acl_set_id = inode.acl_set_id
     WHERE inode.workspace_id = v_workspace.workspace_id
       AND inode.inode_id = p_inode;
    IF v_removed > 0 THEN
        UPDATE _vexfs.inodes SET changed_at = clock_timestamp(), acl_set_id = NULL
         WHERE workspace_id = v_workspace.workspace_id AND inode_id = p_inode;
        PERFORM _vexfs.record_commit(
            v_workspace.workspace_id, 'delete_acl', v_path, p_inode,
            jsonb_build_object(
                'before_version', v_version,
                'after_version', v_version,
                'removed_entries', v_removed,
                'before_acl_set', v_before_set,
                'after_acl_set', NULL));
    END IF;
    RETURN v_removed;
END;
$$;

CREATE FUNCTION public.vexfs_audit_list(
    p_workspace text,
    p_limit integer DEFAULT 100,
    p_before_event bigint DEFAULT NULL)
RETURNS TABLE(
    event_id bigint,
    commit_no bigint,
    actor name,
    operation text,
    path text,
    inode_id bigint,
    details jsonb,
    created_at timestamptz)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    IF p_limit NOT BETWEEN 1 AND 1000 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_LIMIT: limit must be between 1 and 1000'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    RETURN QUERY
    SELECT event.event_id,
           event.commit_no,
           coalesce(role.rolname, event.actor_role)::name,
           event.operation,
           event.path,
           event.inode_id,
           event.details,
           event.created_at
      FROM _vexfs.audit_events AS event
      LEFT JOIN pg_catalog.pg_roles AS role ON role.oid = event.actor_oid
     WHERE event.workspace_id = v_workspace.workspace_id
       AND (p_before_event IS NULL OR event.event_id < p_before_event)
     ORDER BY event.event_id DESC
     LIMIT p_limit;
END;
$$;

CREATE FUNCTION public.vexfs_workspace_stat(p_workspace text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_owner name;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    SELECT r.rolname INTO v_owner
      FROM pg_catalog.pg_roles AS r
     WHERE r.oid = v_workspace.owner_oid;
    RETURN jsonb_build_object(
        'workspace_id', v_workspace.workspace_id,
        'name', v_workspace.name,
        'owner', coalesce(v_owner, v_workspace.owner_role),
        'root_inode', v_workspace.root_inode,
        'head_commit', v_workspace.head_commit,
        'history_floor_commit', v_workspace.history_floor_commit,
        'cache_generation', v_workspace.cache_generation,
        'live_files', v_workspace.live_files,
        'live_bytes', v_workspace.live_bytes,
        'created_at', v_workspace.created_at);
END;
$$;

CREATE FUNCTION public.vexfs_workspace_history(
    p_workspace text, p_limit integer DEFAULT 100)
RETURNS TABLE(
    commit_no bigint,
    parent_commit bigint,
    operation text,
    created_by name,
    created_at timestamptz)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    IF p_limit < 1 OR p_limit > 1000 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_LIMIT: limit must be between 1 and 1000'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    RETURN QUERY
    SELECT c.commit_no,
           c.parent_commit,
           c.operation,
           coalesce(r.rolname, c.created_by)::name,
           c.created_at
      FROM _vexfs.commits AS c
      LEFT JOIN pg_catalog.pg_roles AS r ON r.oid = c.created_by_oid
     WHERE c.workspace_id = v_workspace.workspace_id
     ORDER BY c.commit_no DESC
     LIMIT p_limit;
END;
$$;

CREATE FUNCTION public.vexfs_workspace_log(
    p_workspace text,
    p_limit integer DEFAULT 100,
    p_before_commit bigint DEFAULT 0)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_result jsonb;
BEGIN
    IF p_limit NOT BETWEEN 1 AND 1000 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_LIMIT: limit must be between 1 and 1000'
            USING ERRCODE = '22023';
    END IF;
    IF coalesce(p_before_commit, 0) < 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CURSOR: before commit must be non-negative'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');

    WITH rows AS MATERIALIZED (
        SELECT commit_row.*,
               coalesce(role.rolname, commit_row.created_by)::text AS actor,
               row_number() OVER (ORDER BY commit_row.commit_no DESC) AS ordinal
          FROM _vexfs.commits AS commit_row
          LEFT JOIN pg_catalog.pg_roles AS role
            ON role.oid = commit_row.created_by_oid
         WHERE commit_row.workspace_id = v_workspace.workspace_id
           AND (coalesce(p_before_commit, 0) = 0
                OR commit_row.commit_no < p_before_commit)
         ORDER BY commit_row.commit_no DESC
         LIMIT p_limit + 1
    ), page AS (
        SELECT * FROM rows WHERE ordinal <= p_limit
    )
    SELECT jsonb_build_object(
        'entries', coalesce((
            SELECT jsonb_agg(jsonb_build_object(
                       'commit', item.commit_no,
                       'parent_commit', nullif(item.parent_commit, 0),
                       'created_at', (extract(epoch FROM item.created_at) * 1000)::bigint,
                       'operation', item.operation,
                       'path', item.path,
                       'actor', item.actor,
                       'session_id', item.session_id,
                       'run_id', item.run_id,
                       'has_snapshot', EXISTS (
                           SELECT 1 FROM _vexfs.snapshots AS snapshot
                            WHERE snapshot.workspace_id = item.workspace_id
                              AND snapshot.head_commit = item.commit_no),
                       'snapshots', coalesce((
                           SELECT jsonb_agg(snapshot.name ORDER BY snapshot.name)
                             FROM _vexfs.snapshots AS snapshot
                            WHERE snapshot.workspace_id = item.workspace_id
                              AND snapshot.head_commit = item.commit_no), '[]'::jsonb))
                       ORDER BY item.commit_no DESC)
              FROM page AS item), '[]'::jsonb),
        'next_before', CASE WHEN EXISTS (
            SELECT 1 FROM rows WHERE ordinal > p_limit)
            THEN (SELECT min(commit_no) FROM page) ELSE NULL END)
      INTO v_result;
    RETURN v_result;
END;
$$;

CREATE FUNCTION public.vexfs_create(
    p_workspace text, p_path text, p_kind text, p_mode integer)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_parent bigint;
    v_name text;
    v_parent_kind text;
    v_inode bigint;
    v_version bigint;
    v_commit bigint;
    v_manifest bigint;
    v_checksum text;
    v_principal_oid oid;
BEGIN
    IF p_kind NOT IN ('file', 'directory') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_KIND: %', p_kind
            USING ERRCODE = '22023';
    END IF;
    IF p_mode < 0 OR p_mode > 4095 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_MODE: %', p_mode
            USING ERRCODE = '22023';
    END IF;

    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    SELECT rp.parent_inode, rp.entry_name INTO v_parent, v_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_path) AS rp;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');
    SELECT i.kind INTO v_parent_kind
      FROM _vexfs.inodes AS i WHERE i.inode_id = v_parent;
    IF v_parent_kind <> 'directory' THEN
        RAISE EXCEPTION 'VEXFS_NOT_DIRECTORY: parent of %', p_path
            USING ERRCODE = '42809';
    END IF;
    IF EXISTS (
        SELECT 1 FROM _vexfs.dentries AS d
         WHERE d.workspace_id = v_workspace.workspace_id
           AND d.parent_inode = v_parent
           AND d.name = v_name) THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_EXISTS: %', p_path
            USING ERRCODE = '23505';
    END IF;

    SELECT r.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    IF p_kind = 'file' THEN
        PERFORM _vexfs.enforce_quota(
            v_workspace.workspace_id, NULL, 0);
    END IF;
    v_version := CASE WHEN p_kind = 'file' THEN 1 ELSE 0 END;
    INSERT INTO _vexfs.inodes(
        workspace_id, kind, mode, owner_oid, owner_role,
        owner_principal, current_version)
    VALUES (
        v_workspace.workspace_id, p_kind, p_mode,
        v_principal_oid, session_user, session_user, v_version)
    RETURNING inode_id INTO v_inode;
    INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
    VALUES (v_workspace.workspace_id, v_parent, v_name, v_inode);
    PERFORM _vexfs.inherit_acl(
        v_workspace.workspace_id, v_parent, v_inode);
    v_commit := _vexfs.record_commit(
        v_workspace.workspace_id, 'create', p_path, v_inode,
        jsonb_build_object(
            'before_version', NULL,
            'after_version', v_version,
            'kind', p_kind,
            'mode', p_mode));
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE inode_id = v_parent;

    IF p_kind = 'file' THEN
        v_manifest := _vexfs.store_manifest(
            v_workspace.workspace_id, v_inode, NULL, ''::bytea);
        SELECT m.checksum INTO STRICT v_checksum
          FROM _vexfs.manifests AS m
         WHERE m.manifest_id = v_manifest;
        INSERT INTO _vexfs.file_versions(
            workspace_id, inode_id, version_no, commit_no, manifest_id,
            size_bytes, checksum, created_by_oid, created_by)
        VALUES (
            v_workspace.workspace_id, v_inode, 1, v_commit,
            v_manifest, 0, v_checksum, v_principal_oid, session_user);
    END IF;
    RETURN v_inode;
END;
$$;

-- Create many direct children of one directory with one workspace commit.
-- Per-file versions and change rows are retained, but workspace HEAD, audit,
-- notification, quota counters and the parent timestamp advance once.
CREATE FUNCTION public.vexfs_create_batch(
    p_workspace text,
    p_parent text,
    p_entries jsonb)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_parent bigint;
    v_parent_kind text;
    v_parent_path text;
    v_actor oid;
    v_entry jsonb;
    v_name text;
    v_kind text;
    v_mode integer;
    v_names text[] := ARRAY[]::text[];
    v_kinds text[] := ARRAY[]::text[];
    v_modes integer[] := ARRAY[]::integer[];
    v_inodes bigint[] := ARRAY[]::bigint[];
    v_count integer;
    v_file_count integer;
    v_index integer;
    v_conflict text;
    v_manifest bigint;
    v_acl_set bigint;
    v_checksum text;
    v_commit bigint;
    v_created jsonb;
BEGIN
    IF p_entries IS NULL OR jsonb_typeof(p_entries) <> 'array' THEN
        RAISE EXCEPTION 'VEXFS_INVALID_BATCH: entries must be a JSON array'
            USING ERRCODE = '22023';
    END IF;
    IF octet_length(p_entries::text) > 1048576 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_BATCH: entries exceed 1 MiB'
            USING ERRCODE = '54000';
    END IF;
    v_count := jsonb_array_length(p_entries);
    IF v_count < 1 OR v_count > 1000 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_BATCH: entries must contain 1..1000 items'
            USING ERRCODE = '22023';
    END IF;

    FOR v_entry IN SELECT value FROM jsonb_array_elements(p_entries) LOOP
        IF jsonb_typeof(v_entry) <> 'object'
           OR jsonb_typeof(v_entry->'name') <> 'string'
           OR (v_entry ? 'kind' AND jsonb_typeof(v_entry->'kind') <> 'string')
           OR (v_entry ? 'mode' AND (
               jsonb_typeof(v_entry->'mode') <> 'number'
               OR v_entry->>'mode' !~ '^[0-9]+$'))
           OR EXISTS (
               SELECT 1 FROM jsonb_object_keys(v_entry) AS key
                WHERE key NOT IN ('name', 'kind', 'mode')) THEN
            RAISE EXCEPTION 'VEXFS_INVALID_BATCH: each item requires name and optional kind/mode'
                USING ERRCODE = '22023';
        END IF;
        v_name := v_entry->>'name';
        v_kind := coalesce(v_entry->>'kind', 'file');
        IF v_name IS NULL OR v_name = '' OR v_name IN ('.', '..')
           OR octet_length(v_name) > 255 OR position('/' IN v_name) > 0 THEN
            RAISE EXCEPTION 'VEXFS_INVALID_NAME: %', coalesce(v_name, '<null>')
                USING ERRCODE = '22023';
        END IF;
        IF v_kind NOT IN ('file', 'directory') THEN
            RAISE EXCEPTION 'VEXFS_INVALID_KIND: %', v_kind
                USING ERRCODE = '22023';
        END IF;
        BEGIN
            v_mode := CASE WHEN v_entry ? 'mode'
                THEN (v_entry->>'mode')::integer
                WHEN v_kind = 'directory' THEN 493
                ELSE 420 END;
        EXCEPTION WHEN invalid_text_representation OR numeric_value_out_of_range THEN
            RAISE EXCEPTION 'VEXFS_INVALID_MODE: batch mode is not an integer'
                USING ERRCODE = '22023';
        END;
        IF v_mode < 0 OR v_mode > 4095 THEN
            RAISE EXCEPTION 'VEXFS_INVALID_MODE: %', v_mode
                USING ERRCODE = '22023';
        END IF;
        v_names := array_append(v_names, v_name);
        v_kinds := array_append(v_kinds, v_kind);
        v_modes := array_append(v_modes, v_mode);
    END LOOP;
    IF EXISTS (
        SELECT 1 FROM unnest(v_names) AS item(name)
         GROUP BY item.name HAVING count(*) > 1) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_BATCH: entry names must be unique'
            USING ERRCODE = '22023';
    END IF;

    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT * INTO STRICT v_workspace
      FROM _vexfs.workspaces AS workspace
     WHERE workspace.workspace_id = v_workspace.workspace_id
     FOR UPDATE;
    v_parent := _vexfs.resolve_path(v_workspace.workspace_id, p_parent);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');
    SELECT inode.kind INTO STRICT v_parent_kind
      FROM _vexfs.inodes AS inode
     WHERE inode.workspace_id = v_workspace.workspace_id
       AND inode.inode_id = v_parent
       AND inode.live;
    IF v_parent_kind <> 'directory' THEN
        RAISE EXCEPTION 'VEXFS_NOT_DIRECTORY: %', p_parent
            USING ERRCODE = '42809';
    END IF;
    v_parent_path := _vexfs.path_for_inode(v_workspace.workspace_id, v_parent);
    IF v_parent_path IS NULL THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: batch parent is not reachable'
            USING ERRCODE = 'XX001';
    END IF;
    IF EXISTS (
        SELECT 1 FROM unnest(v_names) AS item(name)
         WHERE octet_length(v_parent_path) + octet_length(item.name)
               + CASE WHEN v_parent_path = '/' THEN 0 ELSE 1 END > 4096) THEN
        RAISE EXCEPTION 'VEXFS_PATH_TOO_LONG: batch child path exceeds 4096 bytes'
            USING ERRCODE = '22023';
    END IF;

    SELECT dentry.name INTO v_conflict
      FROM _vexfs.dentries AS dentry
      JOIN unnest(v_names) AS item(name) ON item.name = dentry.name
     WHERE dentry.workspace_id = v_workspace.workspace_id
       AND dentry.parent_inode = v_parent
     LIMIT 1;
    IF v_conflict IS NOT NULL THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_EXISTS: %',
            CASE WHEN v_parent_path = '/' THEN '/' || v_conflict
                 ELSE v_parent_path || '/' || v_conflict END
            USING ERRCODE = '23505';
    END IF;

    SELECT count(*) INTO v_file_count
      FROM unnest(v_kinds) AS item(kind)
     WHERE item.kind = 'file';
    IF v_workspace.quota_max_files IS NOT NULL
       AND v_workspace.live_files + v_file_count > v_workspace.quota_max_files THEN
        RAISE EXCEPTION 'VEXFS_QUOTA_FILES: workspace file quota exceeded'
            USING ERRCODE = '53100';
    END IF;
    SELECT role.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS role
     WHERE role.rolname = session_user;
    v_acl_set := _vexfs.inherited_acl_set_id(
        v_workspace.workspace_id, v_parent);

    FOR v_index IN 1..v_count LOOP
        v_inodes := array_append(
            v_inodes,
            nextval(pg_get_serial_sequence('_vexfs.inodes', 'inode_id')));
    END LOOP;
    INSERT INTO _vexfs.inodes(
        inode_id, workspace_id, kind, mode, owner_oid, owner_role,
        owner_principal, acl_set_id, current_version, size_bytes)
    OVERRIDING SYSTEM VALUE
    SELECT item.inode_id,
           v_workspace.workspace_id,
           item.kind,
           item.mode,
           v_actor,
           session_user,
           session_user,
           v_acl_set,
           CASE WHEN item.kind = 'file' THEN 1 ELSE 0 END,
           0
      FROM unnest(v_inodes, v_kinds, v_modes)
           AS item(inode_id, kind, mode);
    INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
    SELECT v_workspace.workspace_id, v_parent, item.name, item.inode_id
      FROM unnest(v_names, v_inodes) AS item(name, inode_id);
    IF v_file_count > 0 THEN
        v_manifest := _vexfs.store_manifest(
            v_workspace.workspace_id, v_parent, NULL, ''::bytea);
        SELECT manifest.checksum INTO STRICT v_checksum
          FROM _vexfs.manifests AS manifest
         WHERE manifest.manifest_id = v_manifest;
    END IF;
    v_commit := _vexfs.record_commit_header(
        v_workspace.workspace_id,
        'create_batch',
        v_parent_path,
        v_parent,
        jsonb_build_object(
            'before_version', NULL,
            'after_version', NULL,
            'item_count', v_count,
            'file_count', v_file_count));
    INSERT INTO _vexfs.file_versions(
        workspace_id, inode_id, version_no, commit_no, manifest_id,
        size_bytes, checksum, created_by_oid, created_by)
    SELECT v_workspace.workspace_id,
           item.inode_id,
           1,
           v_commit,
           v_manifest,
           0,
           v_checksum,
           v_actor,
           session_user
      FROM unnest(v_inodes, v_kinds) AS item(inode_id, kind)
     WHERE item.kind = 'file';
    INSERT INTO _vexfs.commit_changes(
        workspace_id, commit_no, ordinal, operation, path, inode_id,
        before_version, after_version, details)
    SELECT v_workspace.workspace_id,
           v_commit,
           item.ordinal::integer,
           'create',
           CASE WHEN v_parent_path = '/' THEN '/' || item.name
                ELSE v_parent_path || '/' || item.name END,
           item.inode_id,
           NULL,
           CASE WHEN item.kind = 'file' THEN 1 ELSE 0 END,
           jsonb_build_object('kind', item.kind, 'mode', item.mode)
      FROM unnest(v_names, v_kinds, v_modes, v_inodes) WITH ORDINALITY
           AS item(name, kind, mode, inode_id, ordinal);
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE workspace_id = v_workspace.workspace_id
       AND inode_id = v_parent;

    SELECT coalesce(jsonb_agg(jsonb_build_object(
               'path', CASE WHEN v_parent_path = '/' THEN '/' || item.name
                            ELSE v_parent_path || '/' || item.name END,
               'inode', item.inode_id,
               'kind', item.kind,
               'version', CASE WHEN item.kind = 'file' THEN 1 ELSE 0 END)
               ORDER BY item.ordinal), '[]'::jsonb)
      INTO v_created
      FROM unnest(v_names, v_kinds, v_inodes) WITH ORDINALITY
           AS item(name, kind, inode_id, ordinal);
    RETURN jsonb_build_object(
        'workspace', p_workspace,
        'parent', v_parent_path,
        'commit', v_commit,
        'created', v_created);
END;
$$;

CREATE FUNCTION public.vexfs_move(
    p_workspace text, p_source text, p_target text)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_source_parent bigint;
    v_source_name text;
    v_target_parent bigint;
    v_target_name text;
    v_source_inode bigint;
    v_source_kind text;
    v_source_version bigint;
    v_target_parent_kind text;
    v_cycle boolean;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    SELECT rp.parent_inode, rp.entry_name INTO v_source_parent, v_source_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_source) AS rp;
    SELECT rp.parent_inode, rp.entry_name INTO v_target_parent, v_target_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_target) AS rp;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_source_parent, 'write');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_target_parent, 'write');

    SELECT d.inode_id, i.kind, i.current_version
      INTO v_source_inode, v_source_kind, v_source_version
      FROM _vexfs.dentries AS d
      JOIN _vexfs.inodes AS i ON i.inode_id = d.inode_id
     WHERE d.workspace_id = v_workspace.workspace_id
       AND d.parent_inode = v_source_parent
       AND d.name = v_source_name;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_PATH_NOT_FOUND: %', p_source
            USING ERRCODE = 'P0002';
    END IF;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_source_inode, 'write');
    IF v_source_parent = v_target_parent AND v_source_name = v_target_name THEN
        RETURN 1;
    END IF;
    SELECT i.kind INTO v_target_parent_kind
      FROM _vexfs.inodes AS i WHERE i.inode_id = v_target_parent;
    IF v_target_parent_kind <> 'directory' THEN
        RAISE EXCEPTION 'VEXFS_NOT_DIRECTORY: parent of %', p_target
            USING ERRCODE = '42809';
    END IF;
    IF EXISTS (
        SELECT 1 FROM _vexfs.dentries AS d
         WHERE d.workspace_id = v_workspace.workspace_id
           AND d.parent_inode = v_target_parent
           AND d.name = v_target_name) THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_EXISTS: %', p_target
            USING ERRCODE = '23505';
    END IF;

    IF v_source_kind = 'directory' THEN
        WITH RECURSIVE descendants(inode_id) AS (
            SELECT v_source_inode
            UNION ALL
            SELECT d.inode_id
              FROM _vexfs.dentries AS d
              JOIN descendants AS x ON d.parent_inode = x.inode_id
             WHERE d.workspace_id = v_workspace.workspace_id
        )
        SELECT EXISTS(
            SELECT 1 FROM descendants WHERE inode_id = v_target_parent)
          INTO v_cycle;
        IF v_cycle THEN
            RAISE EXCEPTION 'VEXFS_INVALID_MOVE: cannot move a directory into itself'
                USING ERRCODE = '22023';
        END IF;
    END IF;

    UPDATE _vexfs.dentries
       SET parent_inode = v_target_parent,
           name = v_target_name
     WHERE workspace_id = v_workspace.workspace_id
       AND parent_inode = v_source_parent
       AND name = v_source_name;
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE inode_id IN (v_source_parent, v_target_parent);
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'move', p_target, v_source_inode,
        jsonb_build_object(
            'before_version', v_source_version,
            'after_version', v_source_version,
            'before_path', p_source,
            'after_path', p_target));
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_rename(
    p_workspace text,
    p_source text,
    p_target text,
    p_replace integer DEFAULT 0)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_source_parent bigint;
    v_source_name text;
    v_target_parent bigint;
    v_target_name text;
    v_source_inode bigint;
    v_source_kind text;
    v_source_version bigint;
    v_target_inode bigint;
    v_target_kind text;
    v_cycle boolean;
BEGIN
    IF p_replace NOT IN (0, 1) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RENAME: replace must be 0 or 1'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    SELECT rp.parent_inode, rp.entry_name INTO v_source_parent, v_source_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_source) AS rp;
    SELECT rp.parent_inode, rp.entry_name INTO v_target_parent, v_target_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_target) AS rp;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_source_parent, 'write');
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_target_parent, 'write');
    SELECT d.inode_id, i.kind, i.current_version
      INTO v_source_inode, v_source_kind, v_source_version
      FROM _vexfs.dentries AS d
      JOIN _vexfs.inodes AS i ON i.inode_id = d.inode_id
     WHERE d.workspace_id = v_workspace.workspace_id
       AND d.parent_inode = v_source_parent AND d.name = v_source_name;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_PATH_NOT_FOUND: %', p_source
            USING ERRCODE = 'P0002';
    END IF;
    IF v_source_parent = v_target_parent AND v_source_name = v_target_name THEN
        RETURN 1;
    END IF;
    SELECT d.inode_id, i.kind INTO v_target_inode, v_target_kind
      FROM _vexfs.dentries AS d
      JOIN _vexfs.inodes AS i ON i.inode_id = d.inode_id
     WHERE d.workspace_id = v_workspace.workspace_id
       AND d.parent_inode = v_target_parent AND d.name = v_target_name;
    IF FOUND THEN
        IF p_replace = 0 THEN
            RAISE EXCEPTION 'VEXFS_ALREADY_EXISTS: %', p_target
                USING ERRCODE = '23505';
        END IF;
        IF (v_source_kind = 'directory') <> (v_target_kind = 'directory') THEN
            RAISE EXCEPTION 'VEXFS_RENAME_TYPE_MISMATCH: %', p_target
                USING ERRCODE = '42809';
        END IF;
        IF v_target_kind = 'directory' AND EXISTS (
            SELECT 1 FROM _vexfs.dentries AS d
             WHERE d.workspace_id = v_workspace.workspace_id
               AND d.parent_inode = v_target_inode) THEN
            RAISE EXCEPTION 'VEXFS_NOT_EMPTY: %', p_target
                USING ERRCODE = '2BP01';
        END IF;
        IF v_target_inode = v_source_inode THEN
            DELETE FROM _vexfs.dentries
             WHERE workspace_id = v_workspace.workspace_id
               AND parent_inode = v_source_parent AND name = v_source_name;
            UPDATE _vexfs.inodes
               SET modified_at = clock_timestamp(), changed_at = clock_timestamp()
             WHERE inode_id = v_source_parent;
            PERFORM _vexfs.record_commit(
                v_workspace.workspace_id, 'rename', p_target, v_source_inode,
                jsonb_build_object(
                    'before_version', v_source_version,
                    'after_version', v_source_version,
                    'before_path', p_source,
                    'after_path', p_target,
                    'replaced_inode', v_target_inode));
            RETURN 1;
        END IF;
    END IF;
    IF v_source_kind = 'directory' THEN
        WITH RECURSIVE descendants(inode_id) AS (
            SELECT v_source_inode
            UNION ALL
            SELECT d.inode_id
              FROM _vexfs.dentries AS d
              JOIN descendants AS tree ON tree.inode_id = d.parent_inode
             WHERE d.workspace_id = v_workspace.workspace_id)
        SELECT EXISTS(SELECT 1 FROM descendants WHERE inode_id = v_target_parent)
          INTO v_cycle;
        IF v_cycle THEN
            RAISE EXCEPTION 'VEXFS_INVALID_RENAME: cannot move a directory into itself'
                USING ERRCODE = '22023';
        END IF;
    END IF;
    IF v_target_inode IS NOT NULL THEN
        DELETE FROM _vexfs.dentries
         WHERE workspace_id = v_workspace.workspace_id
           AND parent_inode = v_target_parent AND name = v_target_name;
        IF v_target_kind = 'directory' OR NOT EXISTS (
            SELECT 1 FROM _vexfs.dentries AS link
             WHERE link.workspace_id = v_workspace.workspace_id
               AND link.inode_id = v_target_inode)
           AND NOT EXISTS (
               SELECT 1 FROM _vexfs.handles AS handle
                WHERE handle.workspace_id = v_workspace.workspace_id
                  AND handle.inode_id = v_target_inode
                  AND handle.state IN ('open', 'retained')) THEN
            UPDATE _vexfs.inodes
               SET live = false, changed_at = clock_timestamp()
             WHERE inode_id = v_target_inode;
        END IF;
    END IF;
    UPDATE _vexfs.dentries
       SET parent_inode = v_target_parent, name = v_target_name
     WHERE workspace_id = v_workspace.workspace_id
       AND parent_inode = v_source_parent AND name = v_source_name;
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(), changed_at = clock_timestamp()
     WHERE inode_id IN (v_source_parent, v_target_parent);
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'rename', p_target, v_source_inode,
        jsonb_build_object(
            'before_version', v_source_version,
            'after_version', v_source_version,
            'before_path', p_source,
            'after_path', p_target,
            'replaced_inode', v_target_inode));
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_remove(
    p_workspace text, p_path text, p_recursive boolean DEFAULT false)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_parent bigint;
    v_name text;
    v_inode bigint;
    v_kind text;
    v_current_version bigint;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    SELECT rp.parent_inode, rp.entry_name INTO v_parent, v_name
      FROM _vexfs.resolve_parent(v_workspace.workspace_id, p_path) AS rp;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_parent, 'write');
    SELECT d.inode_id, i.kind, i.current_version
      INTO v_inode, v_kind, v_current_version
      FROM _vexfs.dentries AS d
      JOIN _vexfs.inodes AS i ON i.inode_id = d.inode_id
     WHERE d.workspace_id = v_workspace.workspace_id
       AND d.parent_inode = v_parent
       AND d.name = v_name;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_PATH_NOT_FOUND: %', p_path
            USING ERRCODE = 'P0002';
    END IF;
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'write');
    IF v_kind = 'directory' AND NOT p_recursive AND EXISTS (
        SELECT 1 FROM _vexfs.dentries AS d
         WHERE d.workspace_id = v_workspace.workspace_id
           AND d.parent_inode = v_inode) THEN
        RAISE EXCEPTION 'VEXFS_NOT_EMPTY: %', p_path
            USING ERRCODE = '2BP01';
    END IF;

    IF v_kind = 'directory' AND p_recursive THEN
        WITH RECURSIVE subtree(inode_id) AS (
            SELECT v_inode
            UNION
            SELECT d.inode_id
              FROM _vexfs.dentries AS d
              JOIN subtree AS s ON d.parent_inode = s.inode_id
             WHERE d.workspace_id = v_workspace.workspace_id
        ),
        deleted AS (
            DELETE FROM _vexfs.dentries AS d
             WHERE d.workspace_id = v_workspace.workspace_id
               AND ((d.parent_inode = v_parent AND d.name = v_name)
                    OR d.parent_inode IN (
                        SELECT tree.inode_id FROM subtree AS tree))
            RETURNING d.inode_id
        ),
        affected AS (
            SELECT DISTINCT deleted.inode_id FROM deleted
        )
        UPDATE _vexfs.inodes AS i
           SET live = false,
               changed_at = clock_timestamp()
         WHERE i.workspace_id = v_workspace.workspace_id
           AND i.live
           AND ((i.kind = 'directory' AND i.inode_id IN (
                    SELECT tree.inode_id FROM subtree AS tree))
                OR (i.kind <> 'directory'
                    AND i.inode_id IN (
                        SELECT item.inode_id FROM affected AS item)
                    AND NOT EXISTS (
                        SELECT 1 FROM _vexfs.dentries AS remaining
                         WHERE remaining.workspace_id = v_workspace.workspace_id
                           AND remaining.inode_id = i.inode_id)
                    AND NOT EXISTS (
                        SELECT 1 FROM _vexfs.handles AS handle
                         WHERE handle.workspace_id = v_workspace.workspace_id
                           AND handle.inode_id = i.inode_id
                           AND handle.state IN ('open', 'retained'))));
    ELSE
        DELETE FROM _vexfs.dentries
         WHERE workspace_id = v_workspace.workspace_id
           AND parent_inode = v_parent
           AND name = v_name;
        IF v_kind = 'directory' OR NOT EXISTS (
            SELECT 1
              FROM _vexfs.dentries AS remaining
             WHERE remaining.workspace_id = v_workspace.workspace_id
               AND remaining.inode_id = v_inode)
           AND NOT EXISTS (
               SELECT 1 FROM _vexfs.handles AS handle
                WHERE handle.workspace_id = v_workspace.workspace_id
                  AND handle.inode_id = v_inode
                  AND handle.state IN ('open', 'retained')) THEN
            UPDATE _vexfs.inodes
               SET live = false,
                   changed_at = clock_timestamp()
             WHERE workspace_id = v_workspace.workspace_id
               AND inode_id = v_inode
               AND live;
        END IF;
    END IF;
    UPDATE _vexfs.inodes
       SET modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE inode_id = v_parent;
    PERFORM _vexfs.record_commit(
        v_workspace.workspace_id, 'remove', p_path, v_inode,
        jsonb_build_object(
            'before_version', v_current_version,
            'after_version', NULL,
            'recursive', p_recursive,
            'kind', v_kind));
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_history(
    p_workspace text,
    p_path text,
    p_limit integer DEFAULT 100,
    p_before_version bigint DEFAULT NULL)
RETURNS TABLE(
    version bigint,
    commit_no bigint,
    size bigint,
    checksum text,
    source_version bigint,
    created_by name,
    created_at timestamptz,
    current boolean)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_inode bigint;
    v_current bigint;
    v_kind text;
BEGIN
    IF p_limit < 1 OR p_limit > 1000 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_LIMIT: limit must be between 1 and 1000'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'read');
    SELECT i.kind, i.current_version INTO v_kind, v_current
      FROM _vexfs.inodes AS i WHERE i.inode_id = v_inode;
    IF v_kind = 'directory' THEN
        RAISE EXCEPTION 'VEXFS_IS_DIRECTORY: %', p_path
            USING ERRCODE = '42809';
    END IF;

    RETURN QUERY
    SELECT f.version_no,
           f.commit_no,
           f.size_bytes,
           f.checksum,
           f.source_version_no,
           coalesce(r.rolname, f.created_by)::name,
           f.created_at,
           f.version_no = v_current
      FROM _vexfs.file_versions AS f
      LEFT JOIN pg_catalog.pg_roles AS r ON r.oid = f.created_by_oid
     WHERE f.workspace_id = v_workspace.workspace_id
       AND f.inode_id = v_inode
       AND (p_before_version IS NULL OR f.version_no < p_before_version)
     ORDER BY f.version_no DESC
     LIMIT p_limit;
END;
$$;

CREATE FUNCTION public.vexfs_history_json(
    p_workspace text,
    p_path text,
    p_limit integer DEFAULT 100,
    p_before_version bigint DEFAULT 0)
RETURNS jsonb
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    WITH rows AS MATERIALIZED (
        SELECT history.*,
               row_number() OVER (ORDER BY history.version DESC) AS ordinal
          FROM public.vexfs_history(
              p_workspace, p_path, p_limit + 1,
              nullif(p_before_version, 0)) AS history
    ), page AS (
        SELECT * FROM rows WHERE ordinal <= p_limit
    )
    SELECT jsonb_build_object(
        'entries', coalesce((
            SELECT jsonb_agg(jsonb_build_object(
                       'version', item.version,
                       'commit', item.commit_no,
                       'parent_commit', commit.parent_commit,
                       'size', item.size,
                       'created_at', (extract(epoch FROM item.created_at) * 1000)::bigint,
                       'message', commit.operation,
                       'checksum', item.checksum,
                       'current', item.current)
                       ORDER BY item.version DESC)
              FROM page AS item
              JOIN _vexfs.workspaces AS workspace ON workspace.name = p_workspace
              JOIN _vexfs.commits AS commit
                ON commit.workspace_id = workspace.workspace_id
               AND commit.commit_no = item.commit_no), '[]'::jsonb),
        'next_before', CASE WHEN EXISTS (
            SELECT 1 FROM rows WHERE ordinal > p_limit)
            THEN (SELECT min(version) FROM page) ELSE NULL END)
$$;

CREATE FUNCTION public.vexfs_read_version(
    p_workspace text, p_path text, p_version bigint)
RETURNS bytea
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_inode bigint;
    v_kind text;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'read');
    SELECT i.kind INTO v_kind FROM _vexfs.inodes AS i WHERE i.inode_id = v_inode;
    IF v_kind = 'directory' THEN
        RAISE EXCEPTION 'VEXFS_IS_DIRECTORY: %', p_path
            USING ERRCODE = '42809';
    END IF;
    RETURN _vexfs.read_version_content(
        v_workspace.workspace_id, v_inode, p_version);
END;
$$;

CREATE FUNCTION public.vexfs_restore_version(
    p_workspace text,
    p_path text,
    p_version bigint,
    p_expected_version bigint)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_inode bigint;
    v_kind text;
    v_current bigint;
    v_new_version bigint;
    v_commit bigint;
    v_size bigint;
    v_checksum text;
    v_canonical bigint;
    v_principal_oid oid;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    PERFORM 1 FROM _vexfs.workspaces
     WHERE workspace_id = v_workspace.workspace_id FOR UPDATE;
    v_inode := _vexfs.resolve_path(v_workspace.workspace_id, p_path);
    PERFORM _vexfs.require_inode_permission(
        v_workspace.workspace_id, v_inode, 'write');
    SELECT i.kind, i.current_version INTO v_kind, v_current
      FROM _vexfs.inodes AS i WHERE i.inode_id = v_inode FOR UPDATE;
    IF v_kind = 'directory' THEN
        RAISE EXCEPTION 'VEXFS_IS_DIRECTORY: %', p_path
            USING ERRCODE = '42809';
    END IF;
    IF v_current <> p_expected_version THEN
        RAISE EXCEPTION 'VEXFS_VERSION_CONFLICT: expected %, actual %',
            p_expected_version, v_current
            USING ERRCODE = '40001';
    END IF;
    SELECT storage.size_bytes,
           storage.checksum,
           storage.canonical_version
      INTO v_size, v_checksum, v_canonical
      FROM _vexfs.resolve_version_storage(
          v_workspace.workspace_id, v_inode, p_version) AS storage;

    SELECT r.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    SELECT coalesce(max(f.version_no), 0) + 1 INTO v_new_version
      FROM _vexfs.file_versions AS f
     WHERE f.workspace_id = v_workspace.workspace_id
       AND f.inode_id = v_inode;
    v_commit := _vexfs.record_commit(
        v_workspace.workspace_id, 'restore_version', p_path, v_inode,
        jsonb_build_object(
            'before_version', v_current,
            'after_version', v_new_version,
            'source_version', p_version,
            'canonical_source_version', v_canonical));
    INSERT INTO _vexfs.file_versions(
        workspace_id, inode_id, version_no, commit_no, source_version_no,
        size_bytes, checksum, created_by_oid, created_by)
    VALUES (
        v_workspace.workspace_id, v_inode, v_new_version, v_commit, v_canonical,
        v_size, v_checksum, v_principal_oid, session_user);
    UPDATE _vexfs.inodes
       SET current_version = v_new_version,
           size_bytes = v_size,
           modified_at = clock_timestamp(),
           changed_at = clock_timestamp()
     WHERE inode_id = v_inode;
    RETURN v_new_version;
END;
$$;

CREATE FUNCTION public.vexfs_snapshot_create(
    p_workspace text,
    p_name text,
    p_expected_head bigint DEFAULT NULL,
    p_mode text DEFAULT 'consistent')
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_head bigint;
    v_snapshot_id bigint;
    v_principal_oid oid;
BEGIN
    IF p_name IS NULL OR btrim(p_name) = '' OR p_name <> btrim(p_name) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SNAPSHOT: invalid snapshot name'
            USING ERRCODE = '22023';
    END IF;
    IF p_mode NOT IN ('consistent', 'committed-only') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SNAPSHOT_MODE: use consistent or committed-only'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT w.head_commit INTO v_head
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = v_workspace.workspace_id
     FOR UPDATE;
    IF p_expected_head IS NOT NULL AND p_expected_head <> v_head THEN
        RAISE EXCEPTION 'VEXFS_HEAD_CONFLICT: expected %, actual %',
            p_expected_head, v_head
            USING ERRCODE = '40001';
    END IF;
    IF p_mode = 'consistent' AND EXISTS (
        SELECT 1 FROM _vexfs.handles AS handle
         WHERE handle.workspace_id = v_workspace.workspace_id
           AND handle.state IN ('open', 'retained')
           AND handle.dirty_generation > handle.published_generation) THEN
        RAISE EXCEPTION 'VEXFS_UNPUBLISHED_HANDLES: synchronize the owning mount or use committed-only'
            USING ERRCODE = '55006';
    END IF;
    SELECT r.oid INTO STRICT v_principal_oid
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    INSERT INTO _vexfs.snapshots(
        workspace_id, name, head_commit, created_by_oid, created_by)
    VALUES (
        v_workspace.workspace_id, p_name, v_head,
        v_principal_oid, session_user)
    RETURNING snapshot_id INTO v_snapshot_id;

    WITH RECURSIVE reachable(inode_id) AS (
        SELECT v_workspace.root_inode
        UNION
        SELECT d.inode_id
          FROM _vexfs.dentries AS d
          JOIN reachable AS r ON d.parent_inode = r.inode_id
         WHERE d.workspace_id = v_workspace.workspace_id
    )
    INSERT INTO _vexfs.snapshot_inodes(
        snapshot_id, inode_id, kind, mode, owner_oid, owner_role, owner_principal,
        uid, gid, acl_set_id, current_version, size_bytes, created_at, accessed_at,
        modified_at, changed_at)
    SELECT v_snapshot_id,
           i.inode_id,
           i.kind,
           i.mode,
           i.owner_oid,
           i.owner_role,
           i.owner_principal,
           i.uid,
           i.gid,
           i.acl_set_id,
           i.current_version,
           i.size_bytes,
           i.created_at,
           i.accessed_at,
           i.modified_at,
           i.changed_at
      FROM reachable AS x
      JOIN _vexfs.inodes AS i ON i.inode_id = x.inode_id;

    INSERT INTO _vexfs.snapshot_dentries(
        snapshot_id, parent_inode, name, inode_id)
    SELECT v_snapshot_id, d.parent_inode, d.name, d.inode_id
      FROM _vexfs.dentries AS d
      JOIN _vexfs.snapshot_inodes AS p
        ON p.snapshot_id = v_snapshot_id AND p.inode_id = d.parent_inode
      JOIN _vexfs.snapshot_inodes AS c
        ON c.snapshot_id = v_snapshot_id AND c.inode_id = d.inode_id
     WHERE d.workspace_id = v_workspace.workspace_id;

    INSERT INTO _vexfs.snapshot_xattrs(
        snapshot_id, inode_id, name, value, updated_at)
    SELECT v_snapshot_id, x.inode_id, x.name, x.value, x.updated_at
      FROM _vexfs.xattrs AS x
      JOIN _vexfs.snapshot_inodes AS i
        ON i.snapshot_id = v_snapshot_id AND i.inode_id = x.inode_id
     WHERE x.workspace_id = v_workspace.workspace_id;

    PERFORM _vexfs.audit(
        v_workspace.workspace_id, v_head, 'snapshot_create',
        '/', v_workspace.root_inode,
        jsonb_build_object(
            'before_version', v_head,
            'after_version', v_head,
            'snapshot', p_name,
            'mode', p_mode));
    RETURN v_head;
END;
$$;

CREATE FUNCTION public.vexfs_snapshot_list(p_workspace text)
RETURNS TABLE(
    name text,
    head_commit bigint,
    created_by name,
    created_at timestamptz)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    RETURN QUERY
    SELECT s.name,
           s.head_commit,
           coalesce(r.rolname, s.created_by)::name,
           s.created_at
      FROM _vexfs.snapshots AS s
      LEFT JOIN pg_catalog.pg_roles AS r ON r.oid = s.created_by_oid
     WHERE s.workspace_id = v_workspace.workspace_id
     ORDER BY s.created_at DESC, s.snapshot_id DESC;
END;
$$;

CREATE FUNCTION public.vexfs_snapshot_list_json(p_workspace text)
RETURNS jsonb
LANGUAGE sql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
    SELECT coalesce(jsonb_agg(jsonb_build_object(
               'name', snapshot.name,
               'commit', snapshot.head_commit,
               'created_by', snapshot.created_by,
               'created_at', (extract(epoch FROM snapshot.created_at) * 1000)::bigint)
               ORDER BY snapshot.created_at DESC), '[]'::jsonb)
      FROM public.vexfs_snapshot_list(p_workspace) AS snapshot
$$;

CREATE FUNCTION public.vexfs_snapshot_show(p_workspace text, p_name text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_result jsonb;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    SELECT jsonb_build_object(
               'name', s.name,
               'commit', s.head_commit,
               'head_commit', s.head_commit,
               'inode_count', (SELECT count(*) FROM _vexfs.snapshot_inodes AS i
                                WHERE i.snapshot_id = s.snapshot_id),
               'dentry_count', (SELECT count(*) FROM _vexfs.snapshot_dentries AS d
                                 WHERE d.snapshot_id = s.snapshot_id),
               'xattr_count', (SELECT count(*) FROM _vexfs.snapshot_xattrs AS x
                                WHERE x.snapshot_id = s.snapshot_id),
               'acl_count', (SELECT count(*) FROM _vexfs.snapshot_acl_entries AS acl
                              WHERE acl.snapshot_id = s.snapshot_id),
               'created_by', coalesce(r.rolname, s.created_by),
               'created_at', (extract(epoch FROM s.created_at) * 1000)::bigint,
               'entries', (SELECT coalesce(jsonb_agg(jsonb_build_object(
                   'path', entry.path,
                   'inode', entry.inode_id,
                   'kind', entry.kind,
                   'mode', entry.mode,
                   'owner_principal', entry.owner_principal,
                   'uid', entry.uid,
                   'gid', entry.gid,
                   'size', entry.size_bytes,
                   'version', entry.current_version,
                   'checksum', entry.content_checksum)
                   ORDER BY entry.path), '[]'::jsonb)
                 FROM _vexfs.tree_at(
                     v_workspace.workspace_id, s.snapshot_id) AS entry))
      INTO v_result
      FROM _vexfs.snapshots AS s
      LEFT JOIN pg_catalog.pg_roles AS r ON r.oid = s.created_by_oid
     WHERE s.workspace_id = v_workspace.workspace_id
       AND s.name = p_name;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_SNAPSHOT_NOT_FOUND: %', p_name
            USING ERRCODE = 'P0002';
    END IF;
    RETURN v_result;
END;
$$;

CREATE FUNCTION _vexfs.tree_at(
    p_workspace_id bigint,
    p_snapshot_id bigint DEFAULT NULL)
RETURNS TABLE(
    path text,
    inode_id bigint,
    kind text,
    mode integer,
    owner_principal text,
    uid bigint,
    gid bigint,
    size_bytes bigint,
    current_version bigint,
    content_checksum text,
    metadata_checksum text)
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_root bigint;
BEGIN
    SELECT w.root_inode INTO STRICT v_root
      FROM _vexfs.workspaces AS w WHERE w.workspace_id = p_workspace_id;
    IF p_snapshot_id IS NULL THEN
        RETURN QUERY
        WITH RECURSIVE tree(path, inode_id) AS (
            SELECT '/'::text, v_root
            UNION ALL
            SELECT CASE WHEN tree.path = '/' THEN '/' || d.name
                        ELSE tree.path || '/' || d.name END,
                   d.inode_id
              FROM tree
              JOIN _vexfs.dentries AS d
                ON d.workspace_id = p_workspace_id
               AND d.parent_inode = tree.inode_id),
        metadata AS (
            SELECT i.inode_id,
                   encode(pg_catalog.sha256(convert_to(jsonb_build_object(
                       'mode', i.mode,
                       'owner', i.owner_principal,
                       'uid', i.uid,
                       'gid', i.gid,
                       'created_at', i.created_at,
                       'accessed_at', i.accessed_at,
                       'modified_at', i.modified_at,
                       'changed_at', i.changed_at,
                       'xattrs', coalesce((
                           SELECT jsonb_agg(jsonb_build_array(x.name, encode(x.value, 'hex'))
                                            ORDER BY x.name)
                             FROM _vexfs.xattrs AS x
                            WHERE x.workspace_id = p_workspace_id
                              AND x.inode_id = i.inode_id), '[]'::jsonb),
                       'acl', coalesce((
                           SELECT jsonb_agg(jsonb_build_array(
                                      acl.principal, acl.effect, acl.permissions,
                                      acl.inherit_flags)
                                    ORDER BY acl.principal, acl.effect)
                             FROM _vexfs.acl_entries AS acl
                            WHERE acl.workspace_id = p_workspace_id
                              AND acl.inode_id = i.inode_id), '[]'::jsonb))::text, 'UTF8')), 'hex')
                       AS metadata_checksum
              FROM _vexfs.inodes AS i
             WHERE i.workspace_id = p_workspace_id AND i.live)
        SELECT tree.path,
               i.inode_id,
               i.kind,
               i.mode,
               i.owner_principal,
               i.uid,
               i.gid,
               i.size_bytes,
               i.current_version,
               f.checksum,
               metadata.metadata_checksum
          FROM tree
          JOIN _vexfs.inodes AS i ON i.inode_id = tree.inode_id
          JOIN metadata ON metadata.inode_id = i.inode_id
          LEFT JOIN _vexfs.file_versions AS f
            ON f.workspace_id = p_workspace_id
           AND f.inode_id = i.inode_id
           AND f.version_no = i.current_version;
    ELSE
        RETURN QUERY
        WITH RECURSIVE tree(path, inode_id) AS (
            SELECT '/'::text, v_root
            UNION ALL
            SELECT CASE WHEN tree.path = '/' THEN '/' || d.name
                        ELSE tree.path || '/' || d.name END,
                   d.inode_id
              FROM tree
              JOIN _vexfs.snapshot_dentries AS d
                ON d.snapshot_id = p_snapshot_id
               AND d.parent_inode = tree.inode_id),
        metadata AS (
            SELECT i.inode_id,
                   encode(pg_catalog.sha256(convert_to(jsonb_build_object(
                       'mode', i.mode,
                       'owner', i.owner_principal,
                       'uid', i.uid,
                       'gid', i.gid,
                       'created_at', i.created_at,
                       'accessed_at', i.accessed_at,
                       'modified_at', i.modified_at,
                       'changed_at', i.changed_at,
                       'xattrs', coalesce((
                           SELECT jsonb_agg(jsonb_build_array(x.name, encode(x.value, 'hex'))
                                            ORDER BY x.name)
                             FROM _vexfs.snapshot_xattrs AS x
                            WHERE x.snapshot_id = p_snapshot_id
                              AND x.inode_id = i.inode_id), '[]'::jsonb),
                       'acl', coalesce((
                           SELECT jsonb_agg(jsonb_build_array(
                                      acl.principal, acl.effect, acl.permissions,
                                      acl.inherit_flags)
                                    ORDER BY acl.principal, acl.effect)
                             FROM _vexfs.snapshot_acl_entries AS acl
                            WHERE acl.snapshot_id = p_snapshot_id
                              AND acl.inode_id = i.inode_id), '[]'::jsonb))::text, 'UTF8')), 'hex')
                       AS metadata_checksum
              FROM _vexfs.snapshot_inodes AS i
             WHERE i.snapshot_id = p_snapshot_id)
        SELECT tree.path,
               i.inode_id,
               i.kind,
               i.mode,
               i.owner_principal,
               i.uid,
               i.gid,
               i.size_bytes,
               i.current_version,
               f.checksum,
               metadata.metadata_checksum
          FROM tree
          JOIN _vexfs.snapshot_inodes AS i
            ON i.snapshot_id = p_snapshot_id AND i.inode_id = tree.inode_id
          JOIN metadata ON metadata.inode_id = i.inode_id
          LEFT JOIN _vexfs.file_versions AS f
            ON f.workspace_id = p_workspace_id
           AND f.inode_id = i.inode_id
           AND f.version_no = i.current_version;
    END IF;
END;
$$;

CREATE FUNCTION public.vexfs_snapshot_diff(
    p_workspace text,
    p_from text,
    p_to text DEFAULT 'HEAD')
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_from_snapshot bigint;
    v_to_snapshot bigint;
    v_changes jsonb;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    IF p_from <> 'HEAD' THEN
        SELECT s.snapshot_id INTO v_from_snapshot
          FROM _vexfs.snapshots AS s
         WHERE s.workspace_id = v_workspace.workspace_id AND s.name = p_from;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'VEXFS_SNAPSHOT_NOT_FOUND: %', p_from
                USING ERRCODE = 'P0002';
        END IF;
    END IF;
    IF p_to <> 'HEAD' THEN
        SELECT s.snapshot_id INTO v_to_snapshot
          FROM _vexfs.snapshots AS s
         WHERE s.workspace_id = v_workspace.workspace_id AND s.name = p_to;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'VEXFS_SNAPSHOT_NOT_FOUND: %', p_to
                USING ERRCODE = 'P0002';
        END IF;
    END IF;
    WITH old_tree AS (
        SELECT * FROM _vexfs.tree_at(v_workspace.workspace_id, v_from_snapshot)),
    new_tree AS (
        SELECT * FROM _vexfs.tree_at(v_workspace.workspace_id, v_to_snapshot)),
    changes AS (
        SELECT coalesce(old_tree.path, new_tree.path) AS path,
               CASE WHEN old_tree.path IS NULL THEN 'added'
                    WHEN new_tree.path IS NULL THEN 'removed'
                    ELSE 'modified' END AS change,
               old_tree.kind AS old_kind,
               new_tree.kind AS new_kind,
               old_tree.size_bytes AS old_size,
               new_tree.size_bytes AS new_size,
               old_tree.current_version AS old_version,
               new_tree.current_version AS new_version
          FROM old_tree
          FULL JOIN new_tree USING (path)
         WHERE old_tree.path IS NULL
            OR new_tree.path IS NULL
            OR old_tree.kind IS DISTINCT FROM new_tree.kind
            OR old_tree.size_bytes IS DISTINCT FROM new_tree.size_bytes
            OR old_tree.content_checksum IS DISTINCT FROM new_tree.content_checksum
            OR old_tree.metadata_checksum IS DISTINCT FROM new_tree.metadata_checksum)
    SELECT coalesce(jsonb_agg(jsonb_build_object(
               'path', changes.path,
               'change', changes.change,
               'old_kind', changes.old_kind,
               'new_kind', changes.new_kind,
               'old_size', changes.old_size,
               'new_size', changes.new_size,
               'old_version', changes.old_version,
               'new_version', changes.new_version)
               ORDER BY changes.path), '[]'::jsonb)
      INTO v_changes
      FROM changes;
    RETURN jsonb_build_object(
        'workspace', p_workspace,
        'from', p_from,
        'to', p_to,
        'changes', v_changes);
END;
$$;

CREATE FUNCTION public.vexfs_snapshot_drop(p_workspace text, p_name text)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_snapshot_head bigint;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    DELETE FROM _vexfs.snapshots
     WHERE workspace_id = v_workspace.workspace_id
       AND name = p_name
    RETURNING head_commit INTO v_snapshot_head;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_SNAPSHOT_NOT_FOUND: %', p_name
            USING ERRCODE = 'P0002';
    END IF;
    PERFORM _vexfs.audit(
        v_workspace.workspace_id, v_workspace.head_commit, 'snapshot_drop',
        '/', v_workspace.root_inode,
        jsonb_build_object(
            'before_version', v_workspace.head_commit,
            'after_version', v_workspace.head_commit,
            'snapshot', p_name,
            'snapshot_version', v_snapshot_head));
    RETURN 1;
END;
$$;

CREATE FUNCTION public.vexfs_snapshot_restore(
    p_workspace text,
    p_name text,
    p_expected_head bigint,
    p_safety_name text DEFAULT NULL,
    p_caller_session_id text DEFAULT NULL)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_current_head bigint;
    v_snapshot_id bigint;
    v_snapshot_head bigint;
    v_commit bigint;
    v_same boolean;
    v_actor oid;
    v_active_mounts bigint;
BEGIN
    IF p_safety_name IS NOT NULL AND
       (btrim(p_safety_name) = '' OR
        p_safety_name <> btrim(p_safety_name) OR
        octet_length(p_safety_name) > 128 OR
        p_safety_name = 'HEAD') THEN
        RAISE EXCEPTION 'VEXFS_INVALID_SNAPSHOT: invalid safety snapshot name'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'write');
    SELECT role.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS role
     WHERE role.rolname = session_user;
    SELECT w.head_commit INTO v_current_head
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = v_workspace.workspace_id
     FOR UPDATE;
    IF v_current_head <> p_expected_head THEN
        RAISE EXCEPTION 'VEXFS_HEAD_CONFLICT: expected %, actual %',
            p_expected_head, v_current_head
            USING ERRCODE = '40001';
    END IF;
    IF p_caller_session_id IS NOT NULL AND NOT EXISTS (
        SELECT 1
          FROM _vexfs.mount_sessions AS caller
         WHERE caller.workspace_id = v_workspace.workspace_id
           AND caller.session_id = p_caller_session_id
           AND caller.owner_oid = v_actor
           AND caller.lease_until > clock_timestamp()) THEN
        RAISE EXCEPTION 'VEXFS_SESSION_STALE: restore caller session is not active'
            USING ERRCODE = '55006';
    END IF;
    SELECT count(*) INTO v_active_mounts
      FROM _vexfs.mount_sessions AS mounted
     WHERE mounted.workspace_id = v_workspace.workspace_id
       AND mounted.lease_until > clock_timestamp()
       AND (p_caller_session_id IS NULL OR
            mounted.session_id <> p_caller_session_id);
    IF v_active_mounts > 0 THEN
        RAISE EXCEPTION
            'VEXFS_MOUNT_BUSY: workspace has % other active mount session(s); unmount all machines before snapshot restore',
            v_active_mounts
            USING ERRCODE = '55006';
    END IF;
    IF EXISTS (
        SELECT 1
          FROM _vexfs.handles AS handle
         WHERE handle.workspace_id = v_workspace.workspace_id
           AND handle.state IN ('open', 'retained')) THEN
        RAISE EXCEPTION
            'VEXFS_OPEN_HANDLES: close or recover all file handles before snapshot restore'
            USING ERRCODE = '55006';
    END IF;
    SELECT s.snapshot_id, s.head_commit INTO v_snapshot_id, v_snapshot_head
      FROM _vexfs.snapshots AS s
     WHERE s.workspace_id = v_workspace.workspace_id
       AND s.name = p_name
     FOR SHARE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_SNAPSHOT_NOT_FOUND: %', p_name
            USING ERRCODE = 'P0002';
    END IF;
    IF v_snapshot_head = v_current_head THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_MATCHES: snapshot already matches workspace head'
            USING ERRCODE = '22000';
    END IF;

    IF EXISTS (
        SELECT 1
          FROM _vexfs.snapshot_inodes AS s
          LEFT JOIN _vexfs.file_versions AS source
            ON source.workspace_id = v_workspace.workspace_id
           AND source.inode_id = s.inode_id
           AND source.version_no = s.current_version
         WHERE s.snapshot_id = v_snapshot_id
           AND s.kind <> 'directory'
           AND source.version_no IS NULL) THEN
        RAISE EXCEPTION 'VEXFS_CORRUPT: snapshot references a missing file version'
            USING ERRCODE = 'XX001';
    END IF;

    SELECT
        NOT EXISTS (
            (SELECT d.parent_inode, d.name, d.inode_id
               FROM _vexfs.dentries AS d
              WHERE d.workspace_id = v_workspace.workspace_id)
            EXCEPT
            (SELECT d.parent_inode, d.name, d.inode_id
               FROM _vexfs.snapshot_dentries AS d
              WHERE d.snapshot_id = v_snapshot_id))
        AND NOT EXISTS (
            (SELECT d.parent_inode, d.name, d.inode_id
               FROM _vexfs.snapshot_dentries AS d
              WHERE d.snapshot_id = v_snapshot_id)
            EXCEPT
            (SELECT d.parent_inode, d.name, d.inode_id
               FROM _vexfs.dentries AS d
              WHERE d.workspace_id = v_workspace.workspace_id))
        AND NOT EXISTS (
            SELECT 1
              FROM _vexfs.snapshot_inodes AS s
              LEFT JOIN _vexfs.inodes AS i
                ON i.workspace_id = v_workspace.workspace_id
               AND i.inode_id = s.inode_id
              LEFT JOIN _vexfs.file_versions AS current_version
                ON current_version.workspace_id = v_workspace.workspace_id
               AND current_version.inode_id = i.inode_id
               AND current_version.version_no = i.current_version
              LEFT JOIN _vexfs.file_versions AS source_version
                ON source_version.workspace_id = v_workspace.workspace_id
               AND source_version.inode_id = s.inode_id
               AND source_version.version_no = s.current_version
             WHERE s.snapshot_id = v_snapshot_id
               AND (i.inode_id IS NULL
                    OR i.kind IS DISTINCT FROM s.kind
                    OR i.mode IS DISTINCT FROM s.mode
                    OR i.owner_oid IS DISTINCT FROM s.owner_oid
                    OR i.owner_principal IS DISTINCT FROM s.owner_principal
                    OR i.uid IS DISTINCT FROM s.uid
                    OR i.gid IS DISTINCT FROM s.gid
                    OR i.size_bytes IS DISTINCT FROM s.size_bytes
                    OR i.created_at IS DISTINCT FROM s.created_at
                    OR i.accessed_at IS DISTINCT FROM s.accessed_at
                    OR i.modified_at IS DISTINCT FROM s.modified_at
                    OR i.changed_at IS DISTINCT FROM s.changed_at
                    OR (s.kind <> 'directory'
                        AND (current_version.size_bytes IS DISTINCT FROM source_version.size_bytes
                             OR current_version.checksum IS DISTINCT FROM source_version.checksum))))
        AND NOT EXISTS (
            (SELECT x.inode_id, x.name, x.value
               FROM _vexfs.xattrs AS x
              WHERE x.workspace_id = v_workspace.workspace_id)
            EXCEPT
            (SELECT x.inode_id, x.name, x.value
               FROM _vexfs.snapshot_xattrs AS x
              WHERE x.snapshot_id = v_snapshot_id))
        AND NOT EXISTS (
            (SELECT x.inode_id, x.name, x.value
               FROM _vexfs.snapshot_xattrs AS x
              WHERE x.snapshot_id = v_snapshot_id)
            EXCEPT
            (SELECT x.inode_id, x.name, x.value
               FROM _vexfs.xattrs AS x
              WHERE x.workspace_id = v_workspace.workspace_id))
        AND NOT EXISTS (
            (SELECT acl.inode_id, acl.principal, acl.effect,
                    acl.permissions, acl.inherit_flags
               FROM _vexfs.acl_entries AS acl
              WHERE acl.workspace_id = v_workspace.workspace_id)
            EXCEPT
            (SELECT acl.inode_id, acl.principal, acl.effect,
                    acl.permissions, acl.inherit_flags
               FROM _vexfs.snapshot_acl_entries AS acl
              WHERE acl.snapshot_id = v_snapshot_id))
        AND NOT EXISTS (
            (SELECT acl.inode_id, acl.principal, acl.effect,
                    acl.permissions, acl.inherit_flags
               FROM _vexfs.snapshot_acl_entries AS acl
              WHERE acl.snapshot_id = v_snapshot_id)
            EXCEPT
            (SELECT acl.inode_id, acl.principal, acl.effect,
                    acl.permissions, acl.inherit_flags
               FROM _vexfs.acl_entries AS acl
              WHERE acl.workspace_id = v_workspace.workspace_id))
      INTO v_same;
    IF v_same THEN
        RAISE EXCEPTION 'VEXFS_ALREADY_MATCHES: snapshot already matches workspace state'
            USING ERRCODE = '22000';
    END IF;

    PERFORM _vexfs.enforce_snapshot_quota(
        v_workspace.workspace_id, v_snapshot_id);
    IF p_safety_name IS NOT NULL THEN
        -- This call reuses the same row lock and transaction. If any later
        -- restore step fails, PostgreSQL rolls the safety snapshot back too.
        PERFORM public.vexfs_snapshot_create(
            p_workspace, p_safety_name, v_current_head, 'consistent');
    END IF;
    v_commit := _vexfs.record_commit(
        v_workspace.workspace_id, 'snapshot_restore', '/', v_workspace.root_inode,
        jsonb_build_object(
            'before_version', v_current_head,
            'after_version', v_current_head + 1,
            'snapshot', p_name,
            'snapshot_version', v_snapshot_head,
            'safety_snapshot', p_safety_name));
    DELETE FROM _vexfs.dentries
     WHERE workspace_id = v_workspace.workspace_id;
    INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
    SELECT v_workspace.workspace_id, d.parent_inode, d.name, d.inode_id
      FROM _vexfs.snapshot_dentries AS d
     WHERE d.snapshot_id = v_snapshot_id;
    UPDATE _vexfs.inodes
       SET live = false
     WHERE workspace_id = v_workspace.workspace_id
       AND live;
    UPDATE _vexfs.inodes AS i
       SET kind = s.kind,
           mode = s.mode,
           owner_oid = s.owner_oid,
           owner_role = s.owner_role,
           owner_principal = s.owner_principal,
           uid = s.uid,
           gid = s.gid,
           acl_set_id = s.acl_set_id,
           size_bytes = s.size_bytes,
           live = true,
           created_at = s.created_at,
           accessed_at = s.accessed_at,
           modified_at = s.modified_at,
           changed_at = s.changed_at
      FROM _vexfs.snapshot_inodes AS s
     WHERE s.snapshot_id = v_snapshot_id
       AND i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = s.inode_id;
    WITH restored AS (
        INSERT INTO _vexfs.file_versions(
            workspace_id, inode_id, version_no, commit_no, source_version_no,
            size_bytes, checksum, created_by_oid, created_by)
        SELECT v_workspace.workspace_id,
               s.inode_id,
               (SELECT coalesce(max(all_versions.version_no), 0) + 1
                  FROM _vexfs.file_versions AS all_versions
                 WHERE all_versions.workspace_id = v_workspace.workspace_id
                   AND all_versions.inode_id = s.inode_id),
               v_commit,
               coalesce(source.source_version_no, source.version_no),
               source.size_bytes,
               source.checksum,
               (SELECT r.oid FROM pg_catalog.pg_roles AS r
                 WHERE r.rolname = session_user),
               session_user
          FROM _vexfs.snapshot_inodes AS s
          JOIN _vexfs.inodes AS current_inode
            ON current_inode.workspace_id = v_workspace.workspace_id
           AND current_inode.inode_id = s.inode_id
          JOIN _vexfs.file_versions AS source
            ON source.workspace_id = v_workspace.workspace_id
           AND source.inode_id = s.inode_id
           AND source.version_no = s.current_version
          JOIN _vexfs.file_versions AS current_content
            ON current_content.workspace_id = v_workspace.workspace_id
           AND current_content.inode_id = current_inode.inode_id
           AND current_content.version_no = current_inode.current_version
         WHERE s.snapshot_id = v_snapshot_id
           AND s.kind <> 'directory'
           AND (current_content.size_bytes IS DISTINCT FROM source.size_bytes
                OR current_content.checksum IS DISTINCT FROM source.checksum)
        RETURNING inode_id, version_no
    )
    UPDATE _vexfs.inodes AS i
       SET current_version = r.version_no
      FROM restored AS r
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.inode_id = r.inode_id;

    DELETE FROM _vexfs.xattrs
     WHERE workspace_id = v_workspace.workspace_id;
    INSERT INTO _vexfs.xattrs(
        workspace_id, inode_id, name, value, updated_at)
    SELECT v_workspace.workspace_id, x.inode_id, x.name, x.value, x.updated_at
      FROM _vexfs.snapshot_xattrs AS x
     WHERE x.snapshot_id = v_snapshot_id;

    RETURN v_commit;
END;
$$;

CREATE FUNCTION public.vexfs_quota_get(p_workspace text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    RETURN _vexfs.quota_status(v_workspace.workspace_id, p_workspace);
END;
$$;

CREATE FUNCTION public.vexfs_quota_set(
    p_workspace text,
    p_max_bytes bigint,
    p_max_files bigint,
    p_max_file_bytes bigint)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    IF p_max_bytes < 0 OR p_max_files < 0 OR p_max_file_bytes < 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_QUOTA: quota values must be non-negative or null'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    UPDATE _vexfs.workspaces
       SET quota_max_bytes = p_max_bytes,
           quota_max_files = p_max_files,
           quota_max_file_bytes = p_max_file_bytes
     WHERE workspace_id = v_workspace.workspace_id;
    RETURN _vexfs.quota_status(v_workspace.workspace_id, p_workspace);
END;
$$;

CREATE FUNCTION public.vexfs_retention_get(p_workspace text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');
    RETURN _vexfs.retention_status(v_workspace.workspace_id, p_workspace);
END;
$$;

CREATE FUNCTION public.vexfs_retention_set(
    p_workspace text,
    p_keep_versions integer,
    p_keep_days integer)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    IF p_keep_versions IS NULL OR p_keep_versions < 0 OR p_keep_versions > 1000000 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RETENTION: keep_versions must be between 0 and 1000000'
            USING ERRCODE = '22023';
    END IF;
    IF p_keep_days IS NULL OR p_keep_days < 0 OR p_keep_days > 36500 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_RETENTION: keep_days must be between 0 and 36500'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    UPDATE _vexfs.workspaces
       SET retention_keep_versions = p_keep_versions,
           retention_keep_days = p_keep_days
     WHERE workspace_id = v_workspace.workspace_id;
    RETURN _vexfs.retention_status(v_workspace.workspace_id, p_workspace);
END;
$$;

CREATE FUNCTION public.vexfs_gc_pause(
    p_workspace text,
    p_paused integer)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
BEGIN
    IF p_paused NOT IN (0, 1) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_GC_STATE: paused must be 0 or 1'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    UPDATE _vexfs.workspaces
       SET gc_paused = p_paused = 1
     WHERE workspace_id = v_workspace.workspace_id;
    RETURN _vexfs.retention_status(v_workspace.workspace_id, p_workspace);
END;
$$;

CREATE FUNCTION public.vexfs_gc(
    p_workspace text,
    p_batch integer)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_gc_paused boolean;
    v_inodes bigint[];
    v_versions bigint[];
    v_manifests bigint[];
    v_deleted_versions bigint := 0;
    v_reclaimed_bytes bigint := 0;
    v_remaining jsonb;
BEGIN
    IF p_batch IS NULL OR p_batch < 1 OR p_batch > 10000 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_GC_BATCH: batch must be between 1 and 10000'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    SELECT w.gc_paused INTO v_gc_paused
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = v_workspace.workspace_id
     FOR UPDATE;
    IF v_gc_paused THEN
        RAISE EXCEPTION 'VEXFS_GC_PAUSED: workspace GC is paused'
            USING ERRCODE = '55006';
    END IF;

    SELECT array_agg(candidate.inode_id ORDER BY candidate.ordinal),
           array_agg(candidate.version_no ORDER BY candidate.ordinal),
           array_agg(candidate.manifest_id ORDER BY candidate.ordinal)
      INTO v_inodes, v_versions, v_manifests
      FROM (
          SELECT f.inode_id,
                 f.version_no,
                 f.manifest_id,
                 row_number() OVER (
                     ORDER BY f.source_version_no IS NULL,
                              f.created_at,
                              f.inode_id,
                              f.version_no) AS ordinal
            FROM _vexfs.file_versions AS f
            LEFT JOIN _vexfs.retention_keep_versions(
                v_workspace.workspace_id) AS keep
              ON keep.inode_id = f.inode_id
             AND keep.version_no = f.version_no
           WHERE f.workspace_id = v_workspace.workspace_id
             AND keep.inode_id IS NULL
             AND (f.source_version_no IS NOT NULL OR NOT EXISTS (
                 SELECT 1
                   FROM _vexfs.file_versions AS alias
                  WHERE alias.workspace_id = f.workspace_id
                    AND alias.inode_id = f.inode_id
                    AND alias.source_version_no = f.version_no))
           ORDER BY f.source_version_no IS NULL,
                    f.created_at,
                    f.inode_id,
                    f.version_no
           LIMIT p_batch
      ) AS candidate;

    v_deleted_versions := coalesce(array_length(v_versions, 1), 0);
    IF v_deleted_versions > 0 THEN
        DELETE FROM _vexfs.file_versions AS f
         USING unnest(v_inodes, v_versions) AS candidate(inode_id, version_no)
         WHERE f.workspace_id = v_workspace.workspace_id
           AND f.inode_id = candidate.inode_id
           AND f.version_no = candidate.version_no;

        DELETE FROM _vexfs.manifests AS m
         WHERE m.workspace_id = v_workspace.workspace_id
           AND m.manifest_id = ANY(array_remove(v_manifests, NULL))
           AND NOT EXISTS (
               SELECT 1 FROM _vexfs.file_versions AS f
                WHERE f.manifest_id = m.manifest_id);

        WITH removed AS (
            DELETE FROM _vexfs.chunks AS c
             WHERE c.workspace_id = v_workspace.workspace_id
               AND NOT EXISTS (
                   SELECT 1 FROM _vexfs.manifest_chunks AS entry
                    WHERE entry.chunk_id = c.chunk_id)
            RETURNING c.size_bytes
        )
        SELECT coalesce(sum(removed.size_bytes), 0)
          INTO v_reclaimed_bytes
          FROM removed;
    END IF;

    v_remaining := _vexfs.retention_status(
        v_workspace.workspace_id, p_workspace);
    RETURN jsonb_build_object(
        'workspace', p_workspace,
        'batch', p_batch,
        'deleted_versions', v_deleted_versions,
        'reclaimed_bytes', v_reclaimed_bytes,
        'remaining_versions', (v_remaining->>'stored_versions')::bigint,
        'remaining_reclaimable_versions',
            (v_remaining->>'reclaimable_versions')::bigint,
        'remaining_reclaimable_bytes',
            (v_remaining->>'reclaimable_bytes')::bigint,
        'has_more', (v_remaining->>'reclaimable_versions')::bigint > 0);
END;
$$;

CREATE FUNCTION public.vexfs_check(
    p_workspace text,
    p_deep integer DEFAULT 1)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
STABLE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_issues jsonb := '[]'::jsonb;
    v_bad bigint;
    v_inode_count bigint;
    v_dentry_count bigint;
    v_version_count bigint;
    v_manifest_count bigint;
    v_chunk_count bigint;
    v_commit_count bigint;
    v_change_count bigint;
    v_request_count bigint;
    v_acl_count bigint;
    v_acl_set_count bigint;
    v_xattr_count bigint;
    v_audit_count bigint;
    v_handle_count bigint;
    v_checked_versions bigint := 0;
    v_content_bytes bigint := 0;
    v_started timestamptz := clock_timestamp();
    v_row record;
    v_error text;
BEGIN
    IF p_deep NOT IN (0, 1) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_CHECK_MODE: deep must be 0 or 1'
            USING ERRCODE = '22023';
    END IF;
    v_workspace := _vexfs.require_workspace(p_workspace, 'read');

    SELECT count(*) INTO v_inode_count
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_dentry_count
      FROM _vexfs.dentries AS d
     WHERE d.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_version_count
      FROM _vexfs.file_versions AS f
     WHERE f.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_manifest_count
      FROM _vexfs.manifests AS m
     WHERE m.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_chunk_count
      FROM _vexfs.chunks AS c
     WHERE c.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_commit_count
      FROM _vexfs.commits AS c
     WHERE c.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_change_count
      FROM _vexfs.commit_changes AS change
     WHERE change.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_request_count
      FROM _vexfs.request_replays AS request
     WHERE request.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_acl_count
      FROM _vexfs.acl_entries AS acl
     WHERE acl.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_acl_set_count
      FROM _vexfs.acl_sets AS acl_set
     WHERE acl_set.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_xattr_count
      FROM _vexfs.xattrs AS xattr
     WHERE xattr.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_audit_count
      FROM _vexfs.audit_events AS audit
     WHERE audit.workspace_id = v_workspace.workspace_id;
    SELECT count(*) INTO v_bad
      FROM _vexfs.audit_events AS audit
     WHERE audit.workspace_id = v_workspace.workspace_id
       AND (audit.workspace_name <> p_workspace
            OR audit.actor_role IS NULL
            OR audit.operation IS NULL
            OR audit.path IS NULL
            OR audit.inode_id IS NULL
            OR NOT audit.details ? 'before_version'
            OR NOT audit.details ? 'after_version');
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'audit_contract',
            'message', format('%s audit events are missing actor, object, or version metadata', v_bad)));
    END IF;
    SELECT count(*) INTO v_handle_count
      FROM _vexfs.handles AS handle
     WHERE handle.workspace_id = v_workspace.workspace_id;
    SELECT coalesce(sum(c.size_bytes), 0) INTO v_content_bytes
      FROM _vexfs.chunks AS c
     WHERE c.workspace_id = v_workspace.workspace_id;

    SELECT count(*) INTO v_bad
      FROM _vexfs.inodes AS i
     WHERE i.inode_id = v_workspace.root_inode
       AND i.workspace_id = v_workspace.workspace_id
       AND i.kind = 'directory'
       AND i.live;
    IF v_bad <> 1 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'root', 'message', 'workspace root inode is invalid'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.dentries AS d
      LEFT JOIN _vexfs.inodes AS parent ON parent.inode_id = d.parent_inode
      LEFT JOIN _vexfs.inodes AS child ON child.inode_id = d.inode_id
     WHERE d.workspace_id = v_workspace.workspace_id
       AND (parent.inode_id IS NULL
            OR child.inode_id IS NULL
            OR parent.workspace_id <> d.workspace_id
            OR child.workspace_id <> d.workspace_id
            OR parent.kind <> 'directory'
            OR NOT parent.live
            OR NOT child.live);
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'dentry', 'count', v_bad,
            'message', 'dentry parent or child is invalid'));
    END IF;

    WITH RECURSIVE reachable(inode_id) AS (
        SELECT v_workspace.root_inode
        UNION
        SELECT d.inode_id
          FROM _vexfs.dentries AS d
          JOIN reachable AS r ON r.inode_id = d.parent_inode
         WHERE d.workspace_id = v_workspace.workspace_id
    )
    SELECT count(*) INTO v_bad
      FROM _vexfs.dentries AS d
     WHERE d.workspace_id = v_workspace.workspace_id
       AND NOT EXISTS (
           SELECT 1 FROM reachable AS r WHERE r.inode_id = d.parent_inode);
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'reachability', 'count', v_bad,
            'message', 'current dentry tree is not reachable from root'));
    END IF;

    WITH RECURSIVE reachable(inode_id) AS (
        SELECT v_workspace.root_inode
        UNION
        SELECT d.inode_id
          FROM _vexfs.dentries AS d
          JOIN reachable AS r ON r.inode_id = d.parent_inode
         WHERE d.workspace_id = v_workspace.workspace_id
    )
    SELECT count(*) INTO v_bad
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.live
       AND NOT EXISTS (
           SELECT 1 FROM reachable AS r WHERE r.inode_id = i.inode_id);
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'unreachable_inode', 'count', v_bad,
            'message', 'live inode is not reachable from root'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.acl_sets AS acl_set
     WHERE acl_set.workspace_id = v_workspace.workspace_id
       AND (acl_set.entry_count <> (
               SELECT count(*) FROM _vexfs.acl_set_entries AS entry
                WHERE entry.acl_set_id = acl_set.acl_set_id)
            OR acl_set.fingerprint IS DISTINCT FROM encode(pg_catalog.sha256(
                   convert_to(acl_set.canonical_acl::text, 'UTF8')), 'hex')
            OR acl_set.canonical_acl IS DISTINCT FROM (
               SELECT jsonb_agg(jsonb_build_object(
                          'principal', entry.principal,
                          'effect', entry.effect,
                          'permissions', entry.permissions,
                          'inherit', entry.inherit_flags)
                          ORDER BY entry.principal, entry.effect)
                 FROM _vexfs.acl_set_entries AS entry
                WHERE entry.acl_set_id = acl_set.acl_set_id));
    SELECT v_bad + count(*) INTO v_bad
      FROM _vexfs.inodes AS inode
      JOIN _vexfs.acl_sets AS acl_set ON acl_set.acl_set_id = inode.acl_set_id
     WHERE inode.workspace_id = v_workspace.workspace_id
       AND acl_set.workspace_id <> inode.workspace_id;
    SELECT v_bad + count(*) INTO v_bad
      FROM _vexfs.snapshot_inodes AS inode
      JOIN _vexfs.snapshots AS snapshot ON snapshot.snapshot_id = inode.snapshot_id
      JOIN _vexfs.acl_sets AS acl_set ON acl_set.acl_set_id = inode.acl_set_id
     WHERE snapshot.workspace_id = v_workspace.workspace_id
       AND acl_set.workspace_id <> snapshot.workspace_id;
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'acl_set', 'count', v_bad,
            'message', 'ACL set content, count, or workspace reference is invalid'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.inodes AS i
     WHERE i.workspace_id = v_workspace.workspace_id
       AND i.kind = 'directory'
       AND i.inode_id <> v_workspace.root_inode
       AND (SELECT count(*) FROM _vexfs.dentries AS d
             WHERE d.workspace_id = v_workspace.workspace_id
               AND d.inode_id = i.inode_id) <> CASE WHEN i.live THEN 1 ELSE 0 END;
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'directory_link', 'count', v_bad,
            'message', 'directory has an invalid parent link count'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.commits AS c
     WHERE c.workspace_id = v_workspace.workspace_id
       AND c.commit_no BETWEEN 1 AND v_workspace.head_commit
       AND c.parent_commit = c.commit_no - 1;
    IF v_workspace.head_commit < 1 OR v_bad <> v_workspace.head_commit
       OR EXISTS (
           SELECT 1 FROM _vexfs.commits AS c
            WHERE c.workspace_id = v_workspace.workspace_id
              AND c.commit_no > v_workspace.head_commit) THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'commit_chain',
            'message', 'workspace commit chain is not continuous'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM (
          SELECT commit.commit_no,
                 count(change.ordinal) AS change_count,
                 min(change.ordinal) AS first_ordinal,
                 max(change.ordinal) AS last_ordinal
            FROM _vexfs.commits AS commit
            LEFT JOIN _vexfs.commit_changes AS change
              ON change.workspace_id = commit.workspace_id
             AND change.commit_no = commit.commit_no
           WHERE commit.workspace_id = v_workspace.workspace_id
           GROUP BY commit.commit_no) AS summary
     WHERE summary.change_count = 0
        OR summary.first_ordinal <> 1
        OR summary.last_ordinal <> summary.change_count;
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'commit_changes', 'count', v_bad,
            'message', 'commit change ordinals are missing or discontinuous'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.inodes AS i
      LEFT JOIN _vexfs.file_versions AS f
        ON f.workspace_id = i.workspace_id
       AND f.inode_id = i.inode_id
       AND f.version_no = i.current_version
     WHERE i.workspace_id = v_workspace.workspace_id
       AND ((i.kind <> 'directory'
             AND NOT (
                 i.kind = 'file'
                 AND i.current_version = 0
                 AND i.size_bytes = 0
                 AND EXISTS (
                     SELECT 1 FROM _vexfs.handles AS pending
                      WHERE pending.workspace_id = i.workspace_id
                        AND pending.inode_id = i.inode_id
                        AND pending.state IN ('open', 'retained')
                        AND pending.dirty_generation > pending.published_generation))
             AND (i.current_version < 1
                  OR f.version_no IS NULL
                  OR i.size_bytes IS DISTINCT FROM f.size_bytes))
            OR (i.kind = 'directory' AND i.current_version <> 0));
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'current_version', 'count', v_bad,
            'message', 'inode current version is invalid'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = v_workspace.workspace_id
       AND (w.live_files IS DISTINCT FROM (
               SELECT count(*)
                 FROM _vexfs.inodes AS i
                WHERE i.workspace_id = v_workspace.workspace_id
                  AND i.kind <> 'directory'
                  AND i.live)
            OR w.live_bytes IS DISTINCT FROM (
               SELECT coalesce(sum(i.size_bytes), 0)
                 FROM _vexfs.inodes AS i
                WHERE i.workspace_id = v_workspace.workspace_id
                  AND i.kind <> 'directory'
                  AND i.live));
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'live_usage',
            'message', 'workspace live usage counters are invalid'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.file_versions AS alias
      LEFT JOIN _vexfs.file_versions AS source
        ON source.workspace_id = alias.workspace_id
       AND source.inode_id = alias.inode_id
       AND source.version_no = alias.source_version_no
     WHERE alias.workspace_id = v_workspace.workspace_id
       AND alias.source_version_no IS NOT NULL
       AND (source.version_no IS NULL
            OR source.source_version_no IS NOT NULL
            OR source.manifest_id IS NULL
            OR source.size_bytes IS DISTINCT FROM alias.size_bytes
            OR source.checksum IS DISTINCT FROM alias.checksum);
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'version_alias', 'count', v_bad,
            'message', 'file version alias is invalid'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.file_versions AS f
      LEFT JOIN _vexfs.manifests AS m ON m.manifest_id = f.manifest_id
     WHERE f.workspace_id = v_workspace.workspace_id
       AND f.manifest_id IS NOT NULL
       AND (m.manifest_id IS NULL
            OR m.workspace_id <> f.workspace_id
            OR m.file_size <> f.size_bytes
            OR m.checksum IS DISTINCT FROM f.checksum
            OR m.chunk_size <> 65536
            OR m.chunk_count <> ((m.file_size + 65535) / 65536)::integer);
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'manifest_metadata', 'count', v_bad,
            'message', 'manifest metadata does not match file version'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.manifests AS m
     WHERE m.workspace_id = v_workspace.workspace_id
       AND (
           (SELECT count(*) FROM _vexfs.manifest_chunks AS entry
             WHERE entry.manifest_id = m.manifest_id) <> m.chunk_count
           OR EXISTS (
               SELECT 1
                 FROM _vexfs.manifest_chunks AS entry
                 JOIN _vexfs.chunks AS c ON c.chunk_id = entry.chunk_id
                WHERE entry.manifest_id = m.manifest_id
                  AND (entry.chunk_no >= m.chunk_count
                       OR c.workspace_id <> m.workspace_id
                       OR c.size_bytes <> CASE
                           WHEN entry.chunk_no < m.chunk_count - 1 THEN 65536
                           ELSE (m.file_size - entry.chunk_no::bigint * 65536)::integer
                       END))
           OR coalesce((
               SELECT sum(c.size_bytes)
                 FROM _vexfs.manifest_chunks AS entry
                 JOIN _vexfs.chunks AS c ON c.chunk_id = entry.chunk_id
                WHERE entry.manifest_id = m.manifest_id), 0) <> m.file_size
           OR m.checksum IS DISTINCT FROM (
               SELECT encode(pg_catalog.sha256(pg_catalog.convert_to(
                   pg_catalog.format(
                       'vexfs-manifest-v1:%s:%s:%s',
                       m.chunk_size, m.file_size, m.chunk_count)
                   || E'\n' || coalesce(pg_catalog.string_agg(
                       pg_catalog.format(
                           '%s:%s:%s',
                           entry.chunk_no, c.size_bytes, c.checksum),
                       E'\n' ORDER BY entry.chunk_no), ''),
                   'UTF8')), 'hex')
                 FROM _vexfs.manifest_chunks AS entry
                 JOIN _vexfs.chunks AS c ON c.chunk_id = entry.chunk_id
                WHERE entry.manifest_id = m.manifest_id));
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'manifest_chunks', 'count', v_bad,
            'message', 'manifest chunk sequence or size is invalid'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.manifests AS m
     WHERE m.workspace_id = v_workspace.workspace_id
       AND NOT EXISTS (
           SELECT 1 FROM _vexfs.file_versions AS f
            WHERE f.manifest_id = m.manifest_id);
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'orphan_manifest', 'count', v_bad,
            'message', 'manifest is not referenced by a canonical version'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.chunks AS c
     WHERE c.workspace_id = v_workspace.workspace_id
       AND NOT EXISTS (
           SELECT 1 FROM _vexfs.manifest_chunks AS entry
            WHERE entry.chunk_id = c.chunk_id);
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'orphan_chunk', 'count', v_bad,
            'message', 'chunk is not referenced by a manifest'));
    END IF;

    -- Writable handles are mutable state, but their overlay still has strict
    -- invariants.  Check them here so a damaged dirty chunk cannot remain
    -- invisible until the next publish attempt.
    SELECT count(*) INTO v_bad
      FROM _vexfs.handles AS handle
      LEFT JOIN _vexfs.handle_staging AS staging
        ON staging.handle_id = handle.handle_id
      LEFT JOIN _vexfs.manifests AS base
        ON base.manifest_id = staging.base_manifest_id
     WHERE handle.workspace_id = v_workspace.workspace_id
       AND (
           (handle.state IN ('open', 'retained')
            AND position('w' IN handle.flags) > 0
            AND staging.handle_id IS NULL)
           OR (staging.handle_id IS NOT NULL
               AND (handle.state = 'closed'
                    OR position('w' IN handle.flags) = 0
                    OR staging.generation <> handle.dirty_generation
                    OR staging.dirty_bytes IS DISTINCT FROM (
                        SELECT coalesce(sum(octet_length(dirty.content)), 0)
                          FROM _vexfs.handle_staging_chunks AS dirty
                         WHERE dirty.handle_id = staging.handle_id)
                    OR (staging.base_manifest_id IS NULL
                        AND staging.base_size <> 0)
                    OR (staging.base_manifest_id IS NOT NULL
                        AND (base.manifest_id IS NULL
                             OR base.workspace_id <> handle.workspace_id
                             OR base.file_size <> staging.base_size))
                    OR EXISTS (
                        SELECT 1
                          FROM _vexfs.handle_staging_chunks AS dirty
                         WHERE dirty.handle_id = staging.handle_id
                           AND (dirty.chunk_no::bigint * 65536 >= staging.logical_size
                                OR octet_length(dirty.content) <>
                                   least(
                                       65536::bigint,
                                       staging.logical_size
                                           - dirty.chunk_no::bigint * 65536))))));
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'handle_staging', 'count', v_bad,
            'message', 'writable handle staging metadata or dirty chunks are invalid'));
    END IF;

    SELECT count(*) INTO v_bad
      FROM _vexfs.snapshots AS s
     WHERE s.workspace_id = v_workspace.workspace_id
       AND (
           NOT EXISTS (
               SELECT 1 FROM _vexfs.snapshot_inodes AS i
                WHERE i.snapshot_id = s.snapshot_id
                  AND i.inode_id = v_workspace.root_inode
                  AND i.kind = 'directory')
           OR EXISTS (
               SELECT 1
                 FROM _vexfs.snapshot_dentries AS d
                 LEFT JOIN _vexfs.snapshot_inodes AS parent
                   ON parent.snapshot_id = d.snapshot_id
                  AND parent.inode_id = d.parent_inode
                 LEFT JOIN _vexfs.snapshot_inodes AS child
                   ON child.snapshot_id = d.snapshot_id
                  AND child.inode_id = d.inode_id
                WHERE d.snapshot_id = s.snapshot_id
                  AND (parent.inode_id IS NULL
                       OR child.inode_id IS NULL
                       OR parent.kind <> 'directory'))
           OR EXISTS (
               SELECT 1
                 FROM _vexfs.snapshot_xattrs AS x
                 LEFT JOIN _vexfs.snapshot_inodes AS i
                   ON i.snapshot_id = x.snapshot_id
                  AND i.inode_id = x.inode_id
                WHERE x.snapshot_id = s.snapshot_id
                  AND i.inode_id IS NULL)
           OR EXISTS (
               SELECT 1
                 FROM _vexfs.snapshot_acl_entries AS acl
                 LEFT JOIN _vexfs.snapshot_inodes AS i
                   ON i.snapshot_id = acl.snapshot_id
                  AND i.inode_id = acl.inode_id
                WHERE acl.snapshot_id = s.snapshot_id
                  AND i.inode_id IS NULL)
           OR EXISTS (
               SELECT 1
                 FROM _vexfs.snapshot_inodes AS i
                 LEFT JOIN _vexfs.file_versions AS f
                   ON f.workspace_id = s.workspace_id
                  AND f.inode_id = i.inode_id
                  AND f.version_no = i.current_version
                WHERE i.snapshot_id = s.snapshot_id
                  AND i.kind <> 'directory'
                  AND f.version_no IS NULL));
    IF v_bad <> 0 THEN
        v_issues := v_issues || jsonb_build_array(jsonb_build_object(
            'code', 'snapshot', 'count', v_bad,
            'message', 'snapshot tree or file reference is invalid'));
    END IF;

    IF p_deep = 1 THEN
        FOR v_row IN
            SELECT f.inode_id, f.version_no
              FROM _vexfs.file_versions AS f
             WHERE f.workspace_id = v_workspace.workspace_id
               AND f.manifest_id IS NOT NULL
             ORDER BY f.inode_id, f.version_no
        LOOP
            v_checked_versions := v_checked_versions + 1;
            BEGIN
                PERFORM _vexfs.read_version_content(
                    v_workspace.workspace_id, v_row.inode_id, v_row.version_no);
            EXCEPTION WHEN OTHERS THEN
                GET STACKED DIAGNOSTICS v_error = MESSAGE_TEXT;
                IF jsonb_array_length(v_issues) < 100 THEN
                    v_issues := v_issues || jsonb_build_array(jsonb_build_object(
                        'code', 'content_checksum',
                        'inode', v_row.inode_id,
                        'version', v_row.version_no,
                        'message', v_error));
                END IF;
            END;
        END LOOP;
    END IF;

    SELECT coalesce(jsonb_agg(issue.value || jsonb_build_object(
               'object', coalesce(
                   issue.value->>'object', issue.value->>'inode', p_workspace),
               'suggestion', coalesce(
                   issue.value->>'suggestion', 'run deep check and restore from a verified snapshot'))),
           '[]'::jsonb)
      INTO v_issues
      FROM jsonb_array_elements(v_issues) AS issue(value);
    RETURN jsonb_build_object(
        'ok', jsonb_array_length(v_issues) = 0,
        'workspace', p_workspace,
        'mode', CASE WHEN p_deep = 1 THEN 'deep' ELSE 'quick' END,
        'content_model', 'chunked-manifest-v1',
        'issue_count', jsonb_array_length(v_issues),
        'versions', v_version_count,
        'content_bytes', v_content_bytes,
        'elapsed_ms', greatest(0, (
            extract(epoch FROM clock_timestamp() - v_started) * 1000)::bigint),
        'issues', v_issues,
        'checked', jsonb_build_object(
            'inodes', v_inode_count,
            'dentries', v_dentry_count,
            'versions', v_version_count,
            'manifests', v_manifest_count,
            'chunks', v_chunk_count,
            'commits', v_commit_count,
            'commit_changes', v_change_count,
            'requests', v_request_count,
            'acl_sets', v_acl_set_count,
            'acl_entries', v_acl_count,
            'xattrs', v_xattr_count,
            'audit_events', v_audit_count,
            'handles', v_handle_count,
            'canonical_versions', v_checked_versions));
END;
$$;

-- format v2 的 PostgreSQL 生产端。CLI 只读取这个公开、带权限检查的记录流，
-- 不需要也不允许直接读取 _vexfs 私有表。临时表把一次导出固定在当前数据库
-- 事务快照中，并把 PG 的完整 snapshot 表转换成 format v2 的状态变更记录。
CREATE FUNCTION public.vexfs_archive_export_records(
    p_workspace text,
    p_snapshot text DEFAULT NULL)
RETURNS TABLE(
    record_type text,
    record_key text,
    record_json jsonb,
    content bytea)
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_workspace _vexfs.workspaces%ROWTYPE;
    v_source_commit bigint;
    v_selected_snapshot bigint;
    v_selected_name text;
    v_created_at bigint := floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint;
BEGIN
    v_workspace := _vexfs.require_workspace(p_workspace, 'admin');
    IF p_snapshot IS NULL OR p_snapshot = '' OR p_snapshot = 'HEAD' THEN
        v_source_commit := v_workspace.head_commit;
        v_selected_snapshot := NULL;
        v_selected_name := NULL;
    ELSE
        SELECT s.snapshot_id, s.head_commit, s.name
          INTO v_selected_snapshot, v_source_commit, v_selected_name
          FROM _vexfs.snapshots AS s
         WHERE s.workspace_id = v_workspace.workspace_id
           AND s.name = p_snapshot;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'VEXFS_SNAPSHOT_NOT_FOUND: %', p_snapshot
                USING ERRCODE = 'P0002';
        END IF;
    END IF;
    IF v_source_commit <= 0 THEN
        RAISE EXCEPTION 'VEXFS_ARCHIVE_EMPTY: workspace has no committed state'
            USING ERRCODE = '55000';
    END IF;

    DROP TABLE IF EXISTS pg_temp.vexfs_export_checkpoints;
    DROP TABLE IF EXISTS pg_temp.vexfs_export_inodes;
    DROP TABLE IF EXISTS pg_temp.vexfs_export_dentries;
    DROP TABLE IF EXISTS pg_temp.vexfs_export_xattrs;
    DROP TABLE IF EXISTS pg_temp.vexfs_export_acls;

    CREATE TEMP TABLE vexfs_export_checkpoints(
        commit_no bigint PRIMARY KEY,
        snapshot_id bigint,
        created_at_ms bigint NOT NULL,
        selected boolean NOT NULL DEFAULT false)
        ON COMMIT DROP;
    INSERT INTO pg_temp.vexfs_export_checkpoints(
        commit_no, snapshot_id, created_at_ms)
    SELECT s.head_commit,
           min(s.snapshot_id),
           floor(extract(epoch FROM min(s.created_at)) * 1000)::bigint
      FROM _vexfs.snapshots AS s
     WHERE s.workspace_id = v_workspace.workspace_id
       AND s.head_commit <= v_source_commit
     GROUP BY s.head_commit;
    INSERT INTO pg_temp.vexfs_export_checkpoints(
        commit_no, snapshot_id, created_at_ms, selected)
    VALUES (
        v_source_commit,
        v_selected_snapshot,
        coalesce((SELECT floor(extract(epoch FROM s.created_at) * 1000)::bigint
                    FROM _vexfs.snapshots AS s
                   WHERE s.snapshot_id = v_selected_snapshot), v_created_at),
        true)
    ON CONFLICT (commit_no) DO UPDATE
      SET snapshot_id = EXCLUDED.snapshot_id,
          created_at_ms = EXCLUDED.created_at_ms,
          selected = true;

    CREATE TEMP TABLE vexfs_export_inodes(
        commit_no bigint NOT NULL,
        inode_id bigint NOT NULL,
        kind text NOT NULL,
        mode integer NOT NULL,
        owner_principal text NOT NULL,
        uid bigint NOT NULL,
        gid bigint NOT NULL,
        size_bytes bigint NOT NULL,
        current_version bigint NOT NULL,
        created_at_ms bigint NOT NULL,
        accessed_at_ms bigint NOT NULL,
        updated_at_ms bigint NOT NULL,
        changed_at_ms bigint NOT NULL,
        PRIMARY KEY (commit_no, inode_id)) ON COMMIT DROP;
    INSERT INTO pg_temp.vexfs_export_inodes
    SELECT cp.commit_no,
           i.inode_id,
           i.kind,
           i.mode,
           i.owner_principal,
           i.uid,
           i.gid,
           i.size_bytes,
           i.current_version,
           floor(extract(epoch FROM i.created_at) * 1000)::bigint,
           floor(extract(epoch FROM i.accessed_at) * 1000)::bigint,
           floor(extract(epoch FROM i.modified_at) * 1000)::bigint,
           floor(extract(epoch FROM i.changed_at) * 1000)::bigint
      FROM pg_temp.vexfs_export_checkpoints AS cp
      JOIN _vexfs.snapshot_inodes AS i ON i.snapshot_id = cp.snapshot_id
     WHERE cp.snapshot_id IS NOT NULL;
    INSERT INTO pg_temp.vexfs_export_inodes
    SELECT cp.commit_no,
           i.inode_id,
           i.kind,
           i.mode,
           i.owner_principal,
           i.uid,
           i.gid,
           i.size_bytes,
           i.current_version,
           floor(extract(epoch FROM i.created_at) * 1000)::bigint,
           floor(extract(epoch FROM i.accessed_at) * 1000)::bigint,
           floor(extract(epoch FROM i.modified_at) * 1000)::bigint,
           floor(extract(epoch FROM i.changed_at) * 1000)::bigint
      FROM pg_temp.vexfs_export_checkpoints AS cp
      JOIN _vexfs.inodes AS i
        ON i.workspace_id = v_workspace.workspace_id AND i.live
     WHERE cp.selected AND cp.snapshot_id IS NULL
    ON CONFLICT (commit_no, inode_id) DO UPDATE SET
        kind = EXCLUDED.kind,
        mode = EXCLUDED.mode,
        owner_principal = EXCLUDED.owner_principal,
        uid = EXCLUDED.uid,
        gid = EXCLUDED.gid,
        size_bytes = EXCLUDED.size_bytes,
        current_version = EXCLUDED.current_version,
        created_at_ms = EXCLUDED.created_at_ms,
        accessed_at_ms = EXCLUDED.accessed_at_ms,
        updated_at_ms = EXCLUDED.updated_at_ms,
        changed_at_ms = EXCLUDED.changed_at_ms;

    CREATE TEMP TABLE vexfs_export_dentries(
        commit_no bigint NOT NULL,
        parent_inode bigint NOT NULL,
        name text NOT NULL,
        inode_id bigint NOT NULL,
        PRIMARY KEY (commit_no, parent_inode, name)) ON COMMIT DROP;
    INSERT INTO pg_temp.vexfs_export_dentries
    SELECT cp.commit_no, d.parent_inode, d.name, d.inode_id
      FROM pg_temp.vexfs_export_checkpoints AS cp
      JOIN _vexfs.snapshot_dentries AS d ON d.snapshot_id = cp.snapshot_id
     WHERE cp.snapshot_id IS NOT NULL;
    INSERT INTO pg_temp.vexfs_export_dentries
    SELECT cp.commit_no, d.parent_inode, d.name, d.inode_id
      FROM pg_temp.vexfs_export_checkpoints AS cp
      JOIN _vexfs.dentries AS d ON d.workspace_id = v_workspace.workspace_id
     WHERE cp.selected AND cp.snapshot_id IS NULL
    ON CONFLICT (commit_no, parent_inode, name) DO UPDATE
      SET inode_id = EXCLUDED.inode_id;

    CREATE TEMP TABLE vexfs_export_xattrs(
        commit_no bigint NOT NULL,
        inode_id bigint NOT NULL,
        name text NOT NULL,
        value bytea NOT NULL,
        updated_at_ms bigint NOT NULL,
        PRIMARY KEY (commit_no, inode_id, name)) ON COMMIT DROP;
    INSERT INTO pg_temp.vexfs_export_xattrs
    SELECT cp.commit_no,
           x.inode_id,
           x.name,
           x.value,
           floor(extract(epoch FROM x.updated_at) * 1000)::bigint
      FROM pg_temp.vexfs_export_checkpoints AS cp
      JOIN _vexfs.snapshot_xattrs AS x ON x.snapshot_id = cp.snapshot_id
     WHERE cp.snapshot_id IS NOT NULL;
    INSERT INTO pg_temp.vexfs_export_xattrs
    SELECT cp.commit_no,
           x.inode_id,
           x.name,
           x.value,
           floor(extract(epoch FROM x.updated_at) * 1000)::bigint
      FROM pg_temp.vexfs_export_checkpoints AS cp
      JOIN _vexfs.xattrs AS x ON x.workspace_id = v_workspace.workspace_id
     WHERE cp.selected AND cp.snapshot_id IS NULL
    ON CONFLICT (commit_no, inode_id, name) DO UPDATE
      SET value = EXCLUDED.value, updated_at_ms = EXCLUDED.updated_at_ms;

    CREATE TEMP TABLE vexfs_export_acls(
        commit_no bigint NOT NULL,
        inode_id bigint NOT NULL,
        principal text NOT NULL,
        effect text NOT NULL,
        permissions text NOT NULL,
        inherit_flags integer NOT NULL,
        created_at_ms bigint NOT NULL,
        updated_at_ms bigint NOT NULL,
        PRIMARY KEY (commit_no, inode_id, principal, effect)) ON COMMIT DROP;
    INSERT INTO pg_temp.vexfs_export_acls
    SELECT cp.commit_no,
           acl.inode_id,
           acl.principal,
           acl.effect,
           acl.permissions,
           acl.inherit_flags,
           floor(extract(epoch FROM acl.created_at) * 1000)::bigint,
           floor(extract(epoch FROM acl.updated_at) * 1000)::bigint
      FROM pg_temp.vexfs_export_checkpoints AS cp
      JOIN _vexfs.snapshot_acl_entries AS acl ON acl.snapshot_id = cp.snapshot_id
     WHERE cp.snapshot_id IS NOT NULL;
    INSERT INTO pg_temp.vexfs_export_acls
    SELECT cp.commit_no,
           acl.inode_id,
           acl.principal,
           acl.effect,
           acl.permissions,
           acl.inherit_flags,
           floor(extract(epoch FROM acl.created_at) * 1000)::bigint,
           floor(extract(epoch FROM acl.updated_at) * 1000)::bigint
      FROM pg_temp.vexfs_export_checkpoints AS cp
      JOIN _vexfs.acl_entries AS acl ON acl.workspace_id = v_workspace.workspace_id
     WHERE cp.selected AND cp.snapshot_id IS NULL
    ON CONFLICT (commit_no, inode_id, principal, effect) DO UPDATE SET
        permissions = EXCLUDED.permissions,
        inherit_flags = EXCLUDED.inherit_flags,
        created_at_ms = EXCLUDED.created_at_ms,
        updated_at_ms = EXCLUDED.updated_at_ms;

    record_type := 'manifest';
    record_key := 'manifest';
    record_json := jsonb_build_object(
        'format_version', 2,
        'source_engine', 'postgresql',
        'source_workspace', p_workspace,
        'source_commit', v_source_commit,
        'source_snapshot', v_selected_name,
        'root_source_inode', v_workspace.root_inode,
        'history_floor_source_commit', v_workspace.history_floor_commit,
        'retention_keep_versions', v_workspace.retention_keep_versions,
        'retention_keep_days', v_workspace.retention_keep_days,
        'quota_max_bytes', v_workspace.quota_max_bytes,
        'quota_max_files', v_workspace.quota_max_files,
        'quota_max_file_bytes', v_workspace.quota_max_file_bytes,
        'created_at', v_created_at);
    content := NULL;
    RETURN NEXT;

    RETURN QUERY
    SELECT 'commits'::text,
           lpad(c.commit_no::text, 20, '0'),
           jsonb_build_object(
               'source_id', c.commit_no,
               'parent_source_id', nullif(c.parent_commit, 0),
               'message', c.operation,
               'path', c.path,
               'actor', c.created_by::text,
               'session_id', c.session_id,
               'run_id', c.run_id,
               'created_at', floor(extract(epoch FROM c.created_at) * 1000)::bigint),
           NULL::bytea
      FROM _vexfs.commits AS c
     WHERE c.workspace_id = v_workspace.workspace_id
       AND c.commit_no <= v_source_commit
     ORDER BY c.commit_no;

    RETURN QUERY
    SELECT 'commit_changes'::text,
           lpad(change.commit_no::text, 20, '0') || ':' ||
               lpad(change.ordinal::text, 10, '0'),
           jsonb_build_object(
               'source_commit', change.commit_no,
               'ordinal', change.ordinal,
               'operation', change.operation,
               'path', change.path,
               'source_inode', change.inode_id,
               'before_version', change.before_version,
               'after_version', change.after_version,
               'details', change.details),
           NULL::bytea
      FROM _vexfs.commit_changes AS change
     WHERE change.workspace_id = v_workspace.workspace_id
       AND change.commit_no <= v_source_commit
     ORDER BY change.commit_no, change.ordinal;

    RETURN QUERY
    WITH latest AS (
        SELECT DISTINCT ON (state.inode_id)
               state.*
          FROM pg_temp.vexfs_export_inodes AS state
         WHERE state.commit_no <= v_source_commit
         ORDER BY state.inode_id, state.commit_no DESC),
    selected AS (
        SELECT state.inode_id
          FROM pg_temp.vexfs_export_inodes AS state
         WHERE state.commit_no = v_source_commit)
    SELECT 'inodes'::text,
           lpad(latest.inode_id::text, 20, '0'),
           jsonb_build_object(
               'source_id', latest.inode_id,
               'kind', latest.kind,
               'mode', latest.mode,
               'owner_principal', latest.owner_principal,
               'uid', latest.uid,
               'gid', latest.gid,
               'size', latest.size_bytes,
               'current_version', latest.current_version,
               'created_at', latest.created_at_ms,
               'accessed_at', latest.accessed_at_ms,
               'updated_at', latest.updated_at_ms,
               'changed_at', latest.changed_at_ms,
               'deleted_at', CASE WHEN selected.inode_id IS NULL
                                  THEN v_created_at ELSE NULL END),
           NULL::bytea
      FROM latest
      LEFT JOIN selected USING (inode_id)
     ORDER BY latest.inode_id;

    RETURN QUERY
    SELECT 'dentries'::text,
           lpad(d.parent_inode::text, 20, '0') || ':' || encode(convert_to(d.name, 'UTF8'), 'hex'),
           jsonb_build_object(
               'parent_source_inode', d.parent_inode,
               'name', d.name,
               'inode_source_id', d.inode_id),
           NULL::bytea
      FROM pg_temp.vexfs_export_dentries AS d
     WHERE d.commit_no = v_source_commit
     ORDER BY d.parent_inode, d.name;

    RETURN QUERY
    SELECT 'file_versions'::text,
           lpad(f.inode_id::text, 20, '0') || ':' || lpad(f.version_no::text, 20, '0'),
           jsonb_build_object(
               'source_inode', f.inode_id,
               'version_no', f.version_no,
               'source_commit', f.commit_no,
               'source_manifest', f.manifest_id,
               'size', f.size_bytes,
               'checksum', f.checksum,
               'source_version_no', f.source_version_no,
               'created_at', floor(extract(epoch FROM f.created_at) * 1000)::bigint),
           NULL::bytea
      FROM _vexfs.file_versions AS f
     WHERE f.workspace_id = v_workspace.workspace_id
       AND f.commit_no <= v_source_commit
       AND EXISTS (
           SELECT 1 FROM pg_temp.vexfs_export_inodes AS state
            WHERE state.inode_id = f.inode_id)
     ORDER BY f.inode_id, f.version_no;

    RETURN QUERY
    SELECT 'manifests'::text,
           lpad(m.manifest_id::text, 20, '0'),
           jsonb_build_object(
               'source_id', m.manifest_id,
               'file_size', m.file_size,
               'chunk_size', m.chunk_size,
               'chunk_count', m.chunk_count,
               'checksum', m.checksum,
               'created_at', floor(extract(epoch FROM m.created_at) * 1000)::bigint),
           NULL::bytea
      FROM _vexfs.manifests AS m
     WHERE m.workspace_id = v_workspace.workspace_id
       AND EXISTS (
           SELECT 1 FROM _vexfs.file_versions AS f
            WHERE f.workspace_id = v_workspace.workspace_id
              AND f.commit_no <= v_source_commit
              AND f.manifest_id = m.manifest_id
              AND EXISTS (
                  SELECT 1 FROM pg_temp.vexfs_export_inodes AS state
                   WHERE state.inode_id = f.inode_id))
     ORDER BY m.manifest_id;

    RETURN QUERY
    SELECT 'chunks'::text,
           lpad(entry.manifest_id::text, 20, '0') || ':' ||
               lpad(entry.chunk_no::text, 20, '0'),
           jsonb_build_object(
               'source_manifest', entry.manifest_id,
               'chunk_no', entry.chunk_no,
               'size', c.size_bytes,
               'checksum', c.checksum),
           c.content
      FROM _vexfs.manifest_chunks AS entry
      JOIN _vexfs.chunks AS c ON c.chunk_id = entry.chunk_id
     WHERE EXISTS (
         SELECT 1 FROM _vexfs.manifests AS m
          WHERE m.manifest_id = entry.manifest_id
            AND m.workspace_id = v_workspace.workspace_id
            AND EXISTS (
                SELECT 1 FROM _vexfs.file_versions AS f
                 WHERE f.workspace_id = v_workspace.workspace_id
                   AND f.commit_no <= v_source_commit
                   AND f.manifest_id = m.manifest_id
                   AND EXISTS (
                       SELECT 1 FROM pg_temp.vexfs_export_inodes AS state
                        WHERE state.inode_id = f.inode_id)))
     ORDER BY entry.manifest_id, entry.chunk_no;

    RETURN QUERY
    WITH inode_keys AS (
        SELECT DISTINCT state.inode_id FROM pg_temp.vexfs_export_inodes AS state),
    grid AS (
        SELECT cp.commit_no,
               cp.created_at_ms AS checkpoint_created_at,
               key.inode_id,
               (state.inode_id IS NOT NULL) AS present,
               lag(state.inode_id IS NOT NULL) OVER (
                   PARTITION BY key.inode_id ORDER BY cp.commit_no) AS was_present
          FROM pg_temp.vexfs_export_checkpoints AS cp
          CROSS JOIN inode_keys AS key
          LEFT JOIN pg_temp.vexfs_export_inodes AS state
            ON state.commit_no = cp.commit_no AND state.inode_id = key.inode_id),
    emitted AS (
        SELECT grid.commit_no,
               grid.checkpoint_created_at,
               coalesce(current_state.inode_id, previous_state.inode_id) AS inode_id,
               coalesce(current_state.kind, previous_state.kind) AS kind,
               coalesce(current_state.mode, previous_state.mode) AS mode,
               coalesce(current_state.owner_principal, previous_state.owner_principal) AS owner_principal,
               coalesce(current_state.uid, previous_state.uid) AS uid,
               coalesce(current_state.gid, previous_state.gid) AS gid,
               coalesce(current_state.size_bytes, previous_state.size_bytes) AS size_bytes,
               coalesce(current_state.current_version, previous_state.current_version) AS current_version,
               coalesce(current_state.created_at_ms, previous_state.created_at_ms) AS created_at_ms,
               coalesce(current_state.accessed_at_ms, previous_state.accessed_at_ms) AS accessed_at_ms,
               coalesce(current_state.updated_at_ms, previous_state.updated_at_ms) AS updated_at_ms,
               coalesce(current_state.changed_at_ms, previous_state.changed_at_ms) AS changed_at_ms,
               grid.present
          FROM grid
          LEFT JOIN pg_temp.vexfs_export_inodes AS current_state
            ON current_state.commit_no = grid.commit_no
           AND current_state.inode_id = grid.inode_id
          LEFT JOIN LATERAL (
              SELECT state.*
                FROM pg_temp.vexfs_export_inodes AS state
               WHERE state.inode_id = grid.inode_id
                 AND state.commit_no < grid.commit_no
               ORDER BY state.commit_no DESC LIMIT 1) AS previous_state ON true
         WHERE grid.present OR (grid.was_present AND NOT grid.present))
    SELECT 'inode_states'::text,
           lpad(emitted.inode_id::text, 20, '0') || ':' ||
               lpad(emitted.commit_no::text, 20, '0'),
           jsonb_build_object(
               'source_inode', emitted.inode_id,
               'source_commit', emitted.commit_no,
               'kind', emitted.kind,
               'mode', emitted.mode,
               'owner_principal', emitted.owner_principal,
               'uid', emitted.uid,
               'gid', emitted.gid,
               'size', emitted.size_bytes,
               'current_version', emitted.current_version,
               'created_at', emitted.created_at_ms,
               'accessed_at', emitted.accessed_at_ms,
               'updated_at', emitted.updated_at_ms,
               'changed_at', emitted.changed_at_ms,
               'deleted_at', CASE WHEN emitted.present THEN NULL
                                  ELSE emitted.checkpoint_created_at END),
           NULL::bytea
      FROM emitted
     ORDER BY emitted.inode_id, emitted.commit_no;

    RETURN QUERY
    WITH keys AS (
        SELECT DISTINCT state.parent_inode, state.name
          FROM pg_temp.vexfs_export_dentries AS state),
    grid AS (
        SELECT cp.commit_no,
               key.parent_inode,
               key.name,
               state.inode_id,
               (state.inode_id IS NOT NULL) AS present,
               lag(state.inode_id IS NOT NULL) OVER (
                   PARTITION BY key.parent_inode, key.name ORDER BY cp.commit_no) AS was_present
          FROM pg_temp.vexfs_export_checkpoints AS cp
          CROSS JOIN keys AS key
          LEFT JOIN pg_temp.vexfs_export_dentries AS state
            ON state.commit_no = cp.commit_no
           AND state.parent_inode = key.parent_inode
           AND state.name = key.name)
    SELECT 'dentry_states'::text,
           lpad(grid.parent_inode::text, 20, '0') || ':' ||
               encode(convert_to(grid.name, 'UTF8'), 'hex') || ':' ||
               lpad(grid.commit_no::text, 20, '0'),
           jsonb_build_object(
               'parent_source_inode', grid.parent_inode,
               'name', grid.name,
               'source_commit', grid.commit_no,
               'inode_source_id', coalesce(grid.inode_id, previous.inode_id),
               'deleted', CASE WHEN grid.present THEN 0 ELSE 1 END),
           NULL::bytea
      FROM grid
      LEFT JOIN LATERAL (
          SELECT state.inode_id
            FROM pg_temp.vexfs_export_dentries AS state
           WHERE state.parent_inode = grid.parent_inode
             AND state.name = grid.name
             AND state.commit_no < grid.commit_no
           ORDER BY state.commit_no DESC LIMIT 1) AS previous ON true
     WHERE grid.present OR (grid.was_present AND NOT grid.present)
     ORDER BY grid.parent_inode, grid.name, grid.commit_no;

    RETURN QUERY
    WITH keys AS (
        SELECT DISTINCT state.inode_id, state.name
          FROM pg_temp.vexfs_export_xattrs AS state),
    grid AS (
        SELECT cp.commit_no,
               key.inode_id,
               key.name,
               state.value,
               (state.inode_id IS NOT NULL) AS present,
               lag(state.inode_id IS NOT NULL) OVER (
                   PARTITION BY key.inode_id, key.name ORDER BY cp.commit_no) AS was_present
          FROM pg_temp.vexfs_export_checkpoints AS cp
          CROSS JOIN keys AS key
          LEFT JOIN pg_temp.vexfs_export_xattrs AS state
            ON state.commit_no = cp.commit_no
           AND state.inode_id = key.inode_id
           AND state.name = key.name)
    SELECT 'xattr_states'::text,
           lpad(grid.inode_id::text, 20, '0') || ':' ||
               encode(convert_to(grid.name, 'UTF8'), 'hex') || ':' ||
               lpad(grid.commit_no::text, 20, '0'),
           jsonb_build_object(
               'source_inode', grid.inode_id,
               'name', grid.name,
               'source_commit', grid.commit_no,
               'value_hex', encode(coalesce(grid.value, previous.value), 'hex'),
               'deleted', CASE WHEN grid.present THEN 0 ELSE 1 END),
           NULL::bytea
      FROM grid
      LEFT JOIN LATERAL (
          SELECT state.value
            FROM pg_temp.vexfs_export_xattrs AS state
           WHERE state.inode_id = grid.inode_id
             AND state.name = grid.name
             AND state.commit_no < grid.commit_no
           ORDER BY state.commit_no DESC LIMIT 1) AS previous ON true
     WHERE grid.present OR (grid.was_present AND NOT grid.present)
     ORDER BY grid.inode_id, grid.name, grid.commit_no;

    RETURN QUERY
    WITH keys AS (
        SELECT DISTINCT state.inode_id, state.principal, state.effect
          FROM pg_temp.vexfs_export_acls AS state),
    grid AS (
        SELECT cp.commit_no,
               key.inode_id,
               key.principal,
               key.effect,
               state.permissions,
               state.inherit_flags,
               (state.inode_id IS NOT NULL) AS present,
               lag(state.inode_id IS NOT NULL) OVER (
                   PARTITION BY key.inode_id, key.principal, key.effect
                   ORDER BY cp.commit_no) AS was_present
          FROM pg_temp.vexfs_export_checkpoints AS cp
          CROSS JOIN keys AS key
          LEFT JOIN pg_temp.vexfs_export_acls AS state
            ON state.commit_no = cp.commit_no
           AND state.inode_id = key.inode_id
           AND state.principal = key.principal
           AND state.effect = key.effect)
    SELECT 'acl_states'::text,
           lpad(grid.inode_id::text, 20, '0') || ':' ||
               encode(convert_to(grid.principal, 'UTF8'), 'hex') || ':' ||
               grid.effect || ':' || lpad(grid.commit_no::text, 20, '0'),
           jsonb_build_object(
               'source_inode', grid.inode_id,
               'principal_id', grid.principal,
               'effect', grid.effect,
               'source_commit', grid.commit_no,
               'permissions', coalesce(grid.permissions, previous.permissions),
               'inherit_flags', coalesce(grid.inherit_flags, previous.inherit_flags),
               'deleted', CASE WHEN grid.present THEN 0 ELSE 1 END),
           NULL::bytea
      FROM grid
      LEFT JOIN LATERAL (
          SELECT state.permissions, state.inherit_flags
            FROM pg_temp.vexfs_export_acls AS state
           WHERE state.inode_id = grid.inode_id
             AND state.principal = grid.principal
             AND state.effect = grid.effect
             AND state.commit_no < grid.commit_no
           ORDER BY state.commit_no DESC LIMIT 1) AS previous ON true
     WHERE grid.present OR (grid.was_present AND NOT grid.present)
     ORDER BY grid.inode_id, grid.principal, grid.effect, grid.commit_no;

    RETURN QUERY
    SELECT 'snapshots'::text,
           encode(convert_to(s.name, 'UTF8'), 'hex'),
           jsonb_build_object(
               'name', s.name,
               'source_commit', s.head_commit,
               'created_at', floor(extract(epoch FROM s.created_at) * 1000)::bigint),
           NULL::bytea
      FROM _vexfs.snapshots AS s
     WHERE s.workspace_id = v_workspace.workspace_id
       AND s.head_commit <= v_source_commit
     ORDER BY s.head_commit, s.snapshot_id;

    RETURN QUERY
    SELECT 'xattrs'::text,
           lpad(x.inode_id::text, 20, '0') || ':' || encode(convert_to(x.name, 'UTF8'), 'hex'),
           jsonb_build_object(
               'source_inode', x.inode_id,
               'name', x.name,
               'value_hex', encode(x.value, 'hex'),
               'updated_at', x.updated_at_ms),
           NULL::bytea
      FROM pg_temp.vexfs_export_xattrs AS x
     WHERE x.commit_no = v_source_commit
     ORDER BY x.inode_id, x.name;

    RETURN QUERY
    SELECT 'acl_entries'::text,
           lpad(acl.inode_id::text, 20, '0') || ':' ||
               encode(convert_to(acl.principal, 'UTF8'), 'hex') || ':' || acl.effect,
           jsonb_build_object(
               'source_inode', acl.inode_id,
               'principal_id', acl.principal,
               'effect', acl.effect,
               'permissions', acl.permissions,
               'inherit_flags', acl.inherit_flags,
               'created_at', acl.created_at_ms,
               'updated_at', acl.updated_at_ms),
           NULL::bytea
      FROM pg_temp.vexfs_export_acls AS acl
     WHERE acl.commit_no = v_source_commit
     ORDER BY acl.inode_id, acl.principal, acl.effect;

    RETURN QUERY
    WITH principals AS (
        SELECT state.owner_principal AS principal
          FROM pg_temp.vexfs_export_inodes AS state
        UNION
        SELECT state.principal FROM pg_temp.vexfs_export_acls AS state)
    SELECT 'principals'::text,
           encode(convert_to(principals.principal, 'UTF8'), 'hex'),
           jsonb_build_object('source_principal', principals.principal),
           NULL::bytea
      FROM principals
     WHERE principals.principal <> ''
     ORDER BY principals.principal;
END;
$$;

CREATE FUNCTION public.vexfs_archive_import_begin(
    p_workspace text,
    p_manifest jsonb)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_job bigint;
    v_actor oid;
BEGIN
    IF p_workspace IS NULL OR btrim(p_workspace) = '' OR p_workspace <> btrim(p_workspace)
       OR octet_length(p_workspace) > 128 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_WORKSPACE: invalid workspace name'
            USING ERRCODE = '22023';
    END IF;
    IF p_manifest IS NULL OR jsonb_typeof(p_manifest) <> 'object'
       OR coalesce((p_manifest->>'format_version')::integer, 0) <> 2
       OR coalesce((p_manifest->>'source_commit')::bigint, 0) <= 0
       OR coalesce((p_manifest->>'root_source_inode')::bigint, 0) <= 0
       OR coalesce((p_manifest->>'history_floor_source_commit')::bigint, 0) <= 0
       OR (p_manifest->>'history_floor_source_commit')::bigint >
          (p_manifest->>'source_commit')::bigint
       OR coalesce((p_manifest->>'retention_keep_versions')::integer, -1)
          NOT BETWEEN 0 AND 1000000
       OR coalesce((p_manifest->>'retention_keep_days')::integer, -1)
          NOT BETWEEN 0 AND 36500 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: invalid format v2 manifest'
            USING ERRCODE = '22023';
    END IF;
    IF (p_manifest->>'quota_max_bytes') IS NOT NULL
       AND (p_manifest->>'quota_max_bytes')::bigint < 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: invalid max bytes'
            USING ERRCODE = '22023';
    END IF;
    IF (p_manifest->>'quota_max_files') IS NOT NULL
       AND (p_manifest->>'quota_max_files')::bigint < 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: invalid max files'
            USING ERRCODE = '22023';
    END IF;
    IF (p_manifest->>'quota_max_file_bytes') IS NOT NULL
       AND (p_manifest->>'quota_max_file_bytes')::bigint < 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: invalid max file bytes'
            USING ERRCODE = '22023';
    END IF;
    IF EXISTS (SELECT 1 FROM _vexfs.workspaces AS w WHERE w.name = p_workspace) THEN
        RAISE EXCEPTION 'VEXFS_WORKSPACE_EXISTS: %', p_workspace
            USING ERRCODE = '23505';
    END IF;
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    DELETE FROM _vexfs.archive_import_jobs AS job
     WHERE job.owner_oid = v_actor
       AND job.created_at < clock_timestamp() - interval '1 day';
    INSERT INTO _vexfs.archive_import_jobs(
        workspace_name, owner_oid, owner_role, backend_pid, manifest)
    VALUES (p_workspace, v_actor, session_user, pg_catalog.pg_backend_pid(), p_manifest)
    RETURNING job_id INTO v_job;
    RETURN v_job;
END;
$$;

CREATE FUNCTION public.vexfs_archive_import_record(
    p_job bigint,
    p_record_type text,
    p_record_key text,
    p_record_json jsonb,
    p_content bytea DEFAULT NULL)
RETURNS bigint
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_actor oid;
    v_count bigint;
BEGIN
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    PERFORM 1 FROM _vexfs.archive_import_jobs AS job
     WHERE job.job_id = p_job
       AND job.owner_oid = v_actor
       AND job.backend_pid = pg_catalog.pg_backend_pid();
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_IMPORT_JOB_NOT_FOUND: %', p_job
            USING ERRCODE = 'P0002';
    END IF;
    IF p_record_type NOT IN (
        'commits', 'commit_changes', 'inodes', 'dentries', 'file_versions', 'manifests', 'chunks',
        'inode_states', 'dentry_states', 'xattr_states', 'acl_states', 'snapshots',
        'xattrs', 'acl_entries', 'principals')
       OR p_record_key IS NULL OR p_record_key = '' OR octet_length(p_record_key) > 1024
       OR p_record_json IS NULL OR jsonb_typeof(p_record_json) <> 'object'
       OR octet_length(p_record_json::text) > 1048576 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE_RECORD: invalid type, key, or JSON'
            USING ERRCODE = '22023';
    END IF;
    IF p_record_type = 'chunks' THEN
        IF p_content IS NULL OR octet_length(p_content) < 1
           OR octet_length(p_content) > 65536
           OR coalesce((p_record_json->>'size')::integer, -1) <> octet_length(p_content)
           OR p_record_json->>'checksum' <> encode(pg_catalog.sha256(p_content), 'hex') THEN
            RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE_CHUNK: size or checksum mismatch'
                USING ERRCODE = '22023';
        END IF;
    ELSIF p_content IS NOT NULL THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE_RECORD: only chunks carry content'
            USING ERRCODE = '22023';
    END IF;
    INSERT INTO _vexfs.archive_import_records(
        job_id, record_type, record_key, record_json, content)
    VALUES (p_job, p_record_type, p_record_key, p_record_json, p_content);
    SELECT count(*) INTO v_count
      FROM _vexfs.archive_import_records AS record
     WHERE record.job_id = p_job;
    IF v_count > 10000000 THEN
        RAISE EXCEPTION 'VEXFS_ARCHIVE_TOO_LARGE: record limit exceeded'
            USING ERRCODE = '54000';
    END IF;
    RETURN v_count;
END;
$$;

CREATE FUNCTION public.vexfs_archive_import_abort(p_job bigint)
RETURNS integer
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_actor oid;
    v_removed integer;
BEGIN
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    DELETE FROM _vexfs.archive_import_jobs AS job
     WHERE job.job_id = p_job
       AND job.owner_oid = v_actor
       AND job.backend_pid = pg_catalog.pg_backend_pid();
    GET DIAGNOSTICS v_removed = ROW_COUNT;
    RETURN v_removed;
END;
$$;

CREATE FUNCTION public.vexfs_archive_import_finish(p_job bigint)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
VOLATILE
SET search_path = pg_catalog, _vexfs
AS $$
DECLARE
    v_job _vexfs.archive_import_jobs%ROWTYPE;
    v_actor oid;
    v_workspace_id bigint;
    v_source_head bigint;
    v_source_floor bigint;
    v_source_root bigint;
    v_head bigint;
    v_floor bigint;
    v_root bigint;
    v_bad bigint;
    v_check jsonb;
    v_versions bigint;
    v_content_bytes bigint;
    v_empty_manifest bigint;
    v_acl record;
    v_acl_set bigint;
BEGIN
    SELECT r.oid INTO STRICT v_actor
      FROM pg_catalog.pg_roles AS r WHERE r.rolname = session_user;
    SELECT * INTO v_job
      FROM _vexfs.archive_import_jobs AS job
     WHERE job.job_id = p_job
       AND job.owner_oid = v_actor
       AND job.backend_pid = pg_catalog.pg_backend_pid()
     FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_IMPORT_JOB_NOT_FOUND: %', p_job
            USING ERRCODE = 'P0002';
    END IF;
    IF EXISTS (SELECT 1 FROM _vexfs.workspaces AS w WHERE w.name = v_job.workspace_name) THEN
        RAISE EXCEPTION 'VEXFS_WORKSPACE_EXISTS: %', v_job.workspace_name
            USING ERRCODE = '23505';
    END IF;
    v_source_head := (v_job.manifest->>'source_commit')::bigint;
    v_source_floor := (v_job.manifest->>'history_floor_source_commit')::bigint;
    v_source_root := (v_job.manifest->>'root_source_inode')::bigint;

    SELECT count(*) INTO v_bad
      FROM _vexfs.archive_import_records AS record
     WHERE record.job_id = p_job
       AND record.record_type = 'commits';
    IF v_bad < 1 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: commits are missing'
            USING ERRCODE = '22023';
    END IF;
    WITH commits AS (
        SELECT (record.record_json->>'source_id')::bigint AS source_id,
               (record.record_json->>'parent_source_id')::bigint AS parent_source_id,
               lag((record.record_json->>'source_id')::bigint) OVER (
                   ORDER BY (record.record_json->>'source_id')::bigint) AS expected_parent
          FROM _vexfs.archive_import_records AS record
         WHERE record.job_id = p_job AND record.record_type = 'commits')
    SELECT count(*) INTO v_bad
      FROM commits
     WHERE parent_source_id IS DISTINCT FROM expected_parent;
    IF v_bad <> 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: commit chain is not linear'
            USING ERRCODE = '22023';
    END IF;

    DROP TABLE IF EXISTS pg_temp.vexfs_import_commit_map;
    DROP TABLE IF EXISTS pg_temp.vexfs_import_inode_map;
    DROP TABLE IF EXISTS pg_temp.vexfs_import_manifest_map;
    DROP TABLE IF EXISTS pg_temp.vexfs_import_chunk_map;
    DROP TABLE IF EXISTS pg_temp.vexfs_import_snapshot_map;
    CREATE TEMP TABLE vexfs_import_commit_map(
        source_id bigint PRIMARY KEY,
        local_id bigint UNIQUE NOT NULL) ON COMMIT DROP;
    CREATE TEMP TABLE vexfs_import_inode_map(
        source_id bigint PRIMARY KEY,
        local_id bigint UNIQUE NOT NULL) ON COMMIT DROP;
    CREATE TEMP TABLE vexfs_import_manifest_map(
        source_id bigint PRIMARY KEY,
        local_id bigint NOT NULL) ON COMMIT DROP;
    CREATE TEMP TABLE vexfs_import_chunk_map(
        source_manifest bigint NOT NULL,
        chunk_no integer NOT NULL,
        local_id bigint UNIQUE NOT NULL,
        PRIMARY KEY(source_manifest, chunk_no)) ON COMMIT DROP;
    CREATE TEMP TABLE vexfs_import_snapshot_map(
        name text PRIMARY KEY,
        source_commit bigint NOT NULL,
        local_id bigint UNIQUE NOT NULL) ON COMMIT DROP;

    INSERT INTO pg_temp.vexfs_import_commit_map(source_id, local_id)
    SELECT (record.record_json->>'source_id')::bigint,
           row_number() OVER (ORDER BY (record.record_json->>'source_id')::bigint)::bigint
      FROM _vexfs.archive_import_records AS record
     WHERE record.job_id = p_job AND record.record_type = 'commits';
    SELECT map.local_id INTO v_head
      FROM pg_temp.vexfs_import_commit_map AS map WHERE map.source_id = v_source_head;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: source head is missing'
            USING ERRCODE = '22023';
    END IF;
    SELECT map.local_id INTO v_floor
      FROM pg_temp.vexfs_import_commit_map AS map WHERE map.source_id = v_source_floor;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: history floor is missing'
            USING ERRCODE = '22023';
    END IF;

    INSERT INTO _vexfs.workspaces(
        name, state, owner_oid, owner_role, root_inode, head_commit,
        history_floor_commit, cache_generation, quota_max_bytes, quota_max_files,
        quota_max_file_bytes, retention_keep_versions, retention_keep_days, created_at)
    VALUES (
        v_job.workspace_name, 'importing', v_actor, session_user, NULL, 0, 0, 0,
        (v_job.manifest->>'quota_max_bytes')::bigint,
        (v_job.manifest->>'quota_max_files')::bigint,
        (v_job.manifest->>'quota_max_file_bytes')::bigint,
        (v_job.manifest->>'retention_keep_versions')::integer,
        (v_job.manifest->>'retention_keep_days')::integer,
        to_timestamp((v_job.manifest->>'created_at')::bigint / 1000.0))
    RETURNING workspace_id INTO v_workspace_id;

    INSERT INTO _vexfs.commits(
        workspace_id, commit_no, parent_commit, operation, path,
        created_by_oid, created_by, session_id, run_id, created_at)
    SELECT v_workspace_id,
           map.local_id,
           map.local_id - 1,
           record.record_json->>'message',
           record.record_json->>'path',
           v_actor,
           session_user,
           record.record_json->>'session_id',
           record.record_json->>'run_id',
           to_timestamp((record.record_json->>'created_at')::bigint / 1000.0)
      FROM _vexfs.archive_import_records AS record
      JOIN pg_temp.vexfs_import_commit_map AS map
        ON map.source_id = (record.record_json->>'source_id')::bigint
     WHERE record.job_id = p_job AND record.record_type = 'commits'
     ORDER BY map.local_id;

    INSERT INTO pg_temp.vexfs_import_inode_map(source_id, local_id)
    SELECT (record.record_json->>'source_id')::bigint,
           nextval(pg_get_serial_sequence('_vexfs.inodes', 'inode_id'))
      FROM _vexfs.archive_import_records AS record
     WHERE record.job_id = p_job AND record.record_type = 'inodes'
     ORDER BY (record.record_json->>'source_id')::bigint;
    INSERT INTO _vexfs.inodes(
        inode_id, workspace_id, kind, mode, owner_oid, owner_role, owner_principal,
        uid, gid, current_version, size_bytes, live, created_at, accessed_at,
        modified_at, changed_at)
    OVERRIDING SYSTEM VALUE
    SELECT map.local_id,
           v_workspace_id,
           record.record_json->>'kind',
           (record.record_json->>'mode')::integer,
           v_actor,
           session_user,
           record.record_json->>'owner_principal',
           (record.record_json->>'uid')::bigint,
           (record.record_json->>'gid')::bigint,
           CASE WHEN record.record_json->>'kind' = 'directory' THEN 0
                ELSE (record.record_json->>'current_version')::bigint END,
           (record.record_json->>'size')::bigint,
           (record.record_json->>'deleted_at') IS NULL,
           to_timestamp((record.record_json->>'created_at')::bigint / 1000.0),
           to_timestamp((record.record_json->>'accessed_at')::bigint / 1000.0),
           to_timestamp((record.record_json->>'updated_at')::bigint / 1000.0),
           to_timestamp((record.record_json->>'changed_at')::bigint / 1000.0)
      FROM _vexfs.archive_import_records AS record
      JOIN pg_temp.vexfs_import_inode_map AS map
        ON map.source_id = (record.record_json->>'source_id')::bigint
     WHERE record.job_id = p_job AND record.record_type = 'inodes'
     ORDER BY map.source_id;
    SELECT map.local_id INTO v_root
      FROM pg_temp.vexfs_import_inode_map AS map WHERE map.source_id = v_source_root;
    IF NOT FOUND OR NOT EXISTS (
        SELECT 1 FROM _vexfs.inodes AS i
         WHERE i.inode_id = v_root AND i.workspace_id = v_workspace_id
           AND i.kind = 'directory' AND i.live) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: root inode is missing or invalid'
            USING ERRCODE = '22023';
    END IF;

    INSERT INTO _vexfs.commit_changes(
        workspace_id, commit_no, ordinal, operation, path, inode_id,
        before_version, after_version, details)
    SELECT v_workspace_id,
           commit_map.local_id,
           (record.record_json->>'ordinal')::integer,
           record.record_json->>'operation',
           record.record_json->>'path',
           inode_map.local_id,
           (record.record_json->>'before_version')::bigint,
           (record.record_json->>'after_version')::bigint,
           coalesce(record.record_json->'details', '{}'::jsonb)
      FROM _vexfs.archive_import_records AS record
      JOIN pg_temp.vexfs_import_commit_map AS commit_map
        ON commit_map.source_id = (record.record_json->>'source_commit')::bigint
      LEFT JOIN pg_temp.vexfs_import_inode_map AS inode_map
        ON inode_map.source_id = (record.record_json->>'source_inode')::bigint
     WHERE record.job_id = p_job
       AND record.record_type = 'commit_changes'
     ORDER BY commit_map.local_id, (record.record_json->>'ordinal')::integer;

    -- SQLite archives created before path-level change records still carry a
    -- complete commit chain. Preserve that history with one explicit marker
    -- per commit so the imported workspace passes the same integrity rules.
    IF NOT EXISTS (
        SELECT 1
          FROM _vexfs.archive_import_records AS record
         WHERE record.job_id = p_job
           AND record.record_type = 'commit_changes') THEN
        INSERT INTO _vexfs.commit_changes(
            workspace_id, commit_no, ordinal, operation, path, inode_id,
            before_version, after_version, details)
        SELECT v_workspace_id,
               commit_map.local_id,
               1,
               record.record_json->>'message',
               '/',
               v_root,
               NULL,
               NULL,
               jsonb_build_object('imported_without_change_detail', true)
          FROM _vexfs.archive_import_records AS record
          JOIN pg_temp.vexfs_import_commit_map AS commit_map
            ON commit_map.source_id = (record.record_json->>'source_id')::bigint
         WHERE record.job_id = p_job
           AND record.record_type = 'commits'
         ORDER BY commit_map.local_id;
    END IF;

    INSERT INTO pg_temp.vexfs_import_manifest_map(source_id, local_id)
    SELECT (record.record_json->>'source_id')::bigint,
           nextval(pg_get_serial_sequence('_vexfs.manifests', 'manifest_id'))
      FROM _vexfs.archive_import_records AS record
     WHERE record.job_id = p_job
       AND record.record_type = 'manifests'
       AND ((record.record_json->>'file_size')::bigint <> 0
            OR (record.record_json->>'chunk_count')::integer <> 0)
     ORDER BY (record.record_json->>'source_id')::bigint;
    IF EXISTS (
        SELECT 1
          FROM _vexfs.archive_import_records AS record
         WHERE record.job_id = p_job
           AND record.record_type = 'manifests'
           AND (record.record_json->>'file_size')::bigint = 0
           AND (record.record_json->>'chunk_count')::integer = 0) THEN
        v_empty_manifest := nextval(
            pg_get_serial_sequence('_vexfs.manifests', 'manifest_id'));
        INSERT INTO pg_temp.vexfs_import_manifest_map(source_id, local_id)
        SELECT (record.record_json->>'source_id')::bigint,
               v_empty_manifest
          FROM _vexfs.archive_import_records AS record
         WHERE record.job_id = p_job
           AND record.record_type = 'manifests'
           AND (record.record_json->>'file_size')::bigint = 0
           AND (record.record_json->>'chunk_count')::integer = 0
         ORDER BY (record.record_json->>'source_id')::bigint;
    END IF;
    INSERT INTO _vexfs.manifests(
        manifest_id, workspace_id, file_size, chunk_size,
        chunk_count, checksum, created_at)
    OVERRIDING SYSTEM VALUE
    SELECT DISTINCT ON (manifest_map.local_id)
           manifest_map.local_id,
           v_workspace_id,
           (manifest.record_json->>'file_size')::bigint,
           (manifest.record_json->>'chunk_size')::integer,
           (manifest.record_json->>'chunk_count')::integer,
           manifest.record_json->>'checksum',
           to_timestamp((manifest.record_json->>'created_at')::bigint / 1000.0)
      FROM _vexfs.archive_import_records AS manifest
     JOIN pg_temp.vexfs_import_manifest_map AS manifest_map
        ON manifest_map.source_id = (manifest.record_json->>'source_id')::bigint
     WHERE manifest.job_id = p_job AND manifest.record_type = 'manifests'
     ORDER BY manifest_map.local_id,
              (manifest.record_json->>'source_id')::bigint;

    INSERT INTO pg_temp.vexfs_import_chunk_map(
        source_manifest, chunk_no, local_id)
    SELECT (record.record_json->>'source_manifest')::bigint,
           (record.record_json->>'chunk_no')::integer,
           nextval(pg_get_serial_sequence('_vexfs.chunks', 'chunk_id'))
      FROM _vexfs.archive_import_records AS record
     WHERE record.job_id = p_job AND record.record_type = 'chunks'
     ORDER BY (record.record_json->>'source_manifest')::bigint,
              (record.record_json->>'chunk_no')::integer;
    INSERT INTO _vexfs.chunks(
        chunk_id, workspace_id, content, size_bytes, checksum)
    OVERRIDING SYSTEM VALUE
    SELECT chunk_map.local_id,
           v_workspace_id,
           record.content,
           (record.record_json->>'size')::integer,
           record.record_json->>'checksum'
      FROM _vexfs.archive_import_records AS record
      JOIN pg_temp.vexfs_import_chunk_map AS chunk_map
        ON chunk_map.source_manifest = (record.record_json->>'source_manifest')::bigint
       AND chunk_map.chunk_no = (record.record_json->>'chunk_no')::integer
     WHERE record.job_id = p_job AND record.record_type = 'chunks'
     ORDER BY chunk_map.source_manifest, chunk_map.chunk_no;
    INSERT INTO _vexfs.manifest_chunks(manifest_id, chunk_no, chunk_id)
    SELECT manifest_map.local_id, chunk_map.chunk_no, chunk_map.local_id
      FROM pg_temp.vexfs_import_chunk_map AS chunk_map
      JOIN pg_temp.vexfs_import_manifest_map AS manifest_map
        ON manifest_map.source_id = chunk_map.source_manifest;

    INSERT INTO _vexfs.file_versions(
        workspace_id, inode_id, version_no, commit_no, manifest_id,
        source_version_no, size_bytes, checksum, created_by_oid, created_by, created_at)
    SELECT v_workspace_id,
           inode_map.local_id,
           (record.record_json->>'version_no')::bigint,
           commit_map.local_id,
           manifest_map.local_id,
           (record.record_json->>'source_version_no')::bigint,
           (record.record_json->>'size')::bigint,
           record.record_json->>'checksum',
           v_actor,
           session_user,
           to_timestamp((record.record_json->>'created_at')::bigint / 1000.0)
      FROM _vexfs.archive_import_records AS record
      JOIN pg_temp.vexfs_import_inode_map AS inode_map
        ON inode_map.source_id = (record.record_json->>'source_inode')::bigint
      JOIN pg_temp.vexfs_import_commit_map AS commit_map
        ON commit_map.source_id = (record.record_json->>'source_commit')::bigint
      LEFT JOIN pg_temp.vexfs_import_manifest_map AS manifest_map
        ON manifest_map.source_id = (record.record_json->>'source_manifest')::bigint
     WHERE record.job_id = p_job AND record.record_type = 'file_versions'
     ORDER BY inode_map.source_id, (record.record_json->>'version_no')::bigint;

    INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
    SELECT v_workspace_id, parent_map.local_id,
           record.record_json->>'name', child_map.local_id
      FROM _vexfs.archive_import_records AS record
      JOIN pg_temp.vexfs_import_inode_map AS parent_map
        ON parent_map.source_id = (record.record_json->>'parent_source_inode')::bigint
      JOIN pg_temp.vexfs_import_inode_map AS child_map
        ON child_map.source_id = (record.record_json->>'inode_source_id')::bigint
     WHERE record.job_id = p_job AND record.record_type = 'dentries';
    INSERT INTO _vexfs.xattrs(workspace_id, inode_id, name, value, updated_at)
    SELECT v_workspace_id, inode_map.local_id,
           record.record_json->>'name',
           decode(record.record_json->>'value_hex', 'hex'),
           to_timestamp((record.record_json->>'updated_at')::bigint / 1000.0)
      FROM _vexfs.archive_import_records AS record
      JOIN pg_temp.vexfs_import_inode_map AS inode_map
        ON inode_map.source_id = (record.record_json->>'source_inode')::bigint
     WHERE record.job_id = p_job AND record.record_type = 'xattrs';
    FOR v_acl IN
        SELECT inode_map.local_id AS inode_id,
               jsonb_agg(jsonb_build_object(
                   'principal', record.record_json->>'principal_id',
                   'effect', record.record_json->>'effect',
                   'permissions', record.record_json->>'permissions',
                   'inherit', (record.record_json->>'inherit_flags')::integer)
                   ORDER BY record.record_json->>'principal_id',
                            record.record_json->>'effect') AS entries
          FROM _vexfs.archive_import_records AS record
          JOIN pg_temp.vexfs_import_inode_map AS inode_map
            ON inode_map.source_id = (record.record_json->>'source_inode')::bigint
         WHERE record.job_id = p_job AND record.record_type = 'acl_entries'
         GROUP BY inode_map.local_id
    LOOP
        v_acl_set := _vexfs.get_or_create_acl_set(v_workspace_id, v_acl.entries);
        UPDATE _vexfs.inodes AS inode
           SET acl_set_id = v_acl_set
         WHERE inode.workspace_id = v_workspace_id
           AND inode.inode_id = v_acl.inode_id;
    END LOOP;

    INSERT INTO pg_temp.vexfs_import_snapshot_map(name, source_commit, local_id)
    SELECT record.record_json->>'name',
           (record.record_json->>'source_commit')::bigint,
           nextval(pg_get_serial_sequence('_vexfs.snapshots', 'snapshot_id'))
      FROM _vexfs.archive_import_records AS record
     WHERE record.job_id = p_job AND record.record_type = 'snapshots'
     ORDER BY (record.record_json->>'source_commit')::bigint,
              record.record_json->>'name';
    INSERT INTO _vexfs.snapshots(
        snapshot_id, workspace_id, name, head_commit,
        created_by_oid, created_by, created_at)
    OVERRIDING SYSTEM VALUE
    SELECT snapshot_map.local_id,
           v_workspace_id,
           snapshot_map.name,
           commit_map.local_id,
           v_actor,
           session_user,
           to_timestamp((record.record_json->>'created_at')::bigint / 1000.0)
      FROM pg_temp.vexfs_import_snapshot_map AS snapshot_map
      JOIN pg_temp.vexfs_import_commit_map AS commit_map
        ON commit_map.source_id = snapshot_map.source_commit
      JOIN _vexfs.archive_import_records AS record
        ON record.job_id = p_job AND record.record_type = 'snapshots'
       AND record.record_json->>'name' = snapshot_map.name;

    INSERT INTO _vexfs.snapshot_inodes(
        snapshot_id, inode_id, kind, mode, owner_oid, owner_role, owner_principal,
        uid, gid, current_version, size_bytes, created_at, accessed_at,
        modified_at, changed_at)
    SELECT snapshot_map.local_id,
           inode_map.local_id,
           state.kind,
           state.mode,
           v_actor,
           session_user,
           state.owner_principal,
           state.uid,
           state.gid,
           CASE WHEN state.kind = 'directory' THEN 0
                ELSE state.current_version END,
           state.size_bytes,
           to_timestamp(state.created_at_ms / 1000.0),
           to_timestamp(state.accessed_at_ms / 1000.0),
           to_timestamp(state.updated_at_ms / 1000.0),
           to_timestamp(state.changed_at_ms / 1000.0)
      FROM pg_temp.vexfs_import_snapshot_map AS snapshot_map
      JOIN LATERAL (
          SELECT DISTINCT ON ((record.record_json->>'source_inode')::bigint)
                 (record.record_json->>'source_inode')::bigint AS source_inode,
                 record.record_json->>'kind' AS kind,
                 (record.record_json->>'mode')::integer AS mode,
                 record.record_json->>'owner_principal' AS owner_principal,
                 (record.record_json->>'uid')::bigint AS uid,
                 (record.record_json->>'gid')::bigint AS gid,
                 (record.record_json->>'size')::bigint AS size_bytes,
                 (record.record_json->>'current_version')::bigint AS current_version,
                 (record.record_json->>'created_at')::bigint AS created_at_ms,
                 (record.record_json->>'accessed_at')::bigint AS accessed_at_ms,
                 (record.record_json->>'updated_at')::bigint AS updated_at_ms,
                 (record.record_json->>'changed_at')::bigint AS changed_at_ms,
                 record.record_json->>'deleted_at' AS deleted_at
            FROM _vexfs.archive_import_records AS record
           WHERE record.job_id = p_job AND record.record_type = 'inode_states'
             AND (record.record_json->>'source_commit')::bigint <= snapshot_map.source_commit
           ORDER BY (record.record_json->>'source_inode')::bigint,
                    (record.record_json->>'source_commit')::bigint DESC) AS state
        ON state.deleted_at IS NULL
      JOIN pg_temp.vexfs_import_inode_map AS inode_map
        ON inode_map.source_id = state.source_inode;

    INSERT INTO _vexfs.snapshot_dentries(snapshot_id, parent_inode, name, inode_id)
    SELECT snapshot_map.local_id, parent_map.local_id, state.name, child_map.local_id
      FROM pg_temp.vexfs_import_snapshot_map AS snapshot_map
      JOIN LATERAL (
          SELECT DISTINCT ON (
                     (record.record_json->>'parent_source_inode')::bigint,
                     record.record_json->>'name')
                 (record.record_json->>'parent_source_inode')::bigint AS parent_source,
                 record.record_json->>'name' AS name,
                 (record.record_json->>'inode_source_id')::bigint AS child_source,
                 (record.record_json->>'deleted')::integer AS deleted
            FROM _vexfs.archive_import_records AS record
           WHERE record.job_id = p_job AND record.record_type = 'dentry_states'
             AND (record.record_json->>'source_commit')::bigint <= snapshot_map.source_commit
           ORDER BY (record.record_json->>'parent_source_inode')::bigint,
                    record.record_json->>'name',
                    (record.record_json->>'source_commit')::bigint DESC) AS state
        ON state.deleted = 0
      JOIN pg_temp.vexfs_import_inode_map AS parent_map
        ON parent_map.source_id = state.parent_source
      JOIN pg_temp.vexfs_import_inode_map AS child_map
        ON child_map.source_id = state.child_source;

    INSERT INTO _vexfs.snapshot_xattrs(snapshot_id, inode_id, name, value, updated_at)
    SELECT snapshot_map.local_id, inode_map.local_id, state.name,
           decode(state.value_hex, 'hex'),
           to_timestamp((snapshot_record.record_json->>'created_at')::bigint / 1000.0)
      FROM pg_temp.vexfs_import_snapshot_map AS snapshot_map
      JOIN _vexfs.archive_import_records AS snapshot_record
        ON snapshot_record.job_id = p_job
       AND snapshot_record.record_type = 'snapshots'
       AND snapshot_record.record_json->>'name' = snapshot_map.name
      JOIN LATERAL (
          SELECT DISTINCT ON (
                     (record.record_json->>'source_inode')::bigint,
                     record.record_json->>'name')
                 (record.record_json->>'source_inode')::bigint AS source_inode,
                 record.record_json->>'name' AS name,
                 record.record_json->>'value_hex' AS value_hex,
                 (record.record_json->>'deleted')::integer AS deleted
            FROM _vexfs.archive_import_records AS record
           WHERE record.job_id = p_job AND record.record_type = 'xattr_states'
             AND (record.record_json->>'source_commit')::bigint <= snapshot_map.source_commit
           ORDER BY (record.record_json->>'source_inode')::bigint,
                    record.record_json->>'name',
                    (record.record_json->>'source_commit')::bigint DESC) AS state
        ON state.deleted = 0
      JOIN pg_temp.vexfs_import_inode_map AS inode_map
        ON inode_map.source_id = state.source_inode;

    FOR v_acl IN
        SELECT snapshot_map.local_id AS snapshot_id,
               inode_map.local_id AS inode_id,
               jsonb_agg(jsonb_build_object(
                   'principal', state.principal,
                   'effect', state.effect,
                   'permissions', state.permissions,
                   'inherit', state.inherit_flags)
                   ORDER BY state.principal, state.effect) AS entries
          FROM pg_temp.vexfs_import_snapshot_map AS snapshot_map
          JOIN LATERAL (
              SELECT DISTINCT ON (
                         (record.record_json->>'source_inode')::bigint,
                         record.record_json->>'principal_id',
                         record.record_json->>'effect')
                     (record.record_json->>'source_inode')::bigint AS source_inode,
                     record.record_json->>'principal_id' AS principal,
                     record.record_json->>'effect' AS effect,
                     record.record_json->>'permissions' AS permissions,
                     (record.record_json->>'inherit_flags')::integer AS inherit_flags,
                     (record.record_json->>'deleted')::integer AS deleted
                FROM _vexfs.archive_import_records AS record
               WHERE record.job_id = p_job AND record.record_type = 'acl_states'
                 AND (record.record_json->>'source_commit')::bigint <= snapshot_map.source_commit
               ORDER BY (record.record_json->>'source_inode')::bigint,
                        record.record_json->>'principal_id',
                        record.record_json->>'effect',
                        (record.record_json->>'source_commit')::bigint DESC) AS state
            ON state.deleted = 0
          JOIN pg_temp.vexfs_import_inode_map AS inode_map
            ON inode_map.source_id = state.source_inode
         GROUP BY snapshot_map.local_id, inode_map.local_id
    LOOP
        v_acl_set := _vexfs.get_or_create_acl_set(v_workspace_id, v_acl.entries);
        UPDATE _vexfs.snapshot_inodes AS inode
           SET acl_set_id = v_acl_set
         WHERE inode.snapshot_id = v_acl.snapshot_id
           AND inode.inode_id = v_acl.inode_id;
    END LOOP;

    UPDATE _vexfs.workspaces
       SET root_inode = v_root,
           head_commit = v_head,
           history_floor_commit = v_floor,
           cache_generation = 1,
           state = 'active'
     WHERE workspace_id = v_workspace_id;
    SELECT count(*) INTO v_bad
      FROM _vexfs.workspaces AS w
     WHERE w.workspace_id = v_workspace_id
       AND ((w.quota_max_bytes IS NOT NULL AND w.live_bytes > w.quota_max_bytes)
            OR (w.quota_max_files IS NOT NULL AND w.live_files > w.quota_max_files)
            OR (w.quota_max_file_bytes IS NOT NULL AND EXISTS (
                SELECT 1 FROM _vexfs.inodes AS i
                 WHERE i.workspace_id = w.workspace_id
                   AND i.live AND i.kind <> 'directory'
                   AND i.size_bytes > w.quota_max_file_bytes)));
    IF v_bad <> 0 THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: imported live data exceeds its quota'
            USING ERRCODE = '22023';
    END IF;
    v_check := public.vexfs_check(v_job.workspace_name, 1);
    IF NOT coalesce((v_check->>'ok')::boolean, false) THEN
        RAISE EXCEPTION 'VEXFS_INVALID_ARCHIVE: imported workspace failed deep check: %',
            v_check::text USING ERRCODE = 'XX001';
    END IF;
    SELECT count(*), coalesce(sum(f.size_bytes), 0)
      INTO v_versions, v_content_bytes
      FROM _vexfs.file_versions AS f
     WHERE f.workspace_id = v_workspace_id;
    PERFORM _vexfs.audit(
        v_workspace_id, v_head, 'archive_import', '/', v_root,
        jsonb_build_object(
            'before_version', NULL,
            'after_version', v_head,
            'source_engine', v_job.manifest->>'source_engine',
            'source_workspace', v_job.manifest->>'source_workspace',
            'source_commit', v_source_head));
    DELETE FROM _vexfs.archive_import_jobs WHERE job_id = p_job;
    RETURN jsonb_build_object(
        'workspace', v_job.workspace_name,
        'source_workspace', v_job.manifest->>'source_workspace',
        'source_commit', v_source_head,
        'versions', v_versions,
        'content_bytes', v_content_bytes,
        'head_commit', v_head,
        'root_inode', v_root);
END;
$$;

REVOKE ALL ON ALL TABLES IN SCHEMA _vexfs FROM PUBLIC;
REVOKE ALL ON ALL SEQUENCES IN SCHEMA _vexfs FROM PUBLIC;
REVOKE ALL ON ALL FUNCTIONS IN SCHEMA _vexfs FROM PUBLIC;

COMMENT ON SCHEMA _vexfs IS
    'Private PostgreSQL storage for the VexFS adapter; use public vexfs_* functions';
COMMENT ON FUNCTION public.vexfs_pg_adapter_version() IS
    'Returns the PostgreSQL VexFS adapter alpha implementation version';
COMMENT ON FUNCTION public.vexfs_contract_version() IS
    'Returns the database-neutral VexFS SQL contract version implemented by this adapter';
COMMENT ON FUNCTION public.vexfs_init() IS
    'Idempotent PostgreSQL VexFS readiness probe; CREATE EXTENSION owns schema initialization';
COMMENT ON FUNCTION public.vexfs_workspace_create(text) IS
    'Creates a PostgreSQL-managed VexFS workspace owned by the authenticated session role';
COMMENT ON FUNCTION public.vexfs_workspace_log(text, integer, bigint) IS
    'Returns one bounded page of workspace commits with path, actor, run and snapshot metadata';
COMMENT ON FUNCTION public.vexfs_write(text, text, bytea) IS
    'Writes a complete file version in the caller transaction';
COMMENT ON FUNCTION public.vexfs_create_batch(text, text, jsonb) IS
    'Creates 1 to 1000 direct children with one commit and path-level change records';
COMMENT ON FUNCTION public.vexfs_read(text, text) IS
    'Reads the current file version from PostgreSQL-managed VexFS storage';
