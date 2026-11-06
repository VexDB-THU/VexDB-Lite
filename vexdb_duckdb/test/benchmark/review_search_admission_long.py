"""Recompute product admission recall, latency, coverage and paired controls."""
import argparse
import ast
import csv
import hashlib
import json
import math
import re
import time
from collections import defaultdict
from fractions import Fraction
from pathlib import Path

import numpy as np


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def require(ok, reason):
    if not ok:
        raise RuntimeError(reason)


def percentile(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, math.ceil(q * len(values)) - 1)]


def review(run):
    out = run / 'independent-review'
    require(not out.exists(), 'review output already exists')
    all_points, all_controls, all_jobs = [], [], []
    for cohort, folder in [('catalog-matched', 'perf-v2-catalog-matched'),
                           ('catalog-mixed', 'perf-catalog-mixed'), ('text-public', 'perf-text-public')]:
        child = run / folder
        p = json.loads((child / 'latency-protocol.json').read_text())
        for name, digest in p['inputs'].items():
            require(sha(name) == digest, 'changed input: ' + name)
        state = json.loads((child / 'clean-state.json').read_text())
        require(state['status'] == 'complete' and not state['controller_pid'] and not state['active_pid'],
                'native queue not complete/stopped: ' + folder)
        require(len(state['jobs']) == len(p['clean']), 'missing native jobs')
        gold = np.load(p['oracle_path'])['none_top_ids']
        reference = {}
        groups = defaultdict(dict)
        total = 0
        for job in p['clean']:
            recorded = next(j for j in state['jobs'] if j['name'] == job['name'])
            require(recorded['status'] == 'complete' and recorded['exit_code'] == 0, 'failed native job')
            hashes = json.loads((child / (job['name'] + '-hashes.json')).read_text())
            for name, digest in hashes.items():
                require(sha(child / name) == digest, 'changed result: ' + name)
            directory = child / job['name']
            with (directory / 'points.csv').open() as f:
                native = {r['point']: r for r in csv.DictReader(f)}
            job_calls = warmups = 0
            limit_setting = None
            for line in Path(job['points']).read_text().splitlines():
                name, mode, ef, factor, threshold, filt, c, plan, requests, limit = line.split()
                ef, c, requests, limit = map(int, [ef, c, requests, limit])
                require(filt == 'none' and requests >= 512 and requests % 512 == 0, 'unregistered quality/load')
                require(limit_setting in (None, limit), 'mixed limits invalidate job peak check')
                limit_setting = limit
                result = native.pop(name)
                wall = int(result['wall_ns'])
                coverage = np.zeros((c, 512), dtype=np.int32)
                ordinals = [0] * c
                last_end = [0] * c
                latencies, cpus, hits = [], [], 0
                with (directory / (name + '.timing.csv')).open() as f:
                    for row in csv.DictReader(f):
                        worker, ordinal, q = (int(row[k]) for k in ('worker', 'ordinal', 'query'))
                        begin, end, cpu = (int(row[k]) for k in ('begin_ns', 'end_ns', 'thread_cpu_ns'))
                        require(0 <= worker < c and ordinal == ordinals[worker] and 0 <= q < 512,
                                'missing/duplicate/out-of-order work')
                        require(last_end[worker] <= begin <= end <= wall and cpu > 0, 'invalid request clock')
                        ordinals[worker] += 1
                        last_end[worker] = end
                        coverage[worker, q] += 1
                        ids = tuple(sorted(map(int, row['ids'].split())))
                        require(len(ids) == 10 and len(set(ids)) == 10 and all(0 <= i < p['corpus_rows'] for i in ids),
                                'illegal returned IDs')
                        key = (mode, ef, factor, q)
                        require(reference.setdefault(key, ids) == ids, 'returned IDs differ across versions/limits/loads')
                        hits += len(set(ids).intersection(map(int, gold[q])))
                        latencies.append((end - begin) / 1e6)
                        cpus.append(cpu / 1e6)
                calls = len(latencies)
                require(calls == c * requests and np.all(coverage == requests // 512) and all(i == requests for i in ordinals),
                        'not every worker completed the full query pool')
                qps = calls * 1e9 / wall
                require(abs(qps - float(result['qps'])) < 1e-6 and int(result['calls']) == calls, 'QPS mismatch')
                for q, field in [(.5, 'p50_ms'), (.95, 'p95_ms'), (.99, 'p99_ms')]:
                    require(abs(percentile(latencies, q) - float(result[field])) < 1e-6, 'latency mismatch')
                recall = Fraction(hits, calls * 10)
                require(recall >= Fraction(999, 1000), '99.9% recall failed')
                actual_plan = (directory / (name + '.plan.txt')).read_text()
                require(('VEXDB_INDEX_SCAN' in actual_plan) == bool(int(plan)), 'unexpected plan')
                row = {'cohort': cohort, 'job': job['name'], 'version': job['version'], 'point': name,
                       'round': int(job['name'][-1]), 'mode': mode, 'ef': ef, 'connections': c,
                       'calls': calls, 'hits': hits, 'denominator': calls * 10, 'recall': float(recall),
                       'qps': qps, 'p50_ms': percentile(latencies, .5), 'p95_ms': percentile(latencies, .95),
                       'p99_ms': percentile(latencies, .99), 'cpu_p95_ms': percentile(cpus, .95),
                       'mean_active_clients': float(result['mean_active_clients']),
                       'full_overlap_ns': int(result['overlap_ns'])}
                require(row['mean_active_clients'] >= .8 * c and (c == 1 or row['full_overlap_ns'] > 0),
                        'insufficient concurrent occupancy')
                all_points.append(row)
                groups[(name, job['version'])][row['round']] = row
                job_calls += calls
                warmups += c
            require(not native, 'unregistered measured points')
            stats = None
            if limit_setting >= -1:
                source = (directory / 'admission-state.txt').read_text()
                stats = ast.literal_eval(next(s for s in source.splitlines() if s.startswith("{'limit':")))
                require(stats['active'] == stats['waiting'] == stats['cancelled'] == stats['timed_out'] == 0,
                        'dropped, expired, cancelled or unfinished measured work')
                require(stats['admitted'] == job_calls + warmups, 'admission count differs from real SQL work')
                if limit_setting != 0:
                    require(0 < stats['peak_active'] <= stats['limit'], 'product concurrency limit exceeded')
                    if limit_setting > 0:
                        require(stats['limit'] == limit_setting, 'explicit common limit differs')
                else:
                    require(stats['limit'] == 0, 'disabled control not disabled')
            all_jobs.append({'cohort': cohort, 'job': job['name'], 'calls': job_calls, 'admission_state': stats})
            total += job_calls
        require(total == p['expected_clean_calls'], 'cohort request total differs')
        for name in {key[0] for key in groups}:
            for candidate, baseline in [('off', 'original'), ('cpu2x', 'off')]:
                ratios, p95_ratios = [], []
                for rnd in (1, 2):
                    a, b = groups[(name, candidate)][rnd], groups[(name, baseline)][rnd]
                    ratios.append(a['qps'] / b['qps'])
                    p95_ratios.append(a['p95_ms'] / b['p95_ms'])
                mean = math.sqrt(ratios[0] * ratios[1])
                all_controls.append({'cohort': cohort, 'point': name, 'candidate': candidate, 'baseline': baseline,
                                     'qps_r1_ratio': ratios[0], 'qps_r2_ratio': ratios[1], 'qps_geomean_ratio': mean,
                                     'p95_r1_ratio': p95_ratios[0], 'p95_r2_ratio': p95_ratios[1],
                                     'non_regression_within_registered_3pct': mean >= .97})
    out.mkdir()
    for name, rows in [('points.csv', all_points), ('controls.csv', all_controls)]:
        with (out / name).open('x') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
    result = {'reviewed_epoch': time.time(), 'reviewer_sha256': sha(__file__),
              'calls': sum(p['calls'] for p in all_points), 'points': len(all_points),
              'all_inputs_outputs_clock_coverage_ID_equivalence_recall_verified': True,
              'all_paired_non_regression_gates_passed': all(c['non_regression_within_registered_3pct'] for c in all_controls),
              'failed_controls': [c for c in all_controls if not c['non_regression_within_registered_3pct']],
              'jobs': all_jobs, 'scope': 'Two longer, preregistered rounds on a shared desktop; all waiting included. Exposed queries are equivalence regression, not fresh business acceptance. Individual slow rounds remain in controls.csv; do not equate a 3% statistical tolerance to zero cost.'}
    (out / 'review.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != 'jobs'}, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', required=True, type=Path)
    review(p.parse_args().run.resolve())
