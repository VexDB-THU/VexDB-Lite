-- vexdb_lite 1.0 -> 1.1
--
-- 1.1 adds RaBitQ metadata to vexdb_index_info(). PostgreSQL cannot change
-- OUT columns with CREATE OR REPLACE FUNCTION, so recreate the extension-owned
-- function while keeping its public name and argument list stable.

DROP FUNCTION vexdb_index_info();

CREATE FUNCTION vexdb_index_info()
    RETURNS TABLE(
        index_name         text,
        indexname          text,
        index_type         text,
        table_name         text,
        partition_count    int4,
        node_count         int8,
        max_level          int4,
        dimension          int4,
        row_id_map_size    int8,
        m                  int4,
        ef_construction    int4,
        metric             text,
        use_pq             bool,
        pq_m               int4,
        memory_bytes       int8,
        pq_codes_bytes     int8,
        pq_codebook_bytes  int8,
        memory_mode        text,
        quantizer          text,
        rabitq_codes_bytes int8,
        rabitq_fixed_bytes int8)
    AS 'MODULE_PATHNAME' LANGUAGE C;

COMMENT ON FUNCTION vexdb_index_info() IS
    'Lists all vexdb_graph indexes with metadata (mirrors duck-side schema)';
