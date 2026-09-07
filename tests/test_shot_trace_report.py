import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('shot_trace_report',
    Path(__file__).resolve().parents[1] / 'tools/scripts/shot_trace_report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def record(seq, kind, qpc, identity=0, flags=0):
    return (f'[SHOTTRACE] cap=1 seq={seq} kind={kind} tid=7 qpc={qpc} end=0 '
            f'caller=1010 object=1000 context=0 event=0 id={identity:X} flags={flags} '
            'origin=8,0,0 before=1,0,0 after=0,1,0\n')


class TraceReportTests(unittest.TestCase):
    def test_ring_order_and_reinjection(self):
        lines = (record(2, 4, 120, flags=2) + record(0, 0, 100, identity=1000) +
                 record(1, 3, 110, identity=42) + record(3, 5, 130, flags=108) +
                 record(4, 1, 140, identity=3) +
                 record(0, 0, 1000, identity=1000) + record(1, 1, 1100))
        result = report.summarize(lines)
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0]['crosshair_calls'], 1)
        self.assertEqual(result[0]['duration_ms'], 40)
        self.assertEqual(result[0]['dropped'], 3)
        self.assertEqual(result[0]['weapon_samples'][0]['projectiles_per_shot'], 8)
        self.assertTrue(result[0]['queued_events'][0]['receiver_matches_sampled_weapon'])
        self.assertEqual(result[1]['crosshair_calls'], 0)

    def test_incomplete_and_settings(self):
        flags = 3 | (1 << 8) | (1 << 9) | (1 << 10) | (17 << 12) | (1 << 17) | (1 << 18)
        result = report.summarize(record(0, 0, 100, identity=1000) + record(1, 2, 101, flags=flags))
        self.assertFalse(result[0]['complete_stop'])
        self.assertIsNone(result[0]['dropped'])
        edge = result[0]['input_edges'][0]
        self.assertTrue(edge['silent'] and edge['no_spread'] and edge['nearest'])
        self.assertEqual(edge['bone_mask'], 17)
        self.assertEqual(edge['profile'], 2)
        self.assertIn('error', report.summarize(record(0, 4, 100))[0])
        self.assertEqual(report.summarize('[SHOTTRACE] cap=1 seq='), [])


if __name__ == '__main__':
    unittest.main()
