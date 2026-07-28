\set ON_ERROR_STOP on
\pset tuples_only on
\pset format unaligned
\pset fieldsep '|'

DROP TABLE IF EXISTS vex_bench_results;
DROP TABLE IF EXISTS vex_bench_gt;
DROP TABLE IF EXISTS vex_bench_queries;
DROP TABLE IF EXISTS vex_bench_data CASCADE;
DROP FUNCTION IF EXISTS __vex_bench_vec32(int);

CREATE FUNCTION __vex_bench_vec32(i int) RETURNS floatvector
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE AS $$
  SELECT array_agg(
    (sin(i * 0.013 + j * 0.17) +
     cos(i * 0.007 * (j + 1)) +
     ((i % 97) * (j + 1)) * 0.00001)::float4
    ORDER BY j
  )::floatvector
  FROM generate_series(0, 31) AS j
$$;

CREATE TABLE vex_bench_data (id int PRIMARY KEY, vec floatvector(32));
INSERT INTO vex_bench_data
SELECT i, __vex_bench_vec32(i)
FROM generate_series(1, 20000) AS i;
ANALYZE vex_bench_data;

CREATE TABLE vex_bench_queries AS
SELECT q AS qid, d.vec
FROM generate_series(1, 200) AS q
JOIN vex_bench_data d ON d.id = ((q * 97 + 137) % 20000) + 1;

-- Exact truth is created before any ANN index exists. Function-call distance is
-- intentionally used instead of the ordering operator.
CREATE TABLE vex_bench_gt AS
SELECT q.qid, exact.id
FROM vex_bench_queries q
CROSS JOIN LATERAL (
  SELECT d.id
  FROM vex_bench_data d
  ORDER BY l2_distance(d.vec, q.vec), d.id
  LIMIT 10
) exact;

CREATE TABLE vex_bench_results (
  run int NOT NULL,
  mode text NOT NULL,
  build_ms double precision NOT NULL,
  query_ms double precision NOT NULL,
  qps double precision NOT NULL,
  recall_at_10 double precision NOT NULL,
  uses_index boolean NOT NULL,
  quantizer_active boolean NOT NULL,
  memory_bytes bigint NOT NULL,
  code_bytes bigint NOT NULL
);

SET maintenance_work_mem = '2GB';
SET vexdb.ef_search = 160;

DO $$
DECLARE
  modes text[];
  mode_name text;
  create_sql text;
  expected_quantizer text;
  expected_memory_mode text;
  actual_quantizer text;
  actual_memory_mode text;
  plan_line text;
  started_at timestamptz;
  build_elapsed double precision;
  query_elapsed double precision;
  recall_value double precision;
  index_used boolean;
  active boolean;
  mem_bytes bigint;
  quant_bytes bigint;
BEGIN
  FOR run_no IN 1..5 LOOP
    IF run_no % 2 = 1 THEN
      modes := ARRAY['plain', 'pq-full', 'pq-compact', 'rabitq-full', 'rabitq-compact'];
    ELSE
      modes := ARRAY['rabitq-compact', 'rabitq-full', 'pq-compact', 'pq-full', 'plain'];
    END IF;

    FOREACH mode_name IN ARRAY modes LOOP
      RAISE NOTICE 'benchmark run %, mode %', run_no, mode_name;
      EXECUTE 'DROP INDEX IF EXISTS vex_bench_idx';

      expected_memory_mode := CASE WHEN mode_name LIKE '%compact' THEN 'compact' ELSE 'full' END;
      expected_quantizer := CASE
        WHEN mode_name LIKE 'pq-%' THEN 'pq'
        WHEN mode_name LIKE 'rabitq-%' THEN 'rabitq'
        ELSE 'none'
      END;

      -- For 32 dimensions, pq_m=16 is the high-recall profile used by the
      -- DuckDB 100k gate. pq_m=8 remains a valid higher-compression choice.
      create_sql :=
        'CREATE INDEX vex_bench_idx ON vex_bench_data USING vexdb_graph '
        || '(vec floatvector_l2_ops) WITH (m=16, ef_construction=160, parallel_workers=4, '
        || 'quantizer=' || quote_literal(expected_quantizer)
        || ', memory_mode=' || quote_literal(expected_memory_mode)
        || CASE WHEN expected_quantizer = 'pq' THEN ', pq_m=16' ELSE '' END
        || ')';

      started_at := clock_timestamp();
      EXECUTE create_sql;
      build_elapsed := extract(epoch FROM clock_timestamp() - started_at) * 1000.0;

      SELECT quantizer, memory_mode, memory_bytes,
             pq_codes_bytes + pq_codebook_bytes + rabitq_codes_bytes + rabitq_fixed_bytes
      INTO actual_quantizer, actual_memory_mode, mem_bytes, quant_bytes
      FROM vexdb_index_info()
      WHERE indexname = 'vex_bench_idx';

      active := actual_quantizer = expected_quantizer
                AND actual_memory_mode = expected_memory_mode
                AND (expected_quantizer = 'none' OR quant_bytes > 0);

      PERFORM set_config('enable_seqscan', 'off', true);
      index_used := false;
      FOR plan_line IN EXECUTE
        'EXPLAIN (COSTS OFF) SELECT id FROM vex_bench_data '
        || 'ORDER BY vec <-> __vex_bench_vec32(234) LIMIT 10'
      LOOP
        IF plan_line LIKE '%vex_bench_idx%' THEN
          index_used := true;
        END IF;
      END LOOP;

      EXECUTE 'DROP TABLE IF EXISTS vex_bench_ann';
      started_at := clock_timestamp();
      EXECUTE
        'CREATE TEMP TABLE vex_bench_ann ON COMMIT DROP AS '
        || 'SELECT q.qid, ann.id FROM vex_bench_queries q '
        || 'CROSS JOIN LATERAL ('
        || '  SELECT d.id FROM vex_bench_data d '
        || '  ORDER BY d.vec <-> q.vec LIMIT 10'
        || ') ann';
      query_elapsed := extract(epoch FROM clock_timestamp() - started_at) * 1000.0;

      EXECUTE
        'SELECT count(g.id)::double precision / 2000.0 '
        || 'FROM vex_bench_ann a LEFT JOIN vex_bench_gt g USING (qid, id)'
      INTO recall_value;

      INSERT INTO vex_bench_results
      VALUES (run_no, mode_name, build_elapsed, query_elapsed,
              200.0 / (query_elapsed / 1000.0), recall_value,
              index_used, active, mem_bytes, quant_bytes);

      EXECUTE 'DROP TABLE vex_bench_ann';
      EXECUTE 'DROP INDEX vex_bench_idx';
    END LOOP;
  END LOOP;
END
$$;

WITH medians AS (
  SELECT mode,
         percentile_cont(0.5) WITHIN GROUP (ORDER BY build_ms) AS build_ms,
         percentile_cont(0.5) WITHIN GROUP (ORDER BY qps) AS qps,
         percentile_cont(0.5) WITHIN GROUP (ORDER BY recall_at_10) AS recall_at_10,
         bool_and(uses_index) AS uses_index,
         bool_and(quantizer_active) AS quantizer_active,
         percentile_cont(0.5) WITHIN GROUP (ORDER BY memory_bytes) AS memory_bytes,
         percentile_cont(0.5) WITHIN GROUP (ORDER BY code_bytes) AS code_bytes
  FROM vex_bench_results
  GROUP BY mode
), baseline AS (
  SELECT build_ms, qps FROM medians WHERE mode = 'plain'
)
SELECT m.mode,
       round(m.build_ms::numeric, 2),
       round(m.qps::numeric, 2),
       round(m.recall_at_10::numeric, 4),
       m.uses_index,
       m.quantizer_active,
       round((m.memory_bytes / 1048576.0)::numeric, 2),
       round((m.code_bytes / 1048576.0)::numeric, 2),
       round((m.build_ms / baseline.build_ms)::numeric, 4),
       round((m.qps / baseline.qps)::numeric, 4)
FROM medians m, baseline
ORDER BY CASE m.mode
  WHEN 'plain' THEN 1 WHEN 'pq-full' THEN 2 WHEN 'pq-compact' THEN 3
  WHEN 'rabitq-full' THEN 4 ELSE 5 END;

DO $$
DECLARE failed text;
BEGIN
  SELECT string_agg(format('%s(recall=%s,index=%s,active=%s)',
                           mode, round(recall_at_10::numeric, 4), uses_index, quantizer_active), ', ')
  INTO failed
  FROM vex_bench_results
  WHERE recall_at_10 < 0.80 OR NOT uses_index OR NOT quantizer_active;
  IF failed IS NOT NULL THEN
    RAISE EXCEPTION 'PG quantizer benchmark gate failed: %', failed;
  END IF;
END
$$;

DO $$
DECLARE failed text;
BEGIN
  WITH medians AS (
    SELECT mode,
           percentile_cont(0.5) WITHIN GROUP (ORDER BY build_ms) AS build_ms,
           percentile_cont(0.5) WITHIN GROUP (ORDER BY qps) AS qps
    FROM vex_bench_results
    GROUP BY mode
  ), baseline AS (
    SELECT build_ms, qps FROM medians WHERE mode = 'plain'
  )
  SELECT string_agg(format('%s(build_ratio=%s,qps_ratio=%s)',
                           m.mode,
                           round((m.build_ms / b.build_ms)::numeric, 4),
                           round((m.qps / b.qps)::numeric, 4)), ', ')
  INTO failed
  FROM medians m CROSS JOIN baseline b
  WHERE m.mode <> 'plain'
    AND (m.build_ms / b.build_ms > 3.0 OR m.qps / b.qps < 0.60);
  IF failed IS NOT NULL THEN
    RAISE EXCEPTION 'PG quantizer performance gate failed: %', failed;
  END IF;
END
$$;

DROP TABLE vex_bench_results;
DROP TABLE vex_bench_gt;
DROP TABLE vex_bench_queries;
DROP TABLE vex_bench_data CASCADE;
DROP FUNCTION __vex_bench_vec32(int);
