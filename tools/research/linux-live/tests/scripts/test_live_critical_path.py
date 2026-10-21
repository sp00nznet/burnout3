"""Offline trace interval accounting: synthetic records, no C/GPU/game calls."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
PATH = ROOT / 'scripts/analyze-xemu-live-critical-path.py'


def helper():
    if not PATH.exists():
        raise AssertionError('Exclusive critical-path analyzer is missing')
    spec = importlib.util.spec_from_file_location('critical_path', PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rows(records):
    return [dict(seq=i + 1, ns=ns, tid=tid, event=event, id=id_, a=a, b=b)
            for i, (ns, tid, event, id_, a, b) in enumerate(records)]


def barrier(cause, depth=0, pending=0, recording=False):
    return cause | ((1 | 32 | (2 if recording else 0)) << 16) | (pending << 32) | (depth << 48)


class CriticalPath(unittest.TestCase):
    def test_partition_subtracts_nested_intervals_and_clips_window(self):
        # Double-counting the inner interval or accepting its unclipped start fails.
        result = helper().partition_intervals(100, 200, [
            (90, 180, 'service', 1), (120, 160, 'outer', 2),
            (130, 150, 'inner', 3)])
        self.assertEqual(result['totals_ns'],
                         {'service': 40, 'outer': 20, 'inner': 20, 'unobserved': 20})
        self.assertEqual(result['partition_sum_ns'], 100)

    def test_partition_rejects_ambiguous_equal_priority_overlap(self):
        with self.assertRaises(ValueError):
            helper().partition_intervals(0, 100, [(10, 30, 'a', 1), (20, 40, 'b', 1)])

    def test_queue_measures_other_service_coverage_without_calling_it_idle(self):
        record = rows([
            (110, 1, 1, 1, 0xabc, 0), (115, 2, 1, 2, 0xdef, 0),
            (120, 9, 2, 1, 0xabc, 0), (121, 9, 5, 1, 0xabc, 0),
            (125, 9, 6, 1, 0xabc, 0), (145, 9, 7, 1, 0xabc, 0),
            (150, 9, 3, 1, 0xabc, 0), (155, 1, 4, 1, 0xabc, 0),
            (160, 9, 2, 2, 0xdef, 0), (170, 9, 3, 2, 0xdef, 0),
            (175, 2, 4, 2, 0xdef, 0)])
        out = helper().analyze_rows(record, 100, 200)
        worker = out['workers']['9']
        self.assertEqual(worker['partition']['totals_ns'], {
            'service_other_elapsed': 36, 'gate_wait_elapsed': 4, 'unobserved': 60})
        req = out['requests'][1]
        self.assertEqual(req['queue_ns'], 45)
        self.assertEqual(req['queue_during_observed_worker_service_ns'], 30)
        self.assertEqual(req['queue_outside_observed_worker_service_ns'], 15)
        self.assertEqual(out['callers']['2']['partition']['totals_ns'],
                         {'queue': 45, 'own_service': 10, 'wake_return': 5, 'unobserved': 40})

    def test_window_cut_does_not_manufacture_missing_service_or_return(self):
        out = helper().analyze_rows(rows([(110, 9, 3, 99, 0xabc, 0),
                                          (120, 1, 4, 99, 0xabc, 0)]), 100, 200)
        req = out['requests'][0]
        self.assertIsNone(req['service_ns'])
        self.assertIsNone(req['queue_ns'])
        self.assertEqual(req['wake_return_ns'], 10)
        self.assertTrue(req['censored'])

    def test_worker_partition_accounts_barriers_exclusively(self):
        out = helper().analyze_rows(rows([
            (105, 1, 1, 1, 0xabc, 0), (110, 9, 2, 1, 0xabc, 0),
            (120, 9, 43, 3, barrier(1, pending=1), 0),
            (130, 9, 43, 4, barrier(263, depth=1, pending=1), 0),
            (150, 9, 44, 4, barrier(263, depth=1), 0),
            (160, 9, 44, 3, barrier(1), 0),
            (180, 9, 3, 1, 0xabc, 0), (185, 1, 4, 1, 0xabc, 0)]), 100, 200)
        self.assertEqual(out['workers']['9']['partition']['totals_ns'], {
            'service_other_elapsed': 30, 'barrier:NOP_NONZERO': 20,
            'barrier:LEGACY_FLUSH': 20, 'unobserved': 30})
        self.assertEqual(out['workers']['9']['barrier_root_totals_ns'], {'NOP_NONZERO': 40})

    def test_boundary_barrier_does_not_expand_proven_service_union(self):
        out = helper().analyze_rows(rows([
            (110, 9, 43, 1, barrier(1), 0),
            (140, 9, 44, 1, barrier(1), 0),
            (150, 9, 3, 99, 0xabc, 0)]), 100, 200)
        worker = out['workers']['9']
        self.assertEqual(worker['observed_service_union_ns'], 0)
        self.assertEqual(worker.get('barrier_outside_observed_service_ns'), 30)

    def test_closure_scope_is_not_reclassified_as_removable_when_release_empty(self):
        out = helper().analyze_rows(rows([
            (110, 9, 40, 1, 1, 0),
            (120, 9, 43, 2, barrier(1, recording=True), 0),
            (130, 9, 41, 1, 2, 0),
            (140, 9, 42, 1, 2, 0),
            (150, 9, 44, 2, barrier(1), 0),
            (160, 9, 40, 2, 3, 0), (170, 9, 41, 2, 4, 0),
            (180, 9, 42, 2, 4, 0)]), 100, 200)
        first, second = out['main_closures']['ledger']
        self.assertEqual(first['outer_scope'], 'NOP_NONZERO')
        self.assertEqual(first['pending_releases_at_scope_begin'], 0)
        self.assertEqual(first['removable_proven'], False)
        self.assertEqual(second['classification'], 'outside_instrumented_barrier')
        self.assertEqual(second['removable_proven'], False)
        self.assertIsNone(second['finish_reason'])

    def test_repeated_same_caller_intervals_use_union_not_sum(self):
        # A same-thread signal can issue a nested synchronous request.
        record = rows([
            (110, 1, 1, 1, 0xabc, 0), (115, 1, 1, 2, 0xdef, 0),
            (120, 9, 2, 1, 0xabc, 0), (130, 9, 3, 1, 0xabc, 0),
            (135, 9, 2, 2, 0xdef, 0), (140, 9, 3, 2, 0xdef, 0),
            (150, 1, 4, 2, 0xdef, 0), (160, 1, 4, 1, 0xabc, 0)])
        out = helper().analyze_rows(record, 100, 200)
        self.assertEqual(out['callers']['1']['caller_union_ns'], 50)
        self.assertEqual(out['callers']['1']['caller_sum_ns'], 85)
        self.assertEqual(out['callers']['1']['overlap_ns'], 35)
        self.assertEqual(out['callers']['1']['partition']['partition_sum_ns'], 100)

    def test_same_worker_overlapping_service_is_rejected(self):
        record = rows([
            (110, 1, 1, 1, 0xabc, 0), (111, 2, 1, 2, 0xdef, 0),
            (120, 9, 2, 1, 0xabc, 0), (130, 9, 2, 2, 0xdef, 0),
            (140, 9, 3, 1, 0xabc, 0), (150, 9, 3, 2, 0xdef, 0),
            (160, 1, 4, 1, 0xabc, 0), (170, 2, 4, 2, 0xdef, 0)])
        with self.assertRaises(ValueError):
            helper().analyze_rows(record, 100, 200)


if __name__ == '__main__':
    unittest.main()
