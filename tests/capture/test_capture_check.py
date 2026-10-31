#!/usr/bin/env python3
"""Self-test for tools/capture_check.py on synthetic images (no game content). Run: python3 -I <this file>.

Each case writes a captures directory in the format of src/runtime/frame_capture.cpp and checks that the
right validation passes or fails.
"""
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stdout

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import capture_check as cc  # noqa: E402

W, H = 320, 180
EXPECT = """
[format]
width = 320
height = 180
[[range]]
name = "logo"
frames = [10, 10]
optional = true
mean = [20.0, 160.0]
min_distinct = 30
[[range]]
name = "fade"
frames = [11, 99]
optional = true
allow_blank = true
[[range]]
name = "loop"
frames = [100, 100000]
mean = [20.0, 160.0]
std = [10.0, 90.0]
min_distinct = 100
max_black = 0.9
[[animation]]
name = "loop"
frames = [100, 100000]
min_change = 0.5
min_captures = 3
"""


def png_bytes(img, corrupt_crc=False):
    h, w, _ = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))

    def chunk(t, body):
        crc = zlib.crc32(t + body) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + t + body + struct.pack(">I", crc ^ (1 if corrupt_crc and t == b"IDAT" else 0))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def scene(t, w=W, h=H):
    """Smooth synthetic pattern that moves with t."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    r = 128 + 100 * np.sin((x + 7 * t) / 23.0)
    g = 128 + 100 * np.cos((y - 5 * t) / 17.0)
    b = 128 + 100 * np.sin((x + y + 3 * t) / 31.0)
    return np.clip(np.stack([r, g, b], axis=2), 0, 255).astype(np.uint8)


def write_captures(d, frames, recorded_size=None):
    """frames: list of (frame, img or None for a failed capture, kwargs)."""
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "captures.jsonl"), "w") as idx:
        for fr, img, opts in frames:
            if img is None:
                idx.write(json.dumps({"requested_frame": fr, "ok": False, "time_s": 0.0}) + "\n")
                continue
            name = f"frame_{fr:06d}.png"
            with open(os.path.join(d, name), "wb") as f:
                f.write(png_bytes(img, corrupt_crc=opts.get("corrupt", False)))
            w, h = recorded_size or (img.shape[1], img.shape[0])
            row = {"requested_frame": fr, "ok": True, "file": name, "width": w, "height": h, "time_s": fr / 30.0}
            if opts.get("label"):
                row["label"] = opts["label"]
            idx.write(json.dumps(row) + "\n")


def good_frames():
    return [(10, scene(0), {}), (50, np.zeros((H, W, 3), np.uint8), {})] + \
        [(fr, scene(fr), {}) for fr in (100, 160, 220, 280)]


class CaptureCheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = os.path.join(self.tmp.name, "captures")
        self.exp = os.path.join(self.tmp.name, "exp.toml")
        open(self.exp, "w").write(EXPECT)

    def tearDown(self):
        self.tmp.cleanup()

    def check(self):
        out = io.StringIO()
        with redirect_stdout(out):
            status = cc.cmd_check(self.dir, self.exp)
        return status, out.getvalue()

    def failed(self, text):
        return {l.split("[FAIL] ", 1)[1].split(": ", 1)[0] for l in text.splitlines() if "[FAIL] " in l}

    def test_good_run_passes(self):
        write_captures(self.dir, good_frames())
        status, text = self.check()
        self.assertEqual(status, 0, text)
        self.assertTrue(os.path.exists(os.path.join(self.dir, "capture-report.txt")))

    def test_blank_frame_outside_allowed_range_fails(self):
        frames = good_frames()
        frames[3] = (160, np.full((H, W, 3), 3, np.uint8), {})
        write_captures(self.dir, frames)
        status, text = self.check()
        self.assertEqual(status, 1)
        self.assertIn("blank", self.failed(text))
        self.assertIn("[160]", text)

    def test_frozen_animation_fails(self):
        frames = [(10, scene(0), {})] + [(fr, scene(100), {}) for fr in (100, 160, 220, 280)]
        write_captures(self.dir, frames)
        status, text = self.check()
        self.assertEqual(status, 1)
        self.assertEqual(self.failed(text), {"animation[loop]"})
        self.assertIn("100->160: 0.00", text)

    def test_wrong_dimensions_fail(self):
        frames = [(fr, scene(fr, w=256, h=144), {}) for fr in (100, 160, 220)]
        write_captures(self.dir, frames)
        status, text = self.check()
        self.assertEqual(status, 1)
        self.assertIn("format", self.failed(text))
        self.assertIn("256x144", text)

    def test_index_size_mismatch_fails(self):
        write_captures(self.dir, good_frames(), recorded_size=(640, 360))
        status, text = self.check()
        self.assertIn("format", self.failed(text))

    def test_malformed_png_and_failed_capture_fail(self):
        frames = good_frames()
        frames[2] = (100, scene(100), {"corrupt": True})
        frames.append((400, None, {}))
        write_captures(self.dir, frames)
        status, text = self.check()
        self.assertEqual(status, 1)
        self.assertTrue({"index", "format"} <= self.failed(text), text)
        self.assertIn("CRC mismatch", text)

    def test_statistics_out_of_bounds_fail(self):
        frames = [(fr, (scene(fr) // 8).astype(np.uint8), {}) for fr in (100, 160, 220)]  # far too dark
        write_captures(self.dir, frames)
        status, text = self.check()
        self.assertIn("stats[loop]", self.failed(text))

    def test_missing_index_fails(self):
        os.makedirs(self.dir)
        status, text = self.check()
        self.assertEqual(status, 1)
        self.assertIn("index", self.failed(text))

    CYCLE = """
[[range]]
name = "cycle"
frames = [100, 100000]
allow_blank = true
max_blank_seconds = 4.0
any_mean_above = 100.0
[[animation]]
name = "cycle"
frames = [100, 100000]
min_change = 0.5
ignore_blank_pairs = true
"""

    def check_cycle(self, frames):
        open(self.exp, "w").write(self.CYCLE)
        write_captures(self.dir, frames)  # time_s = frame / 30
        return self.check()

    def test_short_fade_passes_and_long_black_fails(self):
        black = np.zeros((H, W, 3), np.uint8)
        bright = (scene(0) // 2 + 120).astype(np.uint8)
        short = [(100, bright, {}), (160, black, {}), (220, black, {}), (280, scene(280), {})]  # 2 s black
        status, text = self.check_cycle(short)
        self.assertEqual(status, 0, text)
        long = [(100, bright, {})] + [(fr, black, {}) for fr in range(160, 400, 60)] + [(400, scene(400), {})]
        status, text = self.check_cycle(long)
        self.assertEqual(self.failed(text), {"stats[cycle]"}, text)
        self.assertIn("blank from frame 160 to 340", text)

    def test_missing_bright_scene_fails(self):
        status, text = self.check_cycle([(fr, scene(fr) // 3, {}) for fr in (100, 160, 220)])
        self.assertIn("stats[cycle]", self.failed(text))
        self.assertIn("no capture with mean >= 100.0", text)

    def test_frozen_non_blank_image_still_fails_with_ignore_blank_pairs(self):
        bright = (scene(0) // 2 + 120).astype(np.uint8)
        status, text = self.check_cycle([(100, bright, {}), (160, bright, {}), (220, scene(220), {})])
        self.assertEqual(self.failed(text), {"animation[cycle]"}, text)

    def test_highlight_finds_the_selected_row(self):
        open(self.exp, "w").write("""
[[highlight]]
name = "menu"
x = [100, 220]
rows = [40, 80, 120, 160]
half_height = 8
min_margin = 8.0
expect = { "100" = 2, "160" = 0 }
""")
        def menu(sel):
            img = np.full((H, W, 3), 20, np.uint8)
            for i, y in enumerate((40, 80, 120, 160)):
                img[y - 3:y + 3, 110:210] = 200                    # white item text
                if i == sel:
                    img[y + 4:y + 7, 100:220] = (230, 120, 20)     # orange glow under the selected item
            return img
        write_captures(self.dir, [(100, menu(2), {}), (160, menu(0), {})])
        status, text = self.check()
        self.assertEqual(status, 0, text)
        write_captures(self.dir, [(100, menu(1), {}), (160, menu(0), {})])
        status, text = self.check()
        self.assertEqual(self.failed(text), {"highlight[menu]"}, text)
        self.assertIn("100: selected row 1", text)
        write_captures(self.dir, [(100, menu(None), {}), (160, menu(0), {})])  # nothing selected: no margin
        status, text = self.check()
        self.assertIn("highlight[menu]", self.failed(text))

    def test_region_region_diff_and_shift_by_label(self):
        open(self.exp, "w").write("""
[[region]]
name = "hud"
capture = "before"
rect = [10, 150, 60, 170]
metric = "red_minus_blue"
mean = [100.0, 255.0]
[[region_diff]]
name = "exp line changed"
a = "before"
b = "after"
rect = [100, 20, 200, 40]
min_mean_diff = 5.0
[[region_diff]]
name = "title unchanged"
a = "before"
b = "after"
rect = [100, 60, 200, 80]
max_mean_diff = 1.0
[[shift]]
name = "camera turned"
a = "before"
b = "turned"
rect = [0, 90, 320, 140]
direction = "left"
min_px = 10
""")
        def screen(exp_text_bright, offset=0):
            img = (scene(0)[:, :, :] // 4).astype(np.uint8)
            img[90:140] = np.roll(scene(7)[90:140], -offset, axis=1)  # scenery band for the shift check
            img[150:170, 10:60] = (220, 40, 30)                        # red HUD bar
            img[20:40, 100:200] = 200 if exp_text_bright else 60       # "EXP" line
            img[60:80, 100:200] = 120                                  # static title
            return img
        frames = [(100, screen(False), {"label": "before"}), (160, screen(True), {"label": "after"}),
                  (220, screen(False, offset=25), {"label": "turned"})]
        write_captures(self.dir, frames)
        status, text = self.check()
        self.assertEqual(status, 0, text)
        self.assertIn("content moved -25 px", text)
        frames[1] = (160, screen(False), {"label": "after"})        # the EXP line did not change
        frames[2] = (220, screen(False, offset=-25), {"label": "turned"})  # turned the other way
        write_captures(self.dir, frames)
        status, text = self.check()
        self.assertEqual(self.failed(text), {"region_diff[exp line changed]", "shift[camera turned]"}, text)
        frames = [f for f in frames if f[2]["label"] != "after"]  # a missing label fails clearly
        write_captures(self.dir, frames)
        status, text = self.check()
        self.assertIn("missing capture 'before' or 'after'", text)

    def test_guest_coordinates_reduce_a_native_capture_to_the_frame(self):
        # A frame rendered at 3x2 (3840x1440): each guest pixel is a 3x2 block. With guest_coordinates the rules
        # see the 1280x720 frame (block averages), so rectangles in frame pixels hold at any render scale.
        frame = np.full((720, 1280, 3), 30, np.uint8)
        frame[600:640, 200:400] = (220, 40, 30)   # a red HUD bar, in frame pixels
        native = np.repeat(np.repeat(frame, 2, axis=0), 3, axis=1)
        rules = """
[format]
render_scale = [3, 2]
guest_coordinates = true
[[region]]
name = "hud bar"
capture = 100
rect = [200, 600, 400, 640]
metric = "red_minus_blue"
mean = [180.0, 255.0]
"""
        open(self.exp, "w").write(rules)
        write_captures(self.dir, [(100, native, {})])
        status, text = self.check()
        self.assertEqual(status, 0, text)
        # Without the reduction the same rectangle looks at another part of the native capture.
        open(self.exp, "w").write(rules.replace("guest_coordinates = true", "guest_coordinates = false"))
        status, text = self.check()
        self.assertEqual(self.failed(text), {"region[hud bar]"}, text)
        # A capture of another size: format fails (not 3840x1440) and it is not a whole multiple of the frame.
        open(self.exp, "w").write(rules)
        write_captures(self.dir, [(100, native[:, :3000], {})])
        status, text = self.check()
        self.assertEqual(self.failed(text), {"format", "guest_frame", "region[hud bar]"}, text)

    def test_any_of_group_and_text_metric(self):
        open(self.exp, "w").write("""
[[region_diff]]
name = "gold changed"
a = "before"
b = "after"
rect = [100, 20, 200, 40]
metric = "text"
min_mean_diff = 1.0
any_of = "loot"
[[region]]
name = "items listed"
capture = "after"
rect = [100, 60, 200, 100]
min_std = 12.0
any_of = "loot"
""")
        def screen(digit_x, items, background):
            img = np.full((H, W, 3), background, np.uint8)  # the scene behind a translucent panel
            img[24:36, 110:130] = 230                       # "Gold:" glyphs
            img[24:36, digit_x:digit_x + 8] = 230           # the changing digit
            if items:
                img[64:70, 110:190] = 200                   # one item line
            return img
        def run(after_digit, items):
            write_captures(self.dir, [(100, screen(150, False, 40), {"label": "before"}),
                                      (160, screen(after_digit, items, 90), {"label": "after"})])
            return self.check()
        status, text = run(170, False)  # gold changed, no items: loot
        self.assertEqual(status, 0, text)
        self.assertIn("any_of[loot]: 1 of 2", text)
        status, text = run(150, True)   # items, gold unchanged although the background changed: loot
        self.assertEqual(status, 0, text)
        status, text = run(150, False)  # neither: no loot; members are [--], the group fails
        self.assertEqual(self.failed(text), {"any_of[loot]"}, text)
        self.assertIn("[--] region_diff[gold changed]", text)

    def test_right_aligned_text_compares_the_end_of_a_value(self):
        # "HP: c/m": the current value c has one or two digits, so the maximum m after the slash moves.
        open(self.exp, "w").write("""
[[region_diff]]
name = "maximum as saved"
a = "saved"
b = "loaded"
rect = [100, 20, 220, 40]
metric = "text"
align_x = "right"
keep_px = 23
max_mean_diff = 1.0
""")
        def glyph(img, x, d):  # an 8 px wide digit; each digit has its own pattern of rows
            for row in range(10):
                if (d >> (row % 4)) & 1 or row in (0, 9):
                    img[25 + row, x:x + 8] = 230
        def screen(current, maximum):
            img = np.full((H, W, 3), 40, np.uint8)
            x = 110
            for d in current + [None] + maximum:  # None = the slash, 2 px wide
                if d is None:
                    img[25:35, x + 3:x + 5] = 230
                else:
                    glyph(img, x, d)
                x += 11
            return img
        def run(saved, loaded):
            write_captures(self.dir, [(100, screen(*saved), {"label": "saved"}),
                                      (160, screen(*loaded), {"label": "loaded"})])
            return self.check()
        status, text = run(([6], [7, 8]), ([7, 8], [7, 8]))  # 6/78 then 78/78: same maximum
        self.assertEqual(status, 0, text)
        status, text = run(([6], [7, 8]), ([7, 9], [7, 9]))  # 6/78 then 79/79: another maximum
        self.assertEqual(self.failed(text), {"region_diff[maximum as saved]"}, text)
        status, text = run(([6], [7, 8]), ([7, 8], [7, 8, 1]))  # a maximum with a third digit
        self.assertEqual(self.failed(text), {"region_diff[maximum as saved]"}, text)

    def test_text_rel_reads_dim_rows_and_empty_rectangles_fail(self):
        # A list row: text at luma 180 (below the status values' threshold of 200) on a plain (50) or a
        # highlighted (110) row background; "unchanged" between two empty rectangles must not pass.
        open(self.exp, "w").write("""
[[region_diff]]
name = "row unchanged (text_rel)"
a = "before"
b = "after"
rect = [100, 20, 220, 40]
metric = "text_rel"
max_mean_diff = 1.0
[[region_diff]]
name = "row unchanged (text 200)"
a = "before"
b = "after"
rect = [100, 20, 220, 40]
metric = "text"
text_threshold = 200.0
max_mean_diff = 1.0
[[region_diff]]
name = "empty slot"
a = "before"
b = "after"
rect = [100, 60, 220, 80]
metric = "text_rel"
allow_empty = true
max_mean_diff = 1.0
""")
        def screen(word, row_bg):
            img = np.full((H, W, 3), 40, np.uint8)
            img[20:40, 100:220] = row_bg
            for i, ch in enumerate(word):  # one 8 px glyph per character, its rows from the character code
                for row in range(12):
                    if (ord(ch) >> (row % 7)) & 1:
                        img[24 + row, 104 + 11 * i:112 + 11 * i] = 180
            img[60:80, 100:220] = 50  # an empty slot
            return img
        def run(before, after):
            write_captures(self.dir, [(100, screen(*before), {"label": "before"}),
                                      (160, screen(*after), {"label": "after"})])
            return self.check()
        status, text = run(("Mace", 50), ("Mace", 110))   # same text, plain row then highlighted row
        self.assertEqual(self.failed(text), {"region_diff[row unchanged (text 200)]"}, text)
        self.assertIn("no text in the rectangle", text)  # the fixed threshold sees nothing: fails, not passes
        status, text = run(("Mace", 50), ("Dash", 50))    # another item in the slot
        self.assertEqual(self.failed(text), {"region_diff[row unchanged (text_rel)]",
                                             "region_diff[row unchanged (text 200)]"}, text)

    def test_rules_from_applies_at_top_level_and_in_scenario(self):
        base = os.path.join(self.tmp.name, "base.toml")
        open(base, "w").write("""
[[region_diff]]
name = "value unchanged"
a = "before"
b = "after"
rect = [100, 20, 200, 40]
max_mean_diff = 1.0
""")
        def screen(bright):
            img = np.full((H, W, 3), 40, np.uint8)
            img[20:40, 100:200] = 200 if bright else 60
            return img
        write_captures(self.dir, [(100, screen(False), {"label": "before"}), (160, screen(True), {"label": "after"})])
        for text in ('rules_from = "base.toml"\n', '[scenario]\nseconds = 10\nrules_from = "base.toml"\n'):
            open(self.exp, "w").write(text)
            status, out = self.check()
            self.assertEqual(self.failed(out), {"region_diff[value unchanged]"}, (text, out))

    def compare(self, ref):
        out = io.StringIO()
        with redirect_stdout(out):
            status = cc.cmd_compare(self.dir, ref, self.exp)
        return status, out.getvalue()

    def test_compare_tolerates_driver_noise_and_catches_visible_change(self):
        write_captures(self.dir, good_frames())
        ref = os.path.join(self.tmp.name, "ref")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(cc.cmd_baseline(self.dir, ref), 0)

        rng = np.random.default_rng(1)
        noisy = []
        for fr, img, o in good_frames():  # +-3 per channel: dithering/rounding differences
            n = np.clip(img.astype(np.int16) + rng.integers(-3, 4, img.shape), 0, 255).astype(np.uint8)
            noisy.append((fr, n, o))
        write_captures(self.dir, noisy)
        status, text = self.compare(ref)
        self.assertEqual(status, 0, text)

        changed = good_frames()
        img = changed[3][1].copy()
        img[40:120, 60:200] = 255  # a visible element that is not in the reference
        changed[3] = (160, img, {})
        write_captures(self.dir, changed)
        status, text = self.compare(ref)
        self.assertEqual(status, 1)
        self.assertEqual(self.failed(text), {"frame 160"})

    def test_compare_ignores_listed_regions_only(self):
        write_captures(self.dir, good_frames())
        ref = os.path.join(self.tmp.name, "ref")
        with redirect_stdout(io.StringIO()):
            cc.cmd_baseline(self.dir, ref)
        changed = good_frames()
        img = changed[3][1].copy()
        img[40:120, 60:200] = 255
        changed[3] = (160, img, {})
        write_captures(self.dir, changed)
        with open(self.exp, "a") as f:
            f.write("\n[compare]\nignore = [[56, 32, 208, 128]]\n")
        status, text = self.compare(ref)
        self.assertEqual(status, 0, text)          # the change lies inside the ignored area
        img2 = img.copy()
        img2[140:170, 230:300] = 0                  # and one outside it
        changed[3] = (160, img2, {})
        write_captures(self.dir, changed)
        status, text = self.compare(ref)
        self.assertEqual(self.failed(text), {"frame 160"}, text)

    def test_compare_without_references_fails(self):
        write_captures(self.dir, good_frames())
        status, text = self.compare(os.path.join(self.tmp.name, "nothing"))
        self.assertEqual(status, 1)
        self.assertIn("references", self.failed(text))

    def test_baseline_refuses_tracked_repository_paths(self):
        write_captures(self.dir, good_frames())
        with self.assertRaises(SystemExit):
            cc.cmd_baseline(self.dir, os.path.join(ROOT, "tests", "capture", "refs"))
        self.assertFalse(os.path.exists(os.path.join(ROOT, "tests", "capture", "refs")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
