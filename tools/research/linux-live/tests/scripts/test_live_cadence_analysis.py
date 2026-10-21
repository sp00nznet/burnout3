"""Real parser CLI, independently constructed small binary trace fixtures."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/analyze-xemu-live-cadence.py'


def binary(records=(), **changes):
    header = [0x4233545241434531, 1, 56, 0x0102030405060708, 100, 1000, 100, 1]
    for index, value in changes.items():
        header[int(index)] = value
    return struct.pack('<8Q', *header) + b''.join(struct.pack('<7Q', *r) for r in records)


class ProbeParser(unittest.TestCase):
    def run_parser(self, data, stats=None):
        with tempfile.TemporaryDirectory() as name:
            folder = Path(name); path = folder / 'trace.bin'; path.write_bytes(data)
            command = [sys.executable, str(SCRIPT), str(path)]
            if stats is not None:
                log = folder / 'game.log'; log.write_text(stats)
                command += ['--game-log', str(log)]
            return subprocess.run(command, text=True, capture_output=True)

    def test_same_flip_across_two_workers_is_rejected(self):
        result = self.run_parser(binary([(1, 110, 8, 51, 7, 0, 0), (2, 160, 9, 53, 7, 0, 0)]))
        self.assertNotEqual(result.returncode, 0)

    def test_header_marker_clock_capacity_and_timestamp_bounds_are_enforced(self):
        good = [(1, 110, 8, 50, 7, 0, 0)]
        for index, value in [(3, 0), (7, 0), (6, 0), (5, 99)]:
            with self.subTest(index=index):
                self.assertNotEqual(self.run_parser(binary(good, **{str(index): value})).returncode, 0)
        self.assertNotEqual(self.run_parser(binary([(1, 1000, 8, 50, 7, 0, 0)])).returncode, 0)

    def test_duplicate_or_noncontiguous_reservation_is_rejected(self):
        data = binary([(1, 110, 8, 50, 7, 0, 0), (1, 120, 8, 51, 7, 0, 0)])
        self.assertNotEqual(self.run_parser(data).returncode, 0)

    def test_wsi_tuple_exposes_success_drop_and_content_observation(self):
        records = [(1, 110, 8, 50, 7, 0, 0),
                   (2, 120, 8, 54, 7, 0x2020000, 99),
                   (3, 121, 8, 55, 7, 1234, (1 << 63) | 10),
                   (4, 122, 8, 57, 7, 61, 7),
                   (5, 130, 8, 56, 7, 11, 1 << 56)]
        result = self.run_parser(binary(records))
        self.assertEqual(result.returncode, 0, result.stderr)
        out = json.loads(result.stdout)
        self.assertIn('presentation_attempts', out)
        attempt = out['presentation_attempts'][0]
        self.assertEqual(attempt['elapsed_ns'], 10)
        self.assertEqual(attempt['request_delta'], 1)
        self.assertEqual(attempt['dropped_delta'], 0)
        self.assertTrue(attempt['guest_changed'])
        self.assertFalse(attempt['window_redraw'])
        self.assertEqual(attempt['vblank'], 61)
        self.assertEqual(attempt['classification'], 'accepted_present_request')
        self.assertFalse(attempt['unique_original_frame_proven'])

    def test_interior_wsi_tuple_out_of_order_is_rejected(self):
        result = self.run_parser(binary([(1, 110, 8, 54, 7, 0, 0),
                                        (2, 120, 8, 56, 7, 1, 1 << 56)]))
        self.assertNotEqual(result.returncode, 0)

    def test_leading_partial_tuple_and_trailing_partial_stall_are_censored(self):
        result = self.run_parser(binary([(1, 110, 8, 55, 7, 1234, 0),
                                        (2, 120, 8, 57, 7, 61, 7),
                                        (3, 130, 8, 56, 7, 1, 1 << 56),
                                        (4, 140, 8, 51, 7, 7, 0)]))
        self.assertEqual(result.returncode, 0, result.stderr)
        out = json.loads(result.stdout)
        self.assertEqual(len(out.get('censored_presentation_attempts', [])), 1)
        self.assertEqual(out['unmatched_begins_censored'], 1)
        self.assertEqual(out['complete_stalls']['count'], 0)

    def test_lost_trace_stats_are_reported_and_not_claimed_complete(self):
        data = binary([(1, 110, 8, 50, 7, 0, 0)])
        result = self.run_parser(data, '[XEMU-LIVE] causal_trace reserved=2 written=1 dropped=1 invalid=0 io_errors=0\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout).get('completeness', {}).get('status'), 'incomplete')

    def test_complete_stall_uses_matching_read_and_no_duplicate_sum(self):
        records = [(1, 110, 8, 50, 7, 0, 0), (2, 120, 8, 51, 7, 0, 0),
                   (3, 160, 8, 52, 7, 0, 0), (4, 180, 8, 53, 7, 0, 0)]
        result = self.run_parser(binary(records))
        self.assertEqual(result.returncode, 0, result.stderr)
        out = json.loads(result.stdout)
        self.assertEqual(out['complete_stall_union_ns'], 60)
        self.assertEqual(out['read_to_resume']['sum_ns'], 20)

    def test_big_endian_valid_and_other_producer_clock_inversion_allowed(self):
        words = [0x4233545241434531, 1, 56, 0x0102030405060708, 100, 1000, 100, 1]
        rows = [(1, 150, 8, 50, 7, 0, 0), (2, 120, 9, 17, 0, 0, 0)]
        data = struct.pack('>8Q', *words) + b''.join(struct.pack('>7Q', *r) for r in rows)
        result = self.run_parser(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['header']['byte_order'], 'big')

    def test_worker_clock_and_flip_serial_inversions_fail(self):
        for rows in [[(1, 150, 8, 50, 7, 0, 0), (2, 120, 8, 51, 7, 0, 0)],
                     [(1, 120, 8, 50, 7, 0, 0), (2, 150, 8, 51, 6, 0, 0)]]:
            self.assertNotEqual(self.run_parser(binary(rows)).returncode, 0)

    def test_request_backwards_or_changed_tuple_flip_is_rejected(self):
        rows = [(1, 110, 8, 54, 7, 0, 0), (2, 120, 8, 55, 7, 0, 10),
                (3, 130, 8, 57, 7, 61, 7), (4, 140, 8, 56, 7, 9, 1 << 56)]
        self.assertNotEqual(self.run_parser(binary(rows)).returncode, 0)
        rows[3] = (4, 140, 8, 56, 8, 11, 1 << 56)
        self.assertNotEqual(self.run_parser(binary(rows)).returncode, 0)

    def test_censored_union_requires_external_zero_loss_stats(self):
        data = binary([(1, 110, 8, 51, 7, 0, 0)])
        result = self.run_parser(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(json.loads(result.stdout)['observed_stall_including_censored_union_ns'])
        result = self.run_parser(data, '[XEMU-LIVE] causal_trace reserved=1 written=1 dropped=0 invalid=0 io_errors=0\n')
        self.assertEqual(json.loads(result.stdout)['observed_stall_including_censored_union_ns'], 0)

    def test_read_before_stall_still_links_same_serial_retirement(self):
        rows = [(1, 110, 8, 50, 7, 0, 0), (2, 130, 8, 52, 7, 0, 0),
                (3, 140, 8, 51, 7, 0, 0), (4, 150, 8, 53, 7, 0, 0)]
        result = self.run_parser(binary(rows))
        self.assertEqual(result.returncode, 0, result.stderr)
        out = json.loads(result.stdout)
        self.assertEqual(out['read_to_resume']['sum_ns'], 20)
        self.assertFalse(out['complete_stall_chains'][0]['read_in_stall'])


if __name__ == '__main__':
    unittest.main()
