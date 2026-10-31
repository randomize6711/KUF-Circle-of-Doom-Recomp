#!/usr/bin/env python3
"""Screenshot validation for offscreen frame captures (M3). Works on the PNGs and captures.jsonl written by
src/runtime/frame_capture.cpp (or any 8-bit RGB/RGBA PNG), with the standard library + numpy only.

Usage:
  capture_check.py stats    <captures_dir>                        per-frame statistics table
  capture_check.py check    <captures_dir> <expectations.toml> [<previous_captures_dir>]
                                                                  validate against committed expectations
  capture_check.py baseline <captures_dir> <reference_dir>        store captures as local references
  capture_check.py compare  <captures_dir> <reference_dir> [<expectations.toml>]
                                                                 perceptual comparison with local references

`check` and `compare` print one line per validation ([ok]/[FAIL] name: detail), write the same text to
<captures_dir>/capture-report.txt (or compare-report.txt) and exit 1 if any validation failed.

Captures and references are game content: they must stay outside Git. `baseline` refuses a reference
directory inside the repository unless it is under build/. Only statistics (the expectations file) are
committed. Nothing here stores or emits image content.

Validations (`check`):
  index      captures.jsonl exists, every requested frame was captured (ok=true), the PNG files exist
  format     8-bit RGB/RGBA PNG with a valid structure, dimensions as expected and as recorded in the index
  required   the frames listed in required_frames were captured
  blank      frames outside an allow_blank range are not blank (near-uniform: luma std-dev and distinct
             colours at or below the [blank] thresholds)
  stats      per-range bounds on mean luma, luma std-dev, distinct colours and near-black fraction;
             any_mean_above (some capture in the range is at least this bright, e.g. a movie was shown);
             max_blank_seconds (longest run of consecutive blank captures, e.g. a fade)
  highlight  [[highlight]] menus: which of the listed row bands is selected (by a colour statistic per band),
             compared with the expected row per frame. Rows are coordinates only, no image content
  region     [[region]] statistics inside a rectangle of one capture (mean/std of luma or red-minus-blue;
             metric "text" = bright glyph pixels only (luma > text_threshold, default 150), for numbers on
             translucent panels; region_diff align_x = true lines both up at their first glyph column;
             align_x = "right" at their last one, and keep_px = N then compares only the last N columns,
             e.g. the maximum of "HP: 6/78" against "HP: 78/78"); metric "text_rel" = pixels brighter than the
             rectangle's median by text_delta (default 40), for list rows (dimmer text, highlighted rows).
             An unchanged-text rule (max_mean_diff) fails when neither capture has min_glyph_px (20) glyph
             pixels in the rectangle, unless allow_empty = true (an empty slot)
  region_diff [[region_diff]] a rectangle changed (min_mean_diff) or stayed (max_mean_diff) between two
             captures, e.g. the EXP line of the status screen before and after a fight
  shift      [[shift]] the view moved horizontally by at least min_px between two captures, in the given
             direction (phase correlation), e.g. the camera turned
  rules_from = "<other.toml>" adds that file's rules (and [format]); top level or in [scenario].
  [format] guest_coordinates = true: the captures are the game's 1280x720 frame rendered at a whole multiple per
             axis (the render scale, e.g. 3840x1440 = 3x2): every rule after `format` sees each capture reduced to
             1280x720 by averaging each scale x scale block, so rectangles and statistics stay in frame pixels and
             one rule set checks any render resolution. `format` still checks the captured size; with
             render_scale = [x, y] it must be exactly 1280x720 times that.
  Captures are referred to by frame number or by the label of a scripted `capture <label>`; "prev:<label>"
  is a labelled capture of a previous run (`check <captures> <toml> <previous captures>`), e.g. the status
  screen before saving compared with the one after loading in a new process.
  any_of     region and region_diff rules with the same `any_of = "<group>"` are alternatives: the group
             passes when one of them does (e.g. loot = gold changed or items listed)
  animation  in [[animation]] ranges, consecutive captures differ (mean absolute luma change); a frozen
             image (e.g. KNOWN_ISSUES K13) fails here. ignore_blank_pairs skips blank-to-blank pairs
             (fades), whose length the range's max_blank_seconds bounds

Comparison (`compare`): both images are reduced to 8x8-pixel block averages, which absorbs dithering,
filtering and rounding differences between GPUs and drivers. A frame matches when the mean absolute block
difference and the fraction of strongly differing blocks stay within the [compare] tolerances. [compare]
ignore = [[x0, y0, x1, y1], ...] excludes animated areas (e.g. a movie behind a menu, whose frame follows
the real-time audio clock and may shift by a frame between runs).
"""
import json
import os
import shutil
import struct
import sys
import tomllib
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_BLANK = {"max_std": 1.5, "max_distinct": 4}
DEFAULT_COMPARE = {"block": 8, "max_mean_diff": 3.0, "block_diff": 24.0, "max_block_fraction": 0.01}


class PngError(ValueError):
    pass


def read_png(path):
    """Returns (pixels HxWx3 uint8, info dict). Raises PngError on malformed or unsupported files."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise PngError("not a PNG (bad signature)")
    pos, idat, ihdr, seen_end = 8, b"", None, False
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        name = ctype.decode(errors="replace")
        if pos + 12 + length > len(data):
            raise PngError(f"truncated {name} chunk")
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])[0]
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise PngError(f"CRC mismatch in {name} chunk")
        if ctype == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body[:13])
            if depth != 8 or color not in (2, 6) or interlace != 0:
                raise PngError(f"unsupported format depth={depth} color_type={color} interlace={interlace}")
            ihdr = (width, height, 3 if color == 2 else 4)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            seen_end = True
            break
        pos += 12 + length
    if ihdr is None or not seen_end:
        raise PngError("missing IHDR or IEND")
    width, height, channels = ihdr
    if width == 0 or height == 0:
        raise PngError("zero-sized image")
    try:
        raw = zlib.decompress(idat)
    except zlib.error as e:
        raise PngError(f"bad image data: {e}")
    stride = width * channels
    if len(raw) != height * (stride + 1):
        raise PngError(f"image data size {len(raw)} != {height * (stride + 1)}")
    rows = np.frombuffer(raw, dtype=np.uint8).reshape(height, stride + 1)
    if np.any(rows[:, 0] != 0):  # our writer never filters; decode filters 1-4 for foreign PNGs
        pixels = _unfilter(rows, stride, channels)
    else:
        pixels = rows[:, 1:]
    img = pixels.reshape(height, width, channels)[:, :, :3]
    return img, {"width": width, "height": height, "channels": channels}


def _unfilter(rows, stride, channels):
    height = rows.shape[0]
    out = np.zeros((height, stride), dtype=np.int32)
    for y in range(height):
        f, line = rows[y, 0], rows[y, 1:].astype(np.int32)
        prev = out[y - 1] if y else np.zeros(stride, dtype=np.int32)
        if f == 0:
            out[y] = line
        elif f == 2:
            out[y] = (line + prev) & 0xFF
        elif f in (1, 3, 4):
            cur = np.zeros(stride, dtype=np.int32)
            for x in range(stride):
                a = cur[x - channels] if x >= channels else 0
                b = prev[x]
                c = prev[x - channels] if x >= channels else 0
                if f == 1:
                    p = a
                elif f == 3:
                    p = (a + b) // 2
                else:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[x] = (line[x] + p) & 0xFF
            out[y] = cur
        else:
            raise PngError(f"bad filter type {f} in row {y}")
    return out.astype(np.uint8)


GUEST_WIDTH, GUEST_HEIGHT = 1280, 720  # the game's frame (its render targets are a whole multiple of it)


def to_guest(img):
    """A capture of the frame rendered at a whole multiple per axis, reduced to 1280x720 by averaging each block
    (exact: the render scale's blocks are the guest pixels). None if the size is not a whole multiple."""
    h, w = img.shape[:2]
    if w % GUEST_WIDTH or h % GUEST_HEIGHT:
        return None
    sx, sy = w // GUEST_WIDTH, h // GUEST_HEIGHT
    if sx == sy == 1:
        return img
    blocks = img.astype(np.float32).reshape(GUEST_HEIGHT, sy, GUEST_WIDTH, sx, 3).mean(axis=(1, 3))
    return np.clip(np.rint(blocks), 0, 255).astype(np.uint8)


def to_guest_frames(frames):
    out, not_multiple = [], []
    for fr, img, info, r in frames:
        g = to_guest(img)
        if g is None:
            not_multiple.append(f"{fr}: {info['width']}x{info['height']}")
            continue
        out.append((fr, g, info, r))
    return out, not_multiple


def frame_stats(img):
    rgb = img.astype(np.float32)
    luma = 0.2126 * rgb[:, :, 0] + 0.7152 * rgb[:, :, 1] + 0.0722 * rgb[:, :, 2]
    sample = img[::8, ::8].reshape(-1, 3)
    distinct = len(np.unique(sample[:, 0].astype(np.uint32) << 16 | sample[:, 1].astype(np.uint32) << 8
                             | sample[:, 2]))
    return {"mean": float(luma.mean()), "std": float(luma.std()), "distinct": distinct,
            "black": float((luma < 8).mean()), "luma": luma}


def highlight_scores(img, hl):
    """Per row band: the selection statistic. 'red_minus_blue' finds an orange glow, 'luma' a lighter box."""
    x0, x1 = hl["x"]
    above, below = hl.get("half_height", [12, 14]) if isinstance(hl.get("half_height"), list) \
        else (hl.get("half_height", 12),) * 2
    rgb = img.astype(np.float32)
    scores = []
    for y in hl["rows"]:
        band = rgb[max(0, y - above):y + below, x0:x1]
        if hl.get("metric", "red_minus_blue") == "luma":
            scores.append(float(band.mean()))
        else:
            scores.append(float((band[:, :, 0] - band[:, :, 2]).mean()))
    return scores


def region_values(img, rect, metric="luma", text_threshold=150.0, text_delta=40.0):
    x0, y0, x1, y1 = rect
    band = img[y0:y1, x0:x1].astype(np.float32)
    if metric == "red_minus_blue":
        return band[:, :, 0] - band[:, :, 2]
    luma = 0.2126 * band[:, :, 0] + 0.7152 * band[:, :, 1] + 0.0722 * band[:, :, 2]
    if metric == "text":
        # Bright glyph pixels only (255) on 0: a changed number shows, the scene behind a translucent panel
        # does not.
        return np.where(luma > text_threshold, 255.0, 0.0).astype(np.float32)
    if metric == "text_rel":
        # Glyph pixels brighter than the rectangle's own background (its median) by text_delta: list rows whose
        # text is dimmer than the status values, on a plain or a highlighted (selected) row.
        return np.where(luma > np.median(luma) + text_delta, 255.0, 0.0).astype(np.float32)
    return 0.2126 * band[:, :, 0] + 0.7152 * band[:, :, 1] + 0.0722 * band[:, :, 2]


def align_left(mask):
    """Shifts a glyph mask left so that its first non-empty column is column 0 (proportional text: the same
    word after a number of a different width)."""
    cols = np.nonzero(mask.max(axis=0) > 0)[0]
    if not len(cols):
        return mask
    out = np.zeros_like(mask)
    out[:, :mask.shape[1] - cols[0]] = mask[:, cols[0]:]
    return out


def align_right(mask, keep_px=None):
    """Shifts a glyph mask right so that its last non-empty column is the last column; with keep_px, clears
    everything left of the last keep_px columns (the end of a value whose start varies, e.g. "/78" after a
    current HP of one or two digits)."""
    cols = np.nonzero(mask.max(axis=0) > 0)[0]
    out = np.zeros_like(mask)
    if len(cols):
        shift = mask.shape[1] - 1 - cols[-1]
        out[:, shift:] = mask[:, :mask.shape[1] - shift]
    if keep_px is not None:
        out[:, :max(0, out.shape[1] - keep_px)] = 0
    return out


def horizontal_shift(a, b, rect):
    """Pixels the image content moved horizontally from a to b inside rect (phase correlation)."""
    la, lb = region_values(a, rect), region_values(b, rect)
    la, lb = la - la.mean(), lb - lb.mean()
    w = np.hanning(la.shape[1])[None, :] * np.hanning(la.shape[0])[:, None]
    f = np.fft.fft2(lb * w) * np.conj(np.fft.fft2(la * w))
    f /= np.abs(f) + 1e-9
    r = np.fft.ifft2(f).real
    y, x = np.unravel_index(np.argmax(r), r.shape)
    return int(x - r.shape[1]) if x > r.shape[1] // 2 else int(x)


def robust_horizontal_shift(a, b, rect):
    """Median of the shifts in overlapping sub-windows of rect: near objects move differently from distant
    scenery (parallax) and one large foreground object can dominate a single correlation."""
    x0, y0, x1, y1 = rect
    w, h = x1 - x0, y1 - y0
    estimates = []
    for fx in (0.0, 0.17, 0.34):
        for fy in (0.0, 0.3):
            sub = [int(x0 + fx * w), int(y0 + fy * h), int(x0 + (fx + 0.66) * w), int(y0 + (fy + 0.7) * h)]
            estimates.append(horizontal_shift(a, b, sub))
    estimates.append(horizontal_shift(a, b, rect))
    return int(np.median(estimates)), estimates


def is_blank(s, blank):
    return s["std"] <= blank["max_std"] and s["distinct"] <= blank["max_distinct"]


def load_index(cap_dir):
    path = os.path.join(cap_dir, "captures.jsonl")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def in_range(frame, rng):
    return rng[0] <= frame <= rng[1]


class Report:
    """Validation lines. A rule with `any_of = "<group>"` is one alternative of that group: the group passes
    when at least one of its rules passes (members that miss are shown as [--], not failures)."""

    def __init__(self):
        self.lines = []
        self.failed = False
        self.groups = {}

    def add(self, name, ok, detail, group=None):
        if group is not None:
            self.groups.setdefault(group, []).append(ok)
            self.lines.append(f"  [{'ok' if ok else '--'}] {name}: {detail} (one of '{group}')")
            return
        self.failed |= not ok
        self.lines.append(f"  [{'ok' if ok else 'FAIL'}] {name}: {detail}")

    def finish(self, title, path):
        for group, results in self.groups.items():
            ok = any(results)
            self.failed |= not ok
            self.lines.append(f"  [{'ok' if ok else 'FAIL'}] any_of[{group}]: {sum(results)} of {len(results)} "
                              f"alternatives passed (need >= 1)")
        text = f"{title}: {'FAIL' if self.failed else 'PASS'}\n" + "\n".join(self.lines) + "\n"
        print(text, end="")
        try:
            with open(path, "w") as f:
                f.write(text)
        except OSError:
            pass
        return 1 if self.failed else 0


def load_frames(cap_dir, report):
    """Reads the index and every captured PNG. Returns ([(frame, img, info, row)], [malformed])."""
    rows = load_index(cap_dir)
    if rows is None:
        report.add("index", False, f"no captures.jsonl in {cap_dir}")
        return [], []
    failed = [r["requested_frame"] for r in rows if not r.get("ok")]
    missing, bad, frames = [], [], []
    for r in rows:
        if not r.get("ok"):
            continue
        path = os.path.join(cap_dir, r.get("file", ""))
        if not r.get("file") or not os.path.exists(path):
            missing.append(r["requested_frame"])
            continue
        try:
            img, info = read_png(path)
        except (PngError, OSError) as e:
            bad.append(f"{r['requested_frame']}: {e}")
            continue
        frames.append((r["requested_frame"], img, info, r))
    report.add("index", bool(rows) and not failed and not missing,
               f"{len(rows)} captures recorded, {len(frames)} readable; capture_failed={failed} "
               f"missing_files={missing}")
    return frames, bad


def cmd_stats(cap_dir):
    rows = load_index(cap_dir) or []
    print(f"{'frame':>7} {'time':>7} {'size':>9} {'mean':>6} {'std':>6} {'distinct':>8} {'black':>6} {'change':>7}")
    prev = None
    for r in rows:
        if not r.get("ok"):
            print(f"{r['requested_frame']:>7}  capture failed")
            continue
        img, _ = read_png(os.path.join(cap_dir, r["file"]))
        s = frame_stats(img)
        change = float(np.abs(s["luma"] - prev).mean()) if prev is not None and prev.shape == s["luma"].shape else None
        prev = s["luma"]
        size = f"{img.shape[1]}x{img.shape[0]}"
        print(f"{r['requested_frame']:>7} {round(r['time_s'], 2):>7} {size:>9} {s['mean']:>6.1f} {s['std']:>6.1f} "
              f"{s['distinct']:>8} {s['black']:>6.3f} {'None' if change is None else f'{change:.2f}':>7}")
    return 0


def cmd_check(cap_dir, expectations_path, prev_dir=None):
    with open(expectations_path, "rb") as f:
        exp = tomllib.load(f)
    # rules_from = "<other.toml>": that file's rules apply too (scenarios that repeat another one's checks). At the
    # top level or in a menu_test.py [scenario] table (where the save scenarios put it).
    rules_from = exp.get("rules_from") or exp.get("scenario", {}).get("rules_from")
    if rules_from:
        with open(os.path.join(os.path.dirname(expectations_path), rules_from), "rb") as f:
            base = tomllib.load(f)
        for key, value in base.items():
            if isinstance(value, list):
                exp[key] = value + exp.get(key, [])
            elif key == "format" and key not in exp:
                exp[key] = value
    report = Report()
    frames, bad = load_frames(cap_dir, report)
    prev_by_label = {}
    if prev_dir:
        prev_report = Report()
        prev_frames, _ = load_frames(prev_dir, prev_report)
        if exp.get("format", {}).get("guest_coordinates"):
            prev_frames, _ = to_guest_frames(prev_frames)
        prev_by_label = {r["label"]: img for fr, img, _, r in prev_frames if r.get("label")}

    fmt = exp.get("format", {})
    want = (fmt.get("width"), fmt.get("height"))
    if fmt.get("render_scale"):
        want = (GUEST_WIDTH * fmt["render_scale"][0], GUEST_HEIGHT * fmt["render_scale"][1])
    wrong = [f"{fr}: {info['width']}x{info['height']}" for fr, _, info, _ in frames
             if want != (None, None) and (info["width"], info["height"]) != want]
    mismatch = [fr for fr, _, info, r in frames if (r.get("width"), r.get("height")) != (info["width"], info["height"])]
    report.add("format", bool(frames) and not bad and not wrong and not mismatch,
               f"expected {want[0]}x{want[1]} 8-bit RGB(A); malformed={bad} wrong_size={wrong} "
               f"index_mismatch={mismatch}")

    if fmt.get("guest_coordinates"):
        frames, not_multiple = to_guest_frames(frames)
        report.add("guest_frame", not not_multiple,
                   f"captures reduced to {GUEST_WIDTH}x{GUEST_HEIGHT} (block averages); not a whole multiple: "
                   f"{not_multiple}")

    required = exp.get("required_frames", [])
    if required:
        have = {fr for fr, *_ in frames}
        absent = [f for f in required if f not in have]
        report.add("required", not absent, f"required frames {required}; absent={absent}")

    stats = {fr: frame_stats(img) for fr, img, _, _ in frames}
    times = {fr: r.get("time_s", 0.0) for fr, _, _, r in frames}
    blank_cfg = {**DEFAULT_BLANK, **exp.get("blank", {})}
    allow_blank = [r["frames"] for r in exp.get("range", []) if r.get("allow_blank")]
    blanks = [fr for fr, s in sorted(stats.items())
              if is_blank(s, blank_cfg) and not any(in_range(fr, a) for a in allow_blank)]
    report.add("blank", not blanks,
               f"blank frames outside allowed ranges: {blanks} (blank = std <= {blank_cfg['max_std']} and "
               f"distinct <= {blank_cfg['max_distinct']})")

    for rng in exp.get("range", []):
        sel = [fr for fr in sorted(stats) if in_range(fr, rng["frames"])]
        problems = []
        for fr in sel:
            s = stats[fr]
            for key in ("mean", "std"):
                if key in rng and not rng[key][0] <= s[key] <= rng[key][1]:
                    problems.append(f"{fr}: {key} {s[key]:.1f} not in {rng[key]}")
            if "min_distinct" in rng and s["distinct"] < rng["min_distinct"]:
                problems.append(f"{fr}: distinct {s['distinct']} < {rng['min_distinct']}")
            if "max_black" in rng and s["black"] > rng["max_black"]:
                problems.append(f"{fr}: black {s['black']:.3f} > {rng['max_black']}")
        if "any_mean_above" in rng and sel and max(stats[fr]["mean"] for fr in sel) < rng["any_mean_above"]:
            problems.append(f"no capture with mean >= {rng['any_mean_above']} "
                            f"(brightest {max(stats[fr]['mean'] for fr in sel):.1f})")
        if "max_blank_seconds" in rng:
            run_start = None
            for fr in sel + [None]:
                blank = fr is not None and is_blank(stats[fr], blank_cfg)
                if blank and run_start is None:
                    run_start = fr
                elif not blank and run_start is not None:
                    run = [f for f in sel if run_start <= f and (fr is None or f < fr)]
                    span = times[run[-1]] - times[run[0]]
                    if span > rng["max_blank_seconds"]:
                        problems.append(f"blank from frame {run[0]} to {run[-1]} ({span:.1f} s > "
                                        f"{rng['max_blank_seconds']} s)")
                    run_start = None
        ok = (bool(sel) or rng.get("optional", False)) and not problems
        report.add(f"stats[{rng['name']}]", ok,
                   f"frames {rng['frames']}: {len(sel)} checked" + (f"; {problems[:6]}" if problems else "")
                   + ("" if sel else "; no captures in range"))

    for anim in exp.get("animation", []):
        sel = [fr for fr in sorted(stats) if in_range(fr, anim["frames"])]
        frozen, changes = [], []
        for a, b in zip(sel, sel[1:]):
            if stats[a]["luma"].shape != stats[b]["luma"].shape:
                continue
            if anim.get("ignore_blank_pairs") and is_blank(stats[a], blank_cfg) and is_blank(stats[b], blank_cfg):
                continue
            c = float(np.abs(stats[b]["luma"] - stats[a]["luma"]).mean())
            changes.append(c)
            if c < anim["min_change"]:
                frozen.append(f"{a}->{b}: {c:.2f}")
        need = anim.get("min_captures", 2)
        enough = len(sel) >= need
        report.add(f"animation[{anim['name']}]", enough and not frozen,
                   f"{len(sel)} captures, {len(changes)} pairs, min change "
                   f"{min(changes) if changes else float('nan'):.2f} (need >= {anim['min_change']})"
                   + (f"; frozen pairs {frozen[:6]}" if frozen else "")
                   + ("" if enough else f"; need >= {need} captures"))

    by_frame = {fr: img for fr, img, _, _ in frames}
    by_label = {r["label"]: img for fr, img, _, r in frames if r.get("label")}

    def pick(ref):
        if isinstance(ref, str) and ref.startswith("prev:"):  # a labelled capture of the previous run
            return prev_by_label.get(ref[5:])
        return by_label.get(ref) if isinstance(ref, str) and not ref.isdigit() else by_frame.get(int(ref))

    for rg in exp.get("region", []):
        img = pick(rg["capture"])
        if img is None:
            report.add(f"region[{rg['name']}]", False, f"no capture {rg['capture']!r}")
            continue
        v = region_values(img, rg["rect"], rg.get("metric", "luma"))
        mean, std = float(v.mean()), float(v.std())
        problems = []
        if "mean" in rg and not rg["mean"][0] <= mean <= rg["mean"][1]:
            problems.append(f"mean {mean:.1f} not in {rg['mean']}")
        if "min_std" in rg and std < rg["min_std"]:
            problems.append(f"std {std:.1f} < {rg['min_std']}")
        if "max_std" in rg and std > rg["max_std"]:
            problems.append(f"std {std:.1f} > {rg['max_std']}")
        report.add(f"region[{rg['name']}]", not problems,
                   f"{rg['capture']} {rg['rect']}: mean {mean:.1f}, std {std:.1f}" + (f"; {problems}" if problems else ""),
                   rg.get("any_of"))

    for rd in exp.get("region_diff", []):
        a, b = pick(rd["a"]), pick(rd["b"])
        if a is None or b is None:
            report.add(f"region_diff[{rd['name']}]", False, f"missing capture {rd['a']!r} or {rd['b']!r}")
            continue
        metric, thr, delta = rd.get("metric", "luma"), rd.get("text_threshold", 150.0), rd.get("text_delta", 40.0)
        va, vb = region_values(a, rd["rect"], metric, thr, delta), region_values(b, rd["rect"], metric, thr, delta)
        glyphs = max(int((va > 0).sum()), int((vb > 0).sum()))
        if (metric in ("text", "text_rel") and "max_mean_diff" in rd and not rd.get("allow_empty")
                and glyphs < rd.get("min_glyph_px", 20)):
            # "Unchanged" between two empty rectangles proves nothing (text dimmer than the threshold, or a
            # wrong rectangle): fail unless the rule says the slot may be empty.
            report.add(f"region_diff[{rd['name']}]", False,
                       f"{rd['a']} -> {rd['b']} {rd['rect']}: no text in the rectangle ({glyphs} glyph pixels; "
                       f"allow_empty = true if the slot may be empty)", rd.get("any_of"))
            continue
        if rd.get("align_x") == "right":
            va, vb = align_right(va, rd.get("keep_px")), align_right(vb, rd.get("keep_px"))
        elif rd.get("align_x"):
            va, vb = align_left(va), align_left(vb)
        d = float(np.abs(va - vb).mean())
        ok = d >= rd.get("min_mean_diff", -1) and d <= rd.get("max_mean_diff", 1e9)
        report.add(f"region_diff[{rd['name']}]", ok,
                   f"{rd['a']} -> {rd['b']} {rd['rect']}: mean difference {d:.2f} "
                   f"(need >= {rd.get('min_mean_diff', '-')}, <= {rd.get('max_mean_diff', '-')})", rd.get("any_of"))

    for sh in exp.get("shift", []):
        a, b = pick(sh["a"]), pick(sh["b"])
        if a is None or b is None:
            report.add(f"shift[{sh['name']}]", False, f"missing capture {sh['a']!r} or {sh['b']!r}")
            continue
        # Default band: scenery above the player character and left of the objective marker, both of which
        # stay put on screen while the camera turns.
        px, estimates = robust_horizontal_shift(a, b, sh.get("rect", [0, 0, 960, 240]))
        want = sh["direction"]  # "left" or "right": where the image content moved
        ok = (px <= -sh["min_px"]) if want == "left" else (px >= sh["min_px"])
        report.add(f"shift[{sh['name']}]", ok,
                   f"{sh['a']} -> {sh['b']}: content moved {px:+d} px, median of {estimates} "
                   f"(need {want} by >= {sh['min_px']})")

    for hl in exp.get("highlight", []):
        problems, checked = [], 0
        for frame_key, want in sorted(hl.get("expect", {}).items(),
                                      key=lambda kv: (not kv[0].isdigit(), int(kv[0]) if kv[0].isdigit() else kv[0])):
            fr = frame_key
            img = pick(frame_key)
            if img is None:
                problems.append(f"{fr}: no capture")
                continue
            scores = highlight_scores(img, hl)
            order = sorted(range(len(scores)), key=lambda i: -scores[i])
            got, margin = order[0], scores[order[0]] - scores[order[1]]
            checked += 1
            if got != want or margin < hl.get("min_margin", 8.0):
                problems.append(f"{fr}: selected row {got} (margin {margin:.1f}), expected {want}; "
                                f"scores {[round(x, 1) for x in scores]}")
        report.add(f"highlight[{hl['name']}]", not problems,
                   f"{checked} frames checked" + (f"; {problems[:4]}" if problems else ""))

    return report.finish(f"CAPTURE CHECK ({os.path.basename(expectations_path)})",
                         os.path.join(cap_dir, "capture-report.txt"))


def _inside_repo_outside_build(path):
    real = os.path.realpath(path)
    root = os.path.realpath(ROOT)
    return (real == root or real.startswith(root + os.sep)) and \
        not real.startswith(os.path.join(root, "build") + os.sep)


def cmd_baseline(cap_dir, ref_dir):
    if _inside_repo_outside_build(ref_dir):
        raise SystemExit(f"refusing to store game captures inside the repository outside build/: {ref_dir}")
    rows = [r for r in (load_index(cap_dir) or []) if r.get("ok")]
    if not rows:
        raise SystemExit(f"no captures in {cap_dir}")
    for r in rows:
        read_png(os.path.join(cap_dir, r["file"]))  # never store a malformed reference
    os.makedirs(ref_dir, exist_ok=True)
    for r in rows:
        shutil.copyfile(os.path.join(cap_dir, r["file"]), os.path.join(ref_dir, r["file"]))
    with open(os.path.join(ref_dir, "captures.jsonl"), "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    print(f"stored {len(rows)} reference captures in {ref_dir} (local only, never commit)")
    return 0


def block_average(img, block):
    h, w = img.shape[0] // block * block, img.shape[1] // block * block
    a = img[:h, :w].astype(np.float32).reshape(h // block, block, w // block, block, 3)
    return a.mean(axis=(1, 3))


def compare_images(img, ref, cfg):
    """Returns (ok, detail). Tolerant to small per-pixel differences, sensitive to visible changes."""
    if img.shape != ref.shape:
        return False, f"size {img.shape[1]}x{img.shape[0]} != reference {ref.shape[1]}x{ref.shape[0]}"
    diff = np.abs(block_average(img, cfg["block"]) - block_average(ref, cfg["block"])).max(axis=2)
    keep = np.ones(diff.shape, dtype=bool)
    for x0, y0, x1, y1 in cfg.get("ignore", []):
        b = cfg["block"]
        keep[y0 // b:(y1 + b - 1) // b, x0 // b:(x1 + b - 1) // b] = False
    diff = diff[keep]
    mean = float(diff.mean())
    frac = float((diff > cfg["block_diff"]).mean())
    ok = mean <= cfg["max_mean_diff"] and frac <= cfg["max_block_fraction"]
    return ok, (f"mean block diff {mean:.2f} (<= {cfg['max_mean_diff']}), blocks differing > {cfg['block_diff']}: "
                f"{frac:.4f} (<= {cfg['max_block_fraction']})")


def cmd_compare(cap_dir, ref_dir, expectations_path=None):
    cfg = dict(DEFAULT_COMPARE)
    if expectations_path:
        with open(expectations_path, "rb") as f:
            cfg.update(tomllib.load(f).get("compare", {}))
    report = Report()
    refs = {r["requested_frame"]: r for r in (load_index(ref_dir) or []) if r.get("ok")}
    if not refs:
        report.add("references", False, f"no reference captures in {ref_dir} (create them with `baseline`)")
        return report.finish("CAPTURE COMPARE", os.path.join(cap_dir, "compare-report.txt"))
    frames, bad = load_frames(cap_dir, report)
    if bad:
        report.add("format", False, f"malformed captures {bad}")
    compared = 0
    for fr, img, _, _ in frames:
        if fr not in refs:
            continue
        try:
            ref, _ = read_png(os.path.join(ref_dir, refs[fr]["file"]))
        except (PngError, OSError) as e:
            report.add(f"frame {fr}", False, f"reference unreadable: {e}")
            continue
        ok, detail = compare_images(img, ref, cfg)
        report.add(f"frame {fr}", ok, detail)
        compared += 1
    no_capture = sorted(set(refs) - {fr for fr, *_ in frames})
    report.add("coverage", compared > 0 and not no_capture,
               f"{compared} frames compared; references without a capture: {no_capture}")
    return report.finish("CAPTURE COMPARE", os.path.join(cap_dir, "compare-report.txt"))


def main(argv):
    usage = __doc__.split("\n\n")[1]
    if len(argv) < 3:
        raise SystemExit(usage)
    mode, cap_dir = argv[1], argv[2]
    if mode == "stats" and len(argv) == 3:
        return cmd_stats(cap_dir)
    if mode == "check" and len(argv) in (4, 5):
        return cmd_check(cap_dir, argv[3], argv[4] if len(argv) == 5 else None)
    if mode == "baseline" and len(argv) == 4:
        return cmd_baseline(cap_dir, argv[3])
    if mode == "compare" and len(argv) in (4, 5):
        return cmd_compare(cap_dir, argv[3], argv[4] if len(argv) == 5 else None)
    raise SystemExit(usage)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
