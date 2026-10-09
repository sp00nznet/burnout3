#!/usr/bin/env python3
"""Strict offline analysis of passive events 50..57; never run the game.

Times are wall intervals observed on the sole device worker, not CPU residency
or savings. Source/request counts do not establish unique original frames.
"""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import sys

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location('trace_base', ROOT / 'scripts/analyze-xemu-live-trace.py')
BASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASE)
BASE.EVENTS.update({50: 'flip_write', 51: 'flip_stall_begin', 52: 'flip_read',
                   53: 'flip_stall_end', 54: 'present_source', 55: 'present_content',
                   56: 'present_result', 57: 'present_cadence'})


def distribution(values):
    return dict(count=len(values), sum_ns=sum(values),
                median_ns=statistics.median(values) if values else None,
                min_ns=min(values) if values else None,
                max_ns=max(values) if values else None)


def union(intervals):
    total = 0
    end = None
    for a, b in sorted(intervals):
        total += max(0, b - max(a, end if end is not None else a))
        end = max(end if end is not None else b, b)
    return total


def analyze(records, window_begin, window_end):
    rows = sorted((r for r in records if 50 <= r[3] <= 57), key=lambda r: r[0])
    tids = {r[2] for r in rows}
    if len(tids) > 1:
        raise ValueError('Probe events must belong to one exclusive device-worker TID')
    if any(b[1] < a[1] for a, b in zip(rows, rows[1:])):
        raise ValueError('Nonmonotonic timestamps on exclusive probe worker')
    if any(b[4] < a[4] for a, b in zip(rows, rows[1:])):
        raise ValueError('Nonmonotonic observed flip serial on exclusive worker')
    writes = []
    pairs = []
    active_stall = None
    reads = []
    last_read = None
    unmatched_ends = 0
    source_counts = Counter()
    completed_stalls = set()
    source_before_retire = 0
    attempts = []
    censored = []
    pending = []
    leading_tuple_allowed = True
    expected = (54, 55, 57, 56)
    for row in rows:
        seq, ns, tid, event, flip, a, b = row
        if event == 50:
            if writes and flip != writes[-1][1] + 1:
                raise ValueError('Observed WRITE serial is not consecutive')
            if active_stall is not None:
                raise ValueError('WRITE advanced while observed flip stall is still active')
            writes.append((ns, flip))
        elif event == 51:
            if active_stall is not None:
                raise ValueError('Repeated/overlapping flip-stall BEGIN')
            active_stall = row
            reads = []
        elif event == 52:
            last_read = row
            if active_stall is not None:
                if flip != active_stall[4]:
                    raise ValueError('READ changed serial during stall')
                reads.append(row)
        elif event == 53:
            if active_stall is None:
                # Only the leading edge of the captured prefix can lack BEGIN.
                if pairs or writes:
                    raise ValueError('Interior flip-stall END without BEGIN')
                unmatched_ends += 1
            else:
                if flip != active_stall[4]:
                    raise ValueError('Flip-stall END has a different serial')
                begin = active_stall[1]
                pairs.append(dict(flip=flip, tid=tid, begin_ns=begin, end_ns=ns,
                                  duration_ns=ns-begin, read_in_stall=bool(reads),
                                  last_read_flip=last_read[4] if last_read else None,
                                  read_same_observer_serial=bool(last_read and last_read[4] == flip),
                                  last_read_to_end_ns=ns-last_read[1] if last_read else None))
                active_stall = None
            completed_stalls.add(flip)
        if event not in expected:
            if pending:
                raise ValueError('Probe event interleaved inside atomic presentation tuple')
            continue
        if event == 54:
            source_counts[flip] += 1
            if flip not in completed_stalls:
                source_before_retire += 1
        if not pending:
            if event == 54:
                pending = [row]
                leading_tuple_allowed = False
            elif leading_tuple_allowed:
                # Capture can start at any of the four consecutive events.
                pending = [row]
                leading_tuple_allowed = False
            else:
                raise ValueError('Interior presentation tuple missing SOURCE')
        else:
            previous_index = expected.index(pending[-1][3])
            if expected[previous_index + 1] != event or flip != pending[0][4]:
                raise ValueError('Out-of-order presentation tuple or changed flip serial')
            pending.append(row)
        if event != 56:
            continue
        if pending[0][3] != 54:
            censored.append(dict(edge='leading', tid=tid, flip=flip,
                                 observed_events=[r[3] for r in pending]))
            pending = []
            continue
        source, content, cadence, result = pending
        before_requests = content[6] & ((1 << 61) - 1)
        after_requests = result[5]
        if after_requests < before_requests:
            raise ValueError('Presentation request count decreased')
        request_delta = after_requests - before_requests
        return_code = result[6] >> 56
        dropped_delta = result[6] & ((1 << 56) - 1)
        if return_code not in (0, 1, 2):
            raise ValueError('Unknown native presentation return code')
        if dropped_delta > request_delta:
            raise ValueError('Presentation dropped delta exceeds new request count')
        classification = ('accepted_present_request' if return_code == 1 and
                          request_delta == 1 and dropped_delta == 0 else
                          'retry_or_dropped' if return_code == 2 or dropped_delta else
                          'no_accepted_request_observed' if request_delta == 0 else
                          'unclassified_request_result')
        attempts.append(dict(flip=flip, tid=tid, begin_ns=source[1], end_ns=result[1],
                             elapsed_ns=result[1]-source[1], source_scanout=source[5],
                             source_vk_image=source[6], draw_time=content[5],
                             guest_changed=bool(content[6] & (1 << 63)),
                             window_redraw=bool(content[6] & (1 << 62)),
                             redraw_pending=bool(content[6] & (1 << 61)),
                             before_requests=before_requests, after_requests=after_requests,
                             request_delta=request_delta, dropped_delta=dropped_delta,
                             return_code=return_code, vblank=cadence[5], frame_time=cadence[6],
                             classification=classification, unique_original_frame_proven=False))
        pending = []
    if pending:
        censored.append(dict(edge='trailing', tid=pending[0][2], flip=pending[0][4],
                             observed_events=[r[3] for r in pending]))
    intervals = [(p['begin_ns'], p['end_ns']) for p in pairs]
    clipped = list(intervals)
    if active_stall is not None:
        clipped.append((active_stall[1], rows[-1][1]))
    return dict(window_begin_ns=window_begin, window_end_ns=window_end,
                probe_worker_tid=next(iter(tids)) if tids else None,
                event_counts={str(e): sum(r[3] == e for r in rows) for e in range(50, 58)},
                complete_stalls=distribution([p['duration_ns'] for p in pairs]),
                complete_stall_union_ns=union(intervals),
                observed_stall_including_censored_union_ns=union(clipped),
                unmatched_ends_censored=unmatched_ends,
                unmatched_begins_censored=int(active_stall is not None), repeated_begins=0,
                write_periods=distribution([b[0]-a[0] for a,b in zip(writes,writes[1:])]),
                read_to_resume=distribution([p['last_read_to_end_ns'] for p in pairs
                                            if p['last_read_to_end_ns'] is not None]),
                same_serial_read_to_resume=distribution([p['last_read_to_end_ns'] for p in pairs
                                                        if p['read_same_observer_serial']]),
                source_candidates_before_observed_stall_retirement=source_before_retire,
                source_candidates_per_write_id=dict(source_counts), complete_stall_chains=pairs,
                presentation_attempts=attempts, censored_presentation_attempts=censored,
                presentation_summary=dict(classifications=dict(Counter(a['classification'] for a in attempts)),
                                          request_delta=sum(a['request_delta'] for a in attempts),
                                          dropped_delta=sum(a['dropped_delta'] for a in attempts),
                                          guest_changed=sum(a['guest_changed'] for a in attempts),
                                          frame_time_counts=dict(Counter(a['frame_time'] for a in attempts))),
                limits=['Stall is wall duration of the observed PFIFO waiting_for_flip state, not CPU time or removable savings.',
                        'Censored union includes only trailing BEGIN to the last observed probe event; missing leading BEGIN contributes no invented duration. Neither edge is extended to header bounds.',
                        'READ-to-resume uses the latest observed READ, possibly before WRITE and a different observer serial; same_serial_read_to_resume is separate. Neither proves completion of a particular original image.',
                        'Accepted requests are not WSI completion or unique original frames.',
                        'Only global reservation order is validated; other producer timestamps may invert. Probe worker timestamps must not invert.'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path, nargs='?')
    parser.add_argument('--game-log', type=Path)
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args(argv)
    if args.selftest:
        import subprocess
        return subprocess.call([sys.executable, str(ROOT / 'tests/scripts/test_live_cadence_analysis.py')])
    if args.trace is None:
        parser.error('trace is required unless --selftest is used')
    try:
        data = args.trace.read_bytes()
        header, rows, inversions = BASE.decode_records(data)
        tuples = [(r['seq'],r['ns'],r['tid'],r['event'],r['id'],r['a'],r['b']) for r in rows]
        out = analyze(tuples, header['begin_ns'], header['end_ns'])
        out['header'] = header
        out['reservation_timestamp_inversions_all_producers'] = inversions
        out['completeness'] = BASE.read_stats(args.game_log.read_text() if args.game_log else None, len(rows))
        out['analyzer_source'] = dict(path=str(Path(__file__).resolve()), sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        out['source'] = dict(path=str(args.trace.resolve()), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        if out['completeness']['status'] != 'complete_captured_prefix':
            out['observed_stall_including_censored_union_ns'] = None
        print(json.dumps(out, indent=2))
        return 0
    except (ValueError, OSError) as exc:
        print('Invalid probe trace: ' + str(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
