#!/usr/bin/env python3
"""Exclusive wall-interval accounting of existing LIVE traces, offline only.

This file never compiles or executes game/native/GPU code. Intervals describe
instrumented scopes, not CPU residency, GPU busy time, removable work or frames.
"""
from __future__ import annotations
import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

PARSER_PATH = Path(__file__).with_name('analyze-xemu-live-trace.py')
spec = importlib.util.spec_from_file_location('existing_causal_parser', PARSER_PATH)
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def partition_intervals(begin, end, intervals):
    """Sweep half-open scopes; highest priority owns time, never sum nesting.

    Equal priorities with different labels are rejected rather than arbitrarily
    assigning ambiguous time. Same-label overlapping scopes form a union.
    """
    if begin >= end:
        raise ValueError('Invalid accounting window')
    changes = defaultdict(list)
    changes[begin]; changes[end]
    for index, (a, b, label, priority) in enumerate(intervals):
        if a > b:
            raise ValueError('Backwards scope')
        a, b = max(begin, a), min(end, b)
        if a >= b:
            continue
        changes[a].append((1, index, label, priority))
        changes[b].append((-1, index, label, priority))
    active = {}
    totals = Counter()
    segments = []
    previous = begin
    for now in sorted(changes):
        if now > previous:
            label = 'unobserved'
            if active:
                highest = max(priority for label, priority in active.values())
                labels = {label for label, priority in active.values() if priority == highest}
                if len(labels) != 1:
                    raise ValueError('Ambiguous equal-priority scopes')
                label = next(iter(labels))
            totals[label] += now - previous
            if segments and segments[-1][2] == label:
                segments[-1][1] = now
            else:
                segments.append([previous, now, label])
        for direction, index, label, priority in changes[now]:
            if direction == -1:
                active.pop(index)
            else:
                active[index] = (label, priority)
        previous = now
    return dict(totals_ns=dict(totals), partition_sum_ns=sum(totals.values()),
                window_ns=end - begin, segments=segments)


class Coverage:
    """Integral of a union, for many queue/service intersection queries."""
    def __init__(self, intervals):
        self.intervals = []
        for a, b in sorted(intervals):
            if a > b:
                raise ValueError('Backwards coverage interval')
            if a == b:
                continue
            if self.intervals and a <= self.intervals[-1][1]:
                self.intervals[-1][1] = max(self.intervals[-1][1], b)
            else:
                self.intervals.append([a, b])
        self.starts = [a for a, b in self.intervals]
        self.prefix = [0]
        for a, b in self.intervals:
            self.prefix.append(self.prefix[-1] + b - a)

    def before(self, time):
        index = bisect_right(self.starts, time) - 1
        if index < 0:
            return 0
        a, b = self.intervals[index]
        return self.prefix[index] + min(time, b) - a

    def between(self, a, b):
        return self.before(b) - self.before(a)


def analyze_rows(rows, begin, end):
    # Reuse the strict identity/order/payload validation of the existing parser.
    validation = trace.worker_summary(rows)
    barriers = trace.barrier_summary(rows)
    grouped = defaultdict(dict)
    for row in rows:
        if 1 <= row['event'] <= 7 and row['id']:
            grouped[row['id']][row['event']] = row
    services = defaultdict(list)
    worker_scopes = defaultdict(list)
    caller_scopes = defaultdict(list)
    caller_full = defaultdict(list)
    requests = []
    for request_id, phases in sorted(grouped.items()):
        caller = next((phases[e]['tid'] for e in (1, 4) if e in phases), None)
        worker = next((phases[e]['tid'] for e in (2, 3, 5, 6, 7) if e in phases), None)
        req = dict(id=request_id, raw_tag=hex(next(iter(phases.values()))['a']),
                   caller_tid=caller, worker_tid=worker,
                   censored=not all(e in phases for e in (1, 2, 3, 4)),
                   phases={trace.EVENTS[e]: r['ns'] for e, r in phases.items()})
        for name, (a, b) in {'queue': (1, 2), 'service': (2, 3),
                             'wake_return': (3, 4), 'caller_total': (1, 4),
                             'gate_wait': (5, 6), 'gate_held': (6, 7)}.items():
            req[name + '_ns'] = phases[b]['ns'] - phases[a]['ns'] if a in phases and b in phases else None
        if req['service_ns'] is not None:
            a, b = phases[2]['ns'], phases[3]['ns']
            services[worker].append((a, b))
            worker_scopes[worker].append((a, b, 'service_other_elapsed', 1))
        if req['gate_wait_ns'] is not None:
            worker_scopes[worker].append((phases[5]['ns'], phases[6]['ns'], 'gate_wait_elapsed', 2))
        if caller is not None:
            for label, a, b, priority in [('queue', 1, 2, 1), ('own_service', 2, 3, 3),
                                          ('wake_return', 3, 4, 2)]:
                if a in phases and b in phases:
                    caller_scopes[caller].append((phases[a]['ns'], phases[b]['ns'], label, priority))
            if req['caller_total_ns'] is not None:
                caller_full[caller].append((phases[1]['ns'], phases[4]['ns']))
        requests.append(req)
    coverage = {}
    for tid, intervals in services.items():
        ordered = sorted(intervals)
        if any(a[1] > b[0] for a, b in zip(ordered, ordered[1:])):
            raise ValueError('Overlapping service scopes on one serialized worker')
        coverage[tid] = Coverage(intervals)
    for req in requests:
        value = None
        if req['queue_ns'] is not None and req['worker_tid'] in coverage:
            value = coverage[req['worker_tid']].between(
                req['phases']['worker_enqueue'], req['phases']['worker_start'])
        req['queue_during_observed_worker_service_ns'] = value
        req['queue_outside_observed_worker_service_ns'] = req['queue_ns'] - value if value is not None else None
    complete_barriers = [c for c in barriers['chains'] if c['inclusive_duration_ns'] is not None]
    for chain in complete_barriers:
        worker_scopes[chain['tid']].append((chain['begin']['ns'], chain['end']['ns'],
                                          'barrier:' + chain['cause'], 10 + chain['depth']))
    workers = {}
    for tid, scopes in worker_scopes.items():
        roots = [(c['begin']['ns'], c['end']['ns'], c['cause'], 65536 - c['depth'])
                 for c in complete_barriers if c['tid'] == tid]
        root_partition = partition_intervals(begin, end, roots)
        roots_totals = {k: v for k, v in root_partition['totals_ns'].items() if k != 'unobserved'}
        partition = partition_intervals(begin, end, scopes)
        service_coverage = coverage.get(tid, Coverage([]))
        barrier_within_service = sum(service_coverage.between(a, b)
                                     for a, b, label in root_partition['segments'] if label != 'unobserved')
        workers[str(tid)] = dict(partition=partition,
                                observed_service_union_ns=service_coverage.between(begin, end),
                                barrier_root_totals_ns=roots_totals,
                                barrier_union_ns=sum(roots_totals.values()),
                                barrier_within_observed_service_ns=barrier_within_service,
                                barrier_outside_observed_service_ns=sum(roots_totals.values()) - barrier_within_service)
    callers = {}
    for tid, scopes in caller_scopes.items():
        full = caller_full[tid]
        union = Coverage(full).between(begin, end)
        total = sum(min(end, b) - max(begin, a) for a, b in full if max(begin, a) < min(end, b))
        callers[str(tid)] = dict(partition=partition_intervals(begin, end, scopes),
                                caller_union_ns=union, caller_sum_ns=total, overlap_ns=total - union,
                                request_count=sum(r['caller_tid'] == tid for r in requests))
    # Reservation ordering on each serialized worker supplies scope ancestry.
    # Global timestamps may be inverted between producers; proximity is not cause.
    chains_by_id = {c['id']: c for c in barriers['chains']}
    active = defaultdict(dict)
    batches = defaultdict(dict)
    ledger = []
    for row in sorted(rows, key=lambda r: r['seq']):
        tid = row['tid']
        if row['event'] == 43:
            active[tid][row['id']] = chains_by_id[row['id']]
        elif row['event'] == 44:
            active[tid].pop(row['id'], None)
        elif row['event'] in (40, 41, 42):
            if not row['id'] or row['b']:
                raise ValueError('Invalid batch serial/reserved field')
            if row['event'] in batches[row['id']]:
                raise ValueError('Duplicate batch phase')
            batches[row['id']][row['event']] = row
            if row['event'] == 41:
                scopes = sorted(active[tid].values(), key=lambda c: c['depth'])
                root = scopes[0] if scopes else None
                payload = root['begin'] if root else None
                ledger.append(dict(record_serial=row['id'], tid=tid, seq=row['seq'], ns=row['ns'],
                    classification='consumer_or_resource_scope' if root else 'outside_instrumented_barrier',
                    outer_scope=root['cause'] if root else None,
                    scope_chain=[c['cause'] for c in scopes],
                    outer_scope_id=root['id'] if root else None,
                    pending_releases_at_scope_begin=payload['pending_releases'] if payload else None,
                    submitted_at_scope_begin=payload['submitted'] if payload else None,
                    completed_at_scope_begin=payload['completed'] if payload else None,
                    recording_at_scope_begin=bool(payload['flags'] & 2) if payload else None,
                    outer_parent_censored=bool(root and root['depth']),
                    finish_reason=None, removable_proven=False))
    lifecycle = []
    for serial, phases in sorted(batches.items()):
        seen = [phases[e] for e in (40, 41, 42) if e in phases]
        if len({r['tid'] for r in seen}) != 1 or any(a['ns'] > b['ns'] for a, b in zip(seen, seen[1:])):
            raise ValueError('Invalid batch causal chain')
        lifecycle.append(dict(record_serial=serial, tid=seen[0]['tid'],
                              begin_ns=phases.get(40, {}).get('ns'),
                              submitted_ns=phases.get(41, {}).get('ns'),
                              retired_ns=phases.get(42, {}).get('ns')))
    # GPU submission lifetime is not fence busy time. Report record/pending overlap.
    lifetime_by_tid = defaultdict(list)
    for chain in lifecycle:
        if chain['submitted_ns'] is not None and chain['retired_ns'] is not None:
            lifetime_by_tid[chain['tid']].append((chain['submitted_ns'], chain['retired_ns']))
    pending_coverage = {tid: Coverage(values) for tid, values in lifetime_by_tid.items()}
    for worker in workers.values():
        worker['recording_with_prior_submission_pending_union_ns'] = 0
    for tid in lifetime_by_tid:
        overlaps = []
        for chain in lifecycle:
            if chain['tid'] != tid or chain['begin_ns'] is None or chain['submitted_ns'] is None:
                continue
            for a, b in pending_coverage[tid].intervals:
                lo, hi = max(a, chain['begin_ns']), min(b, chain['submitted_ns'])
                if lo < hi:
                    overlaps.append((lo, hi))
        if str(tid) in workers:
            workers[str(tid)]['recording_with_prior_submission_pending_union_ns'] = Coverage(overlaps).between(begin, end)
    return dict(schema='b3-live-exclusive-wall-v1', window=dict(begin_ns=begin, end_ns=end, ns=end - begin),
                workers=workers, callers=callers, requests=requests,
                worker_chain_validation=dict(chains=validation['chains'], complete=validation['complete'],
                                             censored_chains=len(validation['censored_chains'])),
                barrier_validation=dict(complete=barriers['complete_chains'], censored=barriers['censored_chains']),
                main_closures=dict(count=len(ledger), by_outer_scope=dict(Counter(c['outer_scope'] or 'UNATTRIBUTED' for c in ledger)),
                                   ledger=ledger, lifecycle=lifecycle, removable_proven_count=0),
                limits=[
                    'Exclusive elapsed scopes include CPU execution, preemption, locks, GPU waits and observer overhead; no sched_switch or fence-wait begin/end exists in this trace.',
                    'Queue outside observed service is not demonstrated idle or removable time; enqueue is before pipe write and start is after pipe read.',
                    'Caller phase partition prioritizes own service then wake_return then queue when signal reentry overlaps; union/sum/overlap remain explicit.',
                    'Unobserved time includes missing/censored scopes and ordinary idle; it is not assigned CPU/GPU cost.',
                    'Pending submission lifetime includes polling/retirement delay; recording overlap is not GPU execution overlap.',
                    'Scope association identifies observed dependency/resource boundaries; zero pending RELEASE does not exclude WAIT-prefix or CPU/resource dependence.',
                    'MAIN events lack FinishReason, GET/PUT, actual consumer status and quantum-service origin; outside-barrier closures are unattributed, not proven avoidable.',
                    'No original-frame/flip attribution; per-request dependency chains are not a per-frame critical path or forecast of FPS savings.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--game-log', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        data = args.trace.read_bytes()
        header, rows, inversions = trace.decode_records(data)
        result = analyze_rows(rows, header['begin_ns'], header['end_ns'])
        log = args.game_log.read_text(errors='replace') if args.game_log else None
        result['completeness'] = trace.read_stats(log, len(rows))
        result['header'] = header
        result['records'] = len(rows)
        result['reservation_timestamp_inversions'] = inversions
        result['observed_event_envelope_ns'] = [min(r['ns'] for r in rows), max(r['ns'] for r in rows)] if rows else None
        result['sources'] = dict(trace=str(args.trace.resolve()), trace_sha256=hashlib.sha256(data).hexdigest(),
                                 parser_sha256=hashlib.sha256(PARSER_PATH.read_bytes()).hexdigest(),
                                 analyzer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        if args.game_log:
            result['sources']['game_log'] = str(args.game_log.resolve())
            result['sources']['game_log_sha256'] = hashlib.sha256(args.game_log.read_bytes()).hexdigest()
        with args.output.open('x') as stream:
            json.dump(result, stream, indent=2, sort_keys=True)
            stream.write('\n')
    except (OSError, ValueError) as error:
        print(f'exclusive trace analysis failed: {error}', file=sys.stderr)
        return 1
    print(f'Analyzed {len(rows)} recorded events; report: {args.output}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
