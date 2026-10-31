#!/usr/bin/env python3
"""Self-test for tools/session_report.py on a synthetic run directory (no game content). Run: python3 -I <this>."""
import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import session_report as sr  # noqa: E402

LOG = """[2026-10-21 15:00:01.000] [info] [krnl] [t36] [content] recovery in /run-data/saves/X: removed the staging copy of P (no uncommitted changes)
[2026-10-21 15:00:05.000] [info] [core] [t36] [kuf-input] context: gameplay (frame 100)
[2026-10-21 15:00:06.000] [warning] [gpu] [t9] Skipping Vulkan frame presentation due to async placeholder draw usage in this frame
[2026-10-21 15:00:07.000] [warning] [gpu] [t9] Skipping Vulkan frame presentation due to async placeholder draw usage in this frame
[2026-10-21 15:01:00.000] [info] [krnl] [t36] [content] flush (save message 0x41127) 'P' (type 00000001): committed (2 files, 10 bytes; changed ~CL_1)
[2026-10-21 15:01:30.000] [error] [krnl] [t36] something unexpected at 0x82001234
[2026-10-21 15:01:31.000] [error] [krnl] [t36] something unexpected at 0x82005678
"""


class SessionReportTest(unittest.TestCase):
    def test_report(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "user"))
            # 12 intervals: 6 warm-up, then 300, 300, 240 (24 fps), 250 (25 fps), 300 frames; the last is partial
            summary = {"duration_s": 115.0, "exit_reason": "shutdown",
                       "frame_timeline": [200] * 6 + [300, 300, 240, 250, 300, 40],
                       "cpu_percent": [250] * 11 + [10],
                       "rss_mb": [1000] * 6 + [1100, 1110, 1120, 1130, 1140, 0]}
            json.dump(summary, open(os.path.join(d, "user", "run-summary.json"), "w"))
            open(os.path.join(d, "kuf_cod.log"), "w").write(LOG)
            open(os.path.join(d, "exit_status"), "w").write("0\n")
            r = sr.report(d)
        self.assertEqual(r["exit_status"], "0")
        self.assertEqual(r["fps"]["intervals"], 5)
        self.assertEqual(r["fps"]["below_27"], 2)
        self.assertEqual(r["fps"]["longest_low_run_s"], 20.0)
        self.assertEqual(r["fps"]["min"], 24.0)
        self.assertEqual(r["rss_mb"]["at_60s"], 1100)
        self.assertEqual(r["rss_mb"]["growth_after_warmup"], 40)
        self.assertAlmostEqual(r["rss_mb"]["slope_mb_per_hour"], 3600.0, delta=1.0)  # 10 MB per 10 s
        self.assertEqual(len(r["saves"]), 1)  # the recovery line is not a save
        self.assertIn("changed ~CL_1", r["saves"][0])
        self.assertEqual(r["errors_unexpected"], [("something unexpected at #", 2)])  # grouped
        self.assertEqual(r["warnings"][0][1], 2)
        self.assertEqual(r["contexts"], {"gameplay": 1})


if __name__ == "__main__":
    unittest.main()
