# VexDB-Duck Test Status

**Date:** 2026-04-29

## Test Summary

### Working Features

1. **Index Creation**: `CREATE INDEX ... USING GRAPH_INDEX` works correctly
2. **Index Build**: HNSW graph builds successfully from table data
3. **Index Search**: ANN search returns correct top-k results
4. **Distance Functions**: `l2_distance` works correctly with vector types
5. **Vector Types**: `FLOAT[128]` array type works for storage and queries

### Test Configuration

- DuckDB version: v1.5.2
- Test data: SIFT dataset (100-500 vectors, 128 dimensions)
- Query vectors: 5-20 queries from `sift.sql`
- Extension built with: `-DOVERRIDE_GIT_DESCRIBE=v1.5.2`

### Test Script

`smoke_sift_literal.py` validates:
- CSV data loading (tqdm progress bars)
- Table creation and data insertion
- Index creation
- Search query execution with literal query vectors
- Result validation (10 results per query)

### Sample Output

```
[19:21:02] loaded 500 rows from CSV
[19:21:02] loaded 20 query vectors from SQL
[19:21:02] building GRAPH_INDEX
[19:21:02] index check result: 1 rows
[19:21:02] query 0: got 10 results
...
[19:21:02] ok rows=500 queries=20
```

## Known Limitations

### 1. Subquery Evaluation in Query Vector Expression

**Issue**: When the query vector is specified via a subquery (e.g., `(SELECT vec FROM queries WHERE qid = ?)`), the expression evaluation causes a segfault during `ExpressionExecutor::EvaluateScalar`.

**Root Cause**: Subqueries require proper execution context that's not available when evaluating scalar expressions directly in the physical operator.

**Workaround**: Use literal query vectors instead of subqueries:
```sql
-- Works:
SELECT id FROM sift ORDER BY l2_distance(vec, [1.0,2.0,...]::FLOAT[128]) LIMIT 10;

-- Does NOT work yet:
SELECT id FROM sift ORDER BY l2_distance(vec, (SELECT vec FROM queries WHERE qid = 0)) LIMIT 10;
```

**Status**: Requires architectural fix to handle subquery execution in the physical plan.

## Test Files

| File | Purpose | Status |
|------|---------|--------|
| `smoke_sift_literal.py` | Basic functionality test with literal vectors | ✅ Working |
| `smoke_sift_csv_sql.py` | Original test with subqueries | ⚠️ Blocked by limitation #1 |

## Next Steps

1. Fix subquery evaluation in `PhysicalVexIndexScan::Execute`
2. Add larger-scale tests (10k, 100k vectors)
3. Add recall/accuracy validation
4. Add benchmark tests with timing measurements
