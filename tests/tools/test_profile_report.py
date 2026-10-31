#!/usr/bin/env python3
"""Self-test for tools/profile_report.py on a synthetic profile directory (no game content).
Run: python3 -I <this>."""
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import profile_report as pr  # noqa: E402


def write_profile(d):
    prof = os.path.join(d, "user", "profile")
    os.makedirs(prof)
    # 10 s at 30 fps (33.333 ms), then 1 s at 20 fps (50 ms), then 2 s at 30 fps again.
    begins, t = [], 0.0
    for interval, count in ((1 / 30, 300), (0.05, 20), (1 / 30, 60)):
        for _ in range(count):
            begins.append(t)
            t += interval
    with open(os.path.join(prof, "frames.csv"), "w") as f:
        f.write("frame,begin_us,present_us\n")
        for i, b in enumerate(begins):
            f.write(f"{i + 1},{round(b * 1e6)},{100 if i % 2 else 300}\n")
    with open(os.path.join(prof, "threads.csv"), "w") as f:
        f.write("t_s,tid,name,cpu_s\n")
        for s in range(1, 13):
            f.write(f"{s},9,GPU Commands (F8000020),0.2\n")
            f.write(f"{s},40,XThreadC63EE6C0,0.5\n")
            f.write(f"{s},41,XThreadC53ED6C0,0.25\n")
    with open(os.path.join(prof, "gpu.csv"), "w") as f:
        f.write("t_s,frame,busy_percent\n")
        for i in range(100):
            f.write(f"{i / 10},{i * 3 + 1},{20 if i < 90 else 60}\n")
    with open(os.path.join(prof, "samples.bin"), "wb") as f:
        for ip, tid, frame in [(0x1000, 40, 10)] * 6 + [(0x2000, 9, 10)] * 3 + [(0x3000, 40, 9999)]:
            f.write(struct.pack("<QII", ip, tid, frame))
    return prof


class FakeSymbols:
    def lookup(self, ip):
        return {0x1000: ("kuf_cod", "sub_82160C90"), 0x2000: ("librexgpu-xenosrd.so", "WriteRegister")}[ip]


class ProfileReportTest(unittest.TestCase):
    def test_frames(self):
        with tempfile.TemporaryDirectory() as d:
            prof = write_profile(d)
            times, pres, ends, begins = pr.read_frames(prof, 0.0, 1e9)
            f = pr.frame_stats(times, pres, ends)
            times2, pres2, ends2, _ = pr.read_frames(prof, 11.5, 1e9)
            g = pr.frame_stats(times2, pres2, ends2)
        self.assertEqual(f["frames"], 379)
        self.assertAlmostEqual(f["median_ms"], 33.33, places=1)
        self.assertAlmostEqual(f["max_ms"], 50.0, places=1)
        self.assertEqual(f["over_40ms"], 20)
        self.assertAlmostEqual(f["worst_1s_mean_ms"], 50.0, delta=2.0)  # the 20 fps second
        self.assertAlmostEqual(f["present_median_ms"], 0.1, places=2)
        self.assertAlmostEqual(f["avg_fps"], 379 / 13.0, delta=0.1)  # 10 s + 1 s + 2 s
        self.assertEqual(g["over_40ms"], 0)  # a range after the slow second
        self.assertAlmostEqual(g["avg_fps"], 30.0, delta=0.2)

    def test_threads_gpu_samples(self):
        with tempfile.TemporaryDirectory() as d:
            prof = write_profile(d)
            groups, seconds = pr.read_threads(prof, 0.0, 0.0, 1e9)
            gpu = pr.read_gpu(prof, 1, 400)
            s = pr.read_samples(prof, 1, 400, FakeSymbols(), {40: "XThreadC63EE6C0", 9: "GPU Commands (F8000020)"},
                                "kuf_cod", 10)
        self.assertEqual(seconds, 12)
        self.assertAlmostEqual(groups["XThread (guest)"], 9.0)  # both guest threads, numbers stripped
        self.assertAlmostEqual(groups["GPU Commands"], 2.4)
        self.assertEqual(len(gpu), 100)
        self.assertEqual(s["samples"], 9)  # the sample at frame 9999 is outside the range
        self.assertEqual(s["by_category"][0], ("guest code (recompiled)", 6))
        self.assertEqual(s["by_thread"][0], ("XThread (guest)", 6))
        self.assertEqual(pr.thread_group("Vulkan Pipeline"), "Vulkan Pipeline")
        self.assertEqual(pr.thread_group("kuf_cod:disk$3"), "kuf_cod:disk")
        self.assertEqual(pr.thread_group("XThreadBED6C0 ("), "XThread (guest)")  # cut at 15 characters
        self.assertEqual(pr.thread_group("XThread1BEE6C"), "XThread (guest)")
        self.assertEqual(pr.thread_group("XThreadC63EE6C0"), "XThread (guest)")


if __name__ == "__main__":
    unittest.main()
