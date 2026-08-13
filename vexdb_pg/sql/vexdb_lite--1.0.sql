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
        memory_mode       text,
        quantizer         text,
        rabitq_codes_bytes int8,
        rabitq_fixed_bytes int8)
    AS 'MODULE_PATHNAME' LANGUAGE C;

COMMENT ON FUNCTION vexdb_index_info() IS
    'Lists all vexdb_graph indexes with metadata (mirrors duck-side schema)';
