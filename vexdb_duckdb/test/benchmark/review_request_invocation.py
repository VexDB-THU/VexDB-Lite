"""Independently review native request CSVs without loading the benchmark.

The case manifest supplies paths, frozen hashes and the expected workload.
Failed/missing cases stay failed; they never acquire inferred performance.
Predicate names refer only to this benchmark's documented proxy metadata.
"""
import argparse
import collections
import csv
import functools
import hashlib
import json
import math
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        while block := f.read(2**20):
            h.update(block)
    return h.hexdigest()


@functools.lru_cache(maxsize=8)
def load_truth(path):
    return {k: set(v['ids']) for k, v in json.loads(Path(path).read_text()).items()}


@functools.lru_cache(maxsize=8)
def load_metadata(path):
    with Path(path).open() as f:
        return {int(row['id']): row for row in csv.DictReader(f)}


def eligible(row, profile, qi):
    if profile == 'none':
        return True
    if profile == 'tenant':
        return row['tenant_id'] == str(qi % 100) and row['is_public'] == '1'
    if profile == 'broad':
        return row['is_active'] == '1'
    if profile == 'catalog':
        return row['in_stock'] == '1' and 1000 + qi % 5 * 100 <= int(row['price_cents'] or -1) <= 9000
    raise ValueError('Unknown proxy predicate: ' + profile)


def review_case(case):
    result = {'name': case['name'], 'declared_status': case['status']}
    if case['status'] != 'complete':
        return dict(result, passed=False, status=case['status'], reason=case.get('reason', 'No complete native result'))
    folder = Path(case['path'])
    kind = case.get('kind', 'measure')
    data_file = folder / ('timing.csv' if kind == 'measure' else 'results.csv')
    for name, expected in case.get('expected_sha256', {}).items():
        assert digest(folder / name) == expected, 'Frozen hash differs: ' + name
    truth = load_truth(case['truth'])
    meta = load_metadata(case['metadata'])
    nq = case.get('query_count', 256)
    profile = case['profile']
    hits = collections.Counter()
    seen = set()
    clients = collections.Counter()
    durations = []
    waits = []
    invalid = 0
    largest_end = 0
    count = 0
    with data_file.open() as f:
        for row in csv.DictReader(f):
            qi = int(row['query'])
            assert 0 <= qi < nq
            ordinal = int(row['ordinal']) if kind == 'measure' else qi
            assert ordinal not in seen and qi == ordinal % nq
            seen.add(ordinal)
            ids = [int(x) for x in row['ids'].split()]
            invalid += int(len(ids) != 10 or len(set(ids)) != 10)
            hits[ordinal // nq] += len(set(ids).intersection(truth[profile + ':' + str(qi)]))
            invalid += sum(int(i not in meta or not eligible(meta[i], profile, qi)) for i in ids)
            invalid += int(int(row['payload_bytes']) != sum(int(meta[i]['body_bytes']) for i in ids if i in meta))
            if kind == 'measure':
                client = int(row['client'])
                assert 0 <= client < case['clients']
                clients[client] += 1
                begin, end = int(row['begin_ns']), int(row['end_ns'])
                assert 0 <= begin <= end
                durations.append(end - begin)
                largest_end = max(largest_end, end)
                if 'connection_wait_ns' in row:
                    wait = int(row['connection_wait_ns'])
                    assert 0 <= wait <= end - begin
                    assert 0 <= int(row['physical_session']) < min(case['pool_capacity'], case['clients'])
                    waits.append(wait)
            count += 1
    assert count and count % nq == 0 and seen == set(range(count))
    result.update(calls=count, rounds=count // nq, invalid=invalid, worst_round_hits=min(hits.values()),
                  records_sha256=digest(data_file))
    if kind == 'quality':
        assert count == nq
    else:
        assert len(clients) == case['clients']
        with (folder / 'point.csv').open() as f:
            rows = list(csv.DictReader(f))
        assert len(rows) == 1
        point = {k: float(v) for k, v in rows[0].items()}
        assert point['calls'] == count and point['participating_clients'] == len(clients)
        assert point['wall_ns'] >= case['seconds'] * 10**9 and largest_end <= point['wall_ns']
        qps = count * 10**9 / point['wall_ns']
        assert math.isclose(qps, point['qps'], rel_tol=1e-10)
        durations.sort()
        for key, percentile in [('p50_ms', .50), ('p95_ms', .95), ('p99_ms', .99)]:
            computed = durations[math.ceil(count * percentile) - 1] / 10**6
            assert math.isclose(computed, point[key], rel_tol=1e-9, abs_tol=1e-9), key
        result.update(point=point, point_sha256=digest(folder / 'point.csv'),
                      per_client_calls=dict(sorted(clients.items())))
        if waits:
            with (folder / 'connection-pool.csv').open() as f:
                rows = list(csv.DictReader(f))
            assert len(rows) == 1
            stats = {k: int(v) for k, v in rows[0].items()}
            physical = min(case['pool_capacity'], case['clients'])
            assert stats['configured_capacity'] == case['pool_capacity']
            assert stats['physical_connections'] == physical
            assert stats['active'] == stats['waiting'] == 0
            assert stats['peak_active'] <= physical and stats['timed_acquisitions'] == count
            waits.sort()
            result.update(pool=stats, wait_p95_ms=waits[math.ceil(count * .95) - 1] / 10**6)
    result.update(passed=invalid == 0 and min(hits.values()) >= case.get('minimum_hits', 2558), status='audited')
    return result


def review(manifest):
    results = []
    names = set()
    for case in manifest['cases']:
        assert case['name'] not in names
        names.add(case['name'])
        try:
            results.append(review_case(case))
        except (AssertionError, KeyError, ValueError, OSError) as error:
            results.append({'name': case['name'], 'passed': False, 'status': 'audit_error', 'error': str(error)})
    indexed = {r['name']: r for r in results}
    comparisons = []
    for group in manifest.get('comparisons', []):
        cases = [indexed[name] for name in group['cases']]
        complete = len(cases) == 2 and all(c['passed'] and 'point' in c for c in cases)
        if not complete:
            comparisons.append(dict(group, passed=False, reason='Incomplete or failed native pair'))
            continue
        qps = sorted(c['point']['qps'] for c in cases)
        p95 = sorted(c['point']['p95_ms'] for c in cases)
        comparisons.append(dict(group, qps_range=qps, p95_ms_range=p95,
            qps_ratio=qps[1] / qps[0], p95_ratio=p95[1] / p95[0],
            passed=qps[1] / qps[0] <= 1.15 and p95[1] / p95[0] <= 1.20))
    return {'status': 'audited' if all(r['passed'] for r in results) and all(c['passed'] for c in comparisons)
            else 'audited_with_failures', 'cases': results, 'comparisons': comparisons,
            'actual_return_lists': sum(r.get('calls', 0) for r in results),
            'failed_cases': sum(not r['passed'] for r in results),
            'failed_comparisons': sum(not c['passed'] for c in comparisons)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cases', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = review(json.loads(args.cases.read_text()))
    result['case_manifest_sha256'] = digest(args.cases)
    result['reviewer_sha256'] = digest(__file__)
    with args.out.open('x') as f:
        json.dump(result, f, indent=2)
    print(json.dumps({k: v for k, v in result.items() if k not in ['cases', 'comparisons']}))
