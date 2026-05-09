-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_vexdb" to load this file. \quit

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

CREATE FUNCTION floatvector_to_float4(floatvector, integer, boolean) RETURNS real[]
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector casts

CREATE CAST (floatvector AS floatvector)
    WITH FUNCTION floatvector(floatvector, integer, boolean) AS IMPLICIT;

CREATE CAST (floatvector AS real[])
    WITH FUNCTION floatvector_to_float4(floatvector, integer, boolean) AS IMPLICIT;

CREATE CAST (real[] AS floatvector)
    WITH FUNCTION array_to_floatvector(real[], integer, boolean) AS ASSIGNMENT;

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

-- Duck-side parity: <~> aliases <=> for cosine distance.
CREATE OPERATOR <~> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = cosine_distance,
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

-- halfvector type

CREATE TYPE halfvector;

CREATE FUNCTION halfvector_in(cstring, oid, integer) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_out(halfvector) RETURNS cstring
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_typmod_in(cstring[]) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_recv(internal, oid, integer) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_send(halfvector) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE halfvector (
    INPUT     = halfvector_in,
    OUTPUT    = halfvector_out,
    TYPMOD_IN = halfvector_typmod_in,
    RECEIVE   = halfvector_recv,
    SEND      = halfvector_send,
    STORAGE   = external
);

-- halfvector distance functions

CREATE FUNCTION halfvector_l2_distance(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_l2_squared_distance(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_inner_product(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_negative_inner_product(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_cosine_distance(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_spherical_distance(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- halfvector utility functions

CREATE FUNCTION halfvector_dims(halfvector) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_l2_norm(halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_l2_normalize(halfvector) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_subvector(halfvector, int, int) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- halfvector private functions

CREATE FUNCTION halfvector_add(halfvector, halfvector) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_sub(halfvector, halfvector) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_lt(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_le(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_eq(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_ne(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_ge(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_gt(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_cmp(halfvector, halfvector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hashhalfvector(halfvector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- halfvector cast functions

CREATE FUNCTION halfvector(halfvector, integer, boolean) RETURNS halfvector
    AS 'MODULE_PATHNAME', 'halfvector_to_halfvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_to_floatvector(halfvector, integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_to_halfvector(floatvector, integer, boolean) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_halfvector(real[], integer, boolean) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_to_float4(halfvector, integer, boolean) RETURNS real[]
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- halfvector casts

CREATE CAST (halfvector AS halfvector)
    WITH FUNCTION halfvector(halfvector, integer, boolean) AS IMPLICIT;

CREATE CAST (halfvector AS floatvector)
    WITH FUNCTION halfvector_to_floatvector(halfvector, integer, boolean) AS ASSIGNMENT;

CREATE CAST (floatvector AS halfvector)
    WITH FUNCTION floatvector_to_halfvector(floatvector, integer, boolean) AS IMPLICIT;

CREATE CAST (halfvector AS real[])
    WITH FUNCTION halfvector_to_float4(halfvector, integer, boolean) AS ASSIGNMENT;

CREATE CAST (real[] AS halfvector)
    WITH FUNCTION array_to_halfvector(real[], integer, boolean) AS ASSIGNMENT;

-- halfvector operators

CREATE OPERATOR <-> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_l2_distance,
    COMMUTATOR = '<->'
);

CREATE OPERATOR <#> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_negative_inner_product,
    COMMUTATOR = '<#>'
);

CREATE OPERATOR <=> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_cosine_distance,
    COMMUTATOR = '<=>'
);

CREATE OPERATOR + (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_add,
    COMMUTATOR = +
);

CREATE OPERATOR - (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_sub
);

CREATE OPERATOR < (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_lt,
    COMMUTATOR = >, NEGATOR = >=,
    RESTRICT = scalarltsel, JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_le,
    COMMUTATOR = >=, NEGATOR = >,
    RESTRICT = scalarlesel, JOIN = scalarlejoinsel
);

CREATE OPERATOR = (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_eq,
    COMMUTATOR = =, NEGATOR = <>,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR <> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_ne,
    COMMUTATOR = <>, NEGATOR = =,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR >= (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_ge,
    COMMUTATOR = <=, NEGATOR = <,
    RESTRICT = scalargesel, JOIN = scalargejoinsel
);

CREATE OPERATOR > (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_gt,
    COMMUTATOR = <, NEGATOR = <=,
    RESTRICT = scalargtsel, JOIN = scalargtjoinsel
);

-- halfvector opclasses

CREATE OPERATOR CLASS halfvector_ops
    DEFAULT FOR TYPE halfvector USING btree AS
    OPERATOR 1 <,
    OPERATOR 2 <=,
    OPERATOR 3 =,
    OPERATOR 4 >=,
    OPERATOR 5 >,
    FUNCTION 1 halfvector_cmp(halfvector, halfvector);

CREATE OPERATOR CLASS hash_halfvector_ops
    FOR TYPE halfvector USING hash AS
    OPERATOR 1 =,
    FUNCTION 1 hashhalfvector(halfvector);

-- access method

CREATE FUNCTION vexdb_graph_amhandler(internal) RETURNS index_am_handler
    AS 'MODULE_PATHNAME', 'graph_index_amhandler' LANGUAGE C;

CREATE ACCESS METHOD vexdb_graph
    TYPE INDEX
    HANDLER vexdb_graph_amhandler;

COMMENT ON ACCESS METHOD vexdb_graph IS 'HNSW graph index access method for vector similarity search';

-- vexdb_graph opclasses for floatvector

CREATE OPERATOR CLASS floatvector_l2_ops
    FOR TYPE floatvector USING vexdb_graph AS
    OPERATOR 1 <-> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_l2_squared_distance(floatvector, floatvector);

CREATE OPERATOR CLASS floatvector_ip_ops
    FOR TYPE floatvector USING vexdb_graph AS
    OPERATOR 1 <#> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_negative_inner_product(floatvector, floatvector);

CREATE OPERATOR CLASS floatvector_cosine_ops
    DEFAULT FOR TYPE floatvector USING vexdb_graph AS
    OPERATOR 1 <=> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_negative_inner_product(floatvector, floatvector),
    FUNCTION 2 vector_norm(floatvector);

-- vexdb_graph opclasses for halfvector

CREATE OPERATOR CLASS halfvector_l2_ops
    FOR TYPE halfvector USING vexdb_graph AS
    OPERATOR 1 <-> (halfvector, halfvector) FOR ORDER BY float_ops,
    FUNCTION 1 halfvector_l2_squared_distance(halfvector, halfvector);

CREATE OPERATOR CLASS halfvector_ip_ops
    FOR TYPE halfvector USING vexdb_graph AS
    OPERATOR 1 <#> (halfvector, halfvector) FOR ORDER BY float_ops,
    FUNCTION 1 halfvector_negative_inner_product(halfvector, halfvector);

CREATE OPERATOR CLASS halfvector_cosine_ops
    DEFAULT FOR TYPE halfvector USING vexdb_graph AS
    OPERATOR 1 <=> (halfvector, halfvector) FOR ORDER BY float_ops,
    FUNCTION 1 halfvector_negative_inner_product(halfvector, halfvector),
    FUNCTION 2 halfvector_l2_norm(halfvector);

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
