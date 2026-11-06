"""Review actual SQL returns and intended-arrival clocks without executing SQL."""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(2**20), b''):
            h.update(block)
    return h.hexdigest()


def eligible(row, profile, qi):
    if profile == 'none':
        return True
    if profile == 'broad':
        return row['is_active'] == '1'
    if profile == 'tenant':
        return row['tenant_id'] == str(qi % 100) and row['is_public'] == '1'
    if profile == 'catalog':
        return row['in_stock'] == '1' and 1000 + qi % 5 * 100 <= int(row['price_cents']) <= 9000
    raise ValueError('Unregistered predicate')


def review(folder, cfg, protocol, truth, metadata):
    profile, cohort = cfg['profile'], cfg['cohort']
    normal_gate = protocol['normal'][cohort]
    rate = normal_gate['rate']
    assert float(cfg['rate']) == rate
    nq, clients = int(cfg['query_count']), int(cfg['clients'])
    assert nq == protocol['query_count'] and clients == protocol['clients']
    assert cfg['request_mode'] == 'parameter' and cfg['pool_connections'] == '0'
    assert cfg['search_concurrency'] == '0' and cfg['native_topn_fetch'] == 'true'
    assert int(cfg['normal_windows']) == protocol['normal_windows']
    assert int(cfg['seconds']) == protocol['seconds_minimum']
    windows = list(csv.DictReader((folder / 'windows.csv').open()))
    results = []
    for window in windows:
        path = folder / window['name'] / 'requests.csv'
        seen, per_client = set(), collections.defaultdict(list)
        hits = collections.Counter()
        elapsed, sql, waits = [], [], []
        invalid = errors = completed = 0
        largest_end = 0
        wall = int(window['wall_ns'])
        normal = window['kind'] == 'normal'
        with path.open() as f:
            for r in csv.DictReader(f):
                ordinal, qi, client = (int(r[k]) for k in ('ordinal', 'query', 'client'))
                assert ordinal not in seen and qi == ordinal % nq and 0 <= client < clients
                seen.add(ordinal)
                scheduled, begin, end = (int(r[k]) for k in ('scheduled_ns', 'begin_ns', 'end_ns'))
                assert 0 <= scheduled <= begin <= end <= wall
                if normal:
                    assert scheduled == int(float(ordinal) * 1e9 / rate)
                per_client[client].append((begin, end))
                largest_end = max(largest_end, end)
                ids = [int(x) for x in r['ids'].split()]
                good = r['status'] == 'ok'
                completed += good
                errors += not good
                invalid += len(ids) != 10 or len(set(ids)) != 10
                invalid += sum(i not in metadata or not eligible(metadata[i], profile, qi) for i in ids)
                invalid += int(r['payload_bytes']) != sum(int(metadata[i]['body_bytes']) for i in ids if i in metadata)
                hits[ordinal // nq] += len(set(ids) & set(truth[profile + ':' + str(qi)]['ids']))
                elapsed.append(end - scheduled)
                sql.append(end - begin)
                waits.append(begin - scheduled)
        n = len(seen)
        assert n and seen == set(range(n)) and n == int(window['calls'])
        assert len(per_client) == int(window['participating_clients'])
        for intervals in per_client.values():
            intervals.sort()
            assert all(a[1] <= b[0] for a, b in zip(intervals, intervals[1:]))
        events = [(b, 1) for rows in per_client.values() for b, e in rows]
        events += [(e, -1) for rows in per_client.values() for b, e in rows]
        active = peak = 0
        for _, delta in sorted(events):
            active += delta
            peak = max(peak, active)
        assert active == 0 and peak <= clients
        complete_rounds = n // nq
        quality = (n % nq == 0 and invalid == 0 and errors == 0 and
                   len(hits) == complete_rounds and min(hits.values()) >= protocol['quality_minimum_hits_per256'])
        values = {}
        for key, data in [('latency', elapsed), ('sql', sql), ('wait', waits)]:
            data.sort()
            values[key + '_ms'] = {str(p): data[math.ceil(n * p) - 1] / 1e6 for p in (.5, .95, .99)}
        expected = int(math.ceil(rate * protocol['seconds_minimum'] / nq)) * nq
        complete = not int(window['interrupted']) and not errors and n % nq == 0
        assert errors == int(window['errors'])
        qps = completed * 1e9 / wall
        sla = None
        if normal:
            assert int(window['offering_ns']) == int(expected * 1e9 / rate)
            complete = complete and n == expected and wall >= int(window['offering_ns'])
            sla = (complete and quality and qps >= .99 * rate and
                   values['latency_ms']['0.95'] <= normal_gate['p95_ms'] and
                   values['latency_ms']['0.99'] <= normal_gate['p99_ms'])
        else:
            assert wall >= protocol['stress_seconds_minimum'] * 10**9
            complete = complete and len(per_client) == clients
        results.append(dict(name=window['name'], kind=window['kind'], calls=n, completed=completed,
                            errors=errors, invalid=invalid, wall_ns=wall, largest_end_ns=largest_end,
                            complete=complete, quality_passed=quality, worst_round_hits=min(hits.values()),
                            minimum_recall=min(hits.values()) / (10 * nq), complete_rounds=complete_rounds,
                            achieved_qps=qps, peak_sql_intervals=peak, normal_slo_passed=sla,
                            records_sha256=digest(path), **values))
    normals = [r for r in results if r['kind'] == 'normal']
    stresses = [r for r in results if r['kind'] == 'stress']
    expected_stresses = int(cfg['stress_windows'])
    assert expected_stresses in (0, protocol['stress_windows'])
    stress_scope = ((cohort, cfg['method'], profile) in map(tuple, protocol['stress_cases']))
    assert expected_stresses == (protocol['stress_windows'] if stress_scope else 0)
    normal_passed = len(normals) == protocol['normal_windows'] and all(r['normal_slo_passed'] for r in normals)
    stress_passed = len(stresses) == expected_stresses and all(r['complete'] and r['quality_passed'] for r in stresses)
    ratios = None
    if stresses:
        qps = [r['achieved_qps'] for r in stresses]
        p95 = [r['latency_ms']['0.95'] for r in stresses]
        ratios = dict(qps=max(qps) / min(qps), p95=max(p95) / min(p95))
    return dict(normal_slo_passed=normal_passed, stress_capacity_quality_passed=stress_passed,
                recovery_passed=(normals[-1]['normal_slo_passed'] if normals and expected_stresses else None),
                stress_ratios=ratios, windows=results, protocol_sha256=None,
                scope='Frozen existing query pool, actual SQL plus full body; not independent new-query or production traffic proof.')


def main():
    ap = argparse.ArgumentParser()
    for name in ('case', 'config', 'protocol', 'truth', 'metadata', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    a = ap.parse_args()
    assert not a.out.exists()
    cfg = dict(line.split('=', 1) for line in a.config.read_text().splitlines())
    p = json.loads(a.protocol.read_text())
    truth = json.loads(a.truth.read_text())
    metadata = {int(r['id']): r for r in csv.DictReader(a.metadata.open())}
    result = review(a.case, cfg, p, truth, metadata)
    result.update(protocol_sha256=digest(a.protocol), reviewer_sha256=digest(__file__), config_sha256=digest(a.config))
    a.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('normal_slo_passed', 'stress_capacity_quality_passed', 'recovery_passed')}))


if __name__ == '__main__':
    main()
