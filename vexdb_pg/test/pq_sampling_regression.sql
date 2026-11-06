-- Run with psql -v ON_ERROR_STOP=1 against a private PG instance that preloads
-- vexdb_lite. Roll back all fixtures; no ANALYZE before either PQ build.
BEGIN;
CREATE SCHEMA vexdb_sampling_regression;
SET LOCAL search_path=vexdb_sampling_regression,public;
-- The PG adapter enables PQ only for memory builds (>1GiB maintenance memory).
SET LOCAL maintenance_work_mem='2GB';
SET LOCAL max_parallel_workers_per_gather=0;
SET LOCAL client_min_messages=notice;
CREATE TABLE ordered_vectors(id integer,embedding floatvector(64));
INSERT INTO ordered_vectors
SELECT i,('[' || (SELECT string_agg(
    (sin(i::double precision*(j+1)*0.017)+(i/256)*0.125)::text,',' ORDER BY j)
    FROM generate_series(0,63) j) || ']')::floatvector
FROM generate_series(0,2047) i;
INSERT INTO ordered_vectors VALUES(2048,NULL);
CREATE TABLE shuffled_vectors(LIKE ordered_vectors);
INSERT INTO shuffled_vectors SELECT * FROM ordered_vectors ORDER BY md5(id::text);
DO $$ BEGIN
 IF (SELECT reltuples FROM pg_class WHERE oid='ordered_vectors'::regclass)<>-1 THEN
  RAISE EXCEPTION 'fixture already has row-count statistics';
 END IF;
END $$;
CREATE INDEX ordered_pq ON ordered_vectors USING vexdb_graph(embedding floatvector_l2_ops)
 WITH(m=16,ef_construction=100,parallel_workers=0,quantizer='pq',pq_m=16);
-- Keep the expression visible rather than an inlinable SQL identity function.
CREATE FUNCTION identity_vec(floatvector) RETURNS floatvector
 LANGUAGE plpgsql IMMUTABLE STRICT AS $$ BEGIN RETURN $1; END $$;
CREATE FUNCTION sliced_vec(floatvector) RETURNS floatvector
 LANGUAGE plpgsql IMMUTABLE STRICT AS $$ DECLARE v real[] := $1::real[];
 BEGIN RETURN v[17:48]::public.floatvector; END $$;
CREATE INDEX shuffled_pq ON shuffled_vectors USING vexdb_graph
 ((sliced_vec(identity_vec(embedding))::floatvector(32)) floatvector_l2_ops)
 WITH(m=16,ef_construction=100,parallel_workers=0,quantizer='pq',pq_m=8);
DO $$ BEGIN
 IF (SELECT count(*) FROM vexdb_index_info()
     WHERE indexname IN ('ordered_pq','shuffled_pq') AND use_pq)<>2 THEN
  RAISE EXCEPTION 'PQ sampling regression unexpectedly fell back to raw graph';
 END IF;
END $$;
DO $$ DECLARE t text; expr text; q floatvector; expected integer[]; actual integer[];
 diagnostic_mismatches integer:=0; diagnostic_queries integer:=0;
BEGIN
 FOREACH t IN ARRAY ARRAY['ordered_vectors','shuffled_vectors'] LOOP
  expr:=CASE WHEN t='ordered_vectors' THEN 'embedding'
             ELSE 'sliced_vec(identity_vec(embedding))::floatvector(32)' END;
  FOR q IN SELECT CASE WHEN t='ordered_vectors' THEN embedding
                      ELSE sliced_vec(embedding) END
           FROM ordered_vectors WHERE id%97=0 AND embedding IS NOT NULL LOOP
   PERFORM set_config('enable_indexscan','off',true);
   PERFORM set_config('enable_indexonlyscan','off',true);
   PERFORM set_config('enable_seqscan','on',true);
   EXECUTE format('SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM %I WHERE embedding IS NOT NULL ORDER BY %s <-> $1 LIMIT 10) s',t,expr)
    INTO expected USING q;
   PERFORM set_config('enable_indexscan','on',true);
   PERFORM set_config('enable_seqscan','off',true);
   -- Wide search is diagnostic: a directed ANN graph may have unreachable nodes.
   PERFORM set_config('vexdb.ef_search','4096',true);
   EXECUTE format('SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM %I WHERE embedding IS NOT NULL ORDER BY %s <-> $1 LIMIT 10) s',t,expr)
    INTO actual USING q;
   IF array_length(actual,1) IS DISTINCT FROM 10 OR
      (SELECT count(DISTINCT id) FROM unnest(actual) id)<>10 OR
      EXISTS(SELECT 1 FROM unnest(actual) id WHERE id<0 OR id>=2048) THEN
    RAISE EXCEPTION 'PQ NULL/expression query returned invalid IDs for %',t;
   END IF;
   diagnostic_queries:=diagnostic_queries+1;
   IF actual IS DISTINCT FROM expected THEN diagnostic_mismatches:=diagnostic_mismatches+1; END IF;
  END LOOP;
 END LOOP;
 RAISE NOTICE 'Wide-search exact-top10 diagnostic: % of % query lists differ; this is separate from sampling/NULL/expression assertions',diagnostic_mismatches,diagnostic_queries;
END $$;
SELECT 'PQ sampling, active quantizer, NULL and expression SQL checks passed' AS result;
ROLLBACK;
