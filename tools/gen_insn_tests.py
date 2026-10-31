#!/usr/bin/env python3
"""Generate PPC instruction tests for instructions the game uses heavily that the SDK suite does not cover
(see tools/insn_test_coverage.py). Expected values are computed here, independently of ReXGlue, with
numpy's IEEE float32 arithmetic and exact rational arithmetic, from the PowerPC/VMX definitions.

Usage: gen_insn_tests.py <out_dir>   (writes instr_kuf_fp.s and instr_kuf_vmx.s)

Format: ReXGlue's recompile-tests directives (#_ REGISTER_IN/OUT, MEMORY_IN/OUT). A float register value
containing '.' is a C++ double literal (hex floats keep it exact); anything else is the raw 64-bit pattern.
"""
import os
import struct
import sys
from fractions import Fraction

import numpy as np

out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)


def f32(x):
    return np.float32(x)


def f32_bits(x):
    return struct.unpack(">I", struct.pack(">f", float(np.float32(x))))[0]


def bits_f32(b):
    return np.frombuffer(struct.pack(">I", b), dtype=">f4")[0]


def round_single(fr):
    """Round an exact Fraction to the nearest float32 (ties to even). Valid when fr is exactly
    representable as a double, which every case below guarantees (asserted)."""
    d = float(fr)
    assert Fraction(d) == fr, "case must be exact in double"
    return np.float32(d)


def lit(x):
    """C++ double literal for a float32/float64 value, exact."""
    x = float(x)
    if x != x:
        return "std::numeric_limits<double>::quiet_NaN()"
    if x in (float("inf"), float("-inf")):
        return ("-" if x < 0 else "") + "std::numeric_limits<double>::infinity()"
    return x.hex()


def vec(words):
    return "[" + ", ".join(f"{w:08X}" for w in words) + "]"


tests = []


def test(name, lines):
    tests.append((name, lines))


# ---------------------------------------------------------------- scalar single precision
one_ulp = 1 + 2.0 ** -23  # 1 + ulp(1) in float32
cases = []
# fmuls: (1+2^-23)^2 = 1 + 2^-22 + 2^-46 exactly; single rounding drops 2^-46.
a = Fraction(1) + Fraction(1, 2 ** 23)
cases.append(("fmuls", "fmuls f3, f1, f2", [("f1", one_ulp), ("f2", one_ulp)], ("f3", round_single(a * a))))
cases.append(("fmuls", "fmuls f3, f1, f2", [("f1", -1.5), ("f2", 2.5)], ("f3", f32(-3.75))))
# fadds / fsubs: 1 +- 2^-30 rounds back to 1.0 in single (a double result would keep the bit).
cases.append(("fadds", "fadds f3, f1, f2", [("f1", 1.0), ("f2", 2.0 ** -30)], ("f3", round_single(Fraction(1) + Fraction(1, 2 ** 30)))))
cases.append(("fadds", "fadds f3, f1, f2", [("f1", 0.25), ("f2", -0.75)], ("f3", f32(-0.5))))
cases.append(("fsubs", "fsubs f3, f1, f2", [("f1", 1.0), ("f2", 2.0 ** -30)], ("f3", round_single(Fraction(1) - Fraction(1, 2 ** 30)))))
cases.append(("fsubs", "fsubs f3, f1, f2", [("f1", 3.0), ("f2", 5.5)], ("f3", f32(-2.5))))
# fdivs / fsqrts: correctly rounded single results (double-then-single is exact for these ops).
cases.append(("fdivs", "fdivs f3, f1, f2", [("f1", 1.0), ("f2", 3.0)], ("f3", f32(1.0) / f32(3.0))))
cases.append(("fdivs", "fdivs f3, f1, f2", [("f1", -7.0), ("f2", 2.0)], ("f3", f32(-3.5))))
cases.append(("fsqrts", "fsqrts f3, f1", [("f1", 2.0)], ("f3", np.sqrt(f32(2.0)))))
cases.append(("fsqrts", "fsqrts f3, f1", [("f1", 6.25)], ("f3", f32(2.5))))
# fmsubs: f3 = f1*f2 - f4 ; fnmsubs: f3 = -(f1*f2 - f4)
cases.append(("fmsubs", "fmsubs f3, f1, f2, f4", [("f1", 1.5), ("f2", 2.5), ("f4", 0.25)], ("f3", f32(3.5))))
cases.append(("fmsubs", "fmsubs f3, f1, f2, f4", [("f1", one_ulp), ("f2", one_ulp), ("f4", 1.0)],
              ("f3", round_single(a * a - 1))))
cases.append(("fnmsubs", "fnmsubs f3, f1, f2, f4", [("f1", 1.5), ("f2", 2.5), ("f4", 0.25)], ("f3", f32(-3.5))))
cases.append(("fnmsubs", "fnmsubs f3, f1, f2, f4", [("f1", 2.0), ("f2", 3.0), ("f4", 10.0)], ("f3", f32(4.0))))
# frsp: double -> single, round half to even.
cases.append(("frsp", "frsp f3, f1", [("f1", 1 + 2.0 ** -24)], ("f3", f32(1.0))))  # tie -> even (down)
cases.append(("frsp", "frsp f3, f1", [("f1", 1 + 3 * 2.0 ** -24)], ("f3", f32(1 + 2.0 ** -22))))  # tie -> even (up)
cases.append(("frsp", "frsp f3, f1", [("f1", 1 + 2.0 ** -24 + 2.0 ** -40)], ("f3", f32(one_ulp))))  # above tie
cases.append(("frsp", "frsp f3, f1", [("f1", -0.1)], ("f3", f32(-0.1))))
for i, (op, insn, ins, out) in enumerate(cases):
    lines = [f"  #_ REGISTER_IN {r} {lit(v)}" for r, v in ins]
    lines += [f"  {insn}", "  blr"]
    lines += [f"  #_ REGISTER_OUT {out[0]} {lit(out[1])}"]
    test(f"test_kuf_{op}_{i}", lines)

# fneg / fmr: bit-exact, including signed zero (raw bit patterns).
test("test_kuf_fneg_zero", ["  #_ REGISTER_IN f1 0x0000000000000000", "  fneg f3, f1", "  blr",
                            "  #_ REGISTER_OUT f3 0x8000000000000000"])
test("test_kuf_fneg_nan", ["  #_ REGISTER_IN f1 0x7FF8000000000001", "  fneg f3, f1", "  blr",
                           "  #_ REGISTER_OUT f3 0xFFF8000000000001"])
test("test_kuf_fmr_bits", ["  #_ REGISTER_IN f1 0x800FFFFFFFFFFFFF", "  fmr f3, f1", "  blr",
                           "  #_ REGISTER_OUT f3 0x800FFFFFFFFFFFFF"])
# fcfid: int64 bits in an FPR -> double; 2^53+1 rounds to even.
test("test_kuf_fcfid_neg", ["  #_ REGISTER_IN f1 0xFFFFFFFFFFFFFFFB", "  fcfid f3, f1", "  blr",
                            f"  #_ REGISTER_OUT f3 {lit(-5.0)}"])
test("test_kuf_fcfid_round", ["  #_ REGISTER_IN f1 0x0020000000000001", "  fcfid f3, f1", "  blr",
                              f"  #_ REGISTER_OUT f3 {lit(float(2 ** 53))}"])
# fcmpu into cr6: LT=0x80 GT=0x40 EQ=0x20 UN=0x10 (bits 24..27 of CR, read with mfcr).
for name, x, y, crbits in (("lt", 1.0, 2.0, 0x80), ("gt", 2.0, 1.0, 0x40), ("eq_signed_zero", 0.0, -0.0, 0x20)):
    test(f"test_kuf_fcmpu_{name}", [f"  #_ REGISTER_IN f1 {lit(x)}", f"  #_ REGISTER_IN f2 {lit(y)}",
                                    "  fcmpu cr6, f1, f2", "  mfcr r12", "  blr",
                                    f"  #_ REGISTER_OUT r12 0x{crbits:08X}"])
test("test_kuf_fcmpu_unordered", ["  #_ REGISTER_IN f1 0x7FF8000000000000", f"  #_ REGISTER_IN f2 {lit(1.0)}",
                                  "  fcmpu cr6, f1, f2", "  mfcr r12", "  blr",
                                  "  #_ REGISTER_OUT r12 0x00000010"])
# stfiwx: stores the low word of the FPR, big-endian.
test("test_kuf_stfiwx", ["  #_ MEMORY_IN 10001000 00 00 00 00 11 11", "  #_ REGISTER_IN f1 0x12345678DEADBEEF",
                         "  #_ REGISTER_IN r4 0x10001000", "  stfiwx f1, 0, r4", "  blr",
                         "  #_ MEMORY_OUT 10001000 de ad be ef 11 11"])

# ---------------------------------------------------------------- remaining scalar instructions
# double precision: results must NOT be rounded to single
test("test_kuf_fsub_double", [f"  #_ REGISTER_IN f1 {lit(1.0)}", f"  #_ REGISTER_IN f2 {lit(2.0 ** -30)}",
                              "  fsub f3, f1, f2", "  blr", f"  #_ REGISTER_OUT f3 {lit(1.0 - 2.0 ** -30)}"])
test("test_kuf_fdiv_double", [f"  #_ REGISTER_IN f1 {lit(1.0)}", f"  #_ REGISTER_IN f2 {lit(3.0)}",
                              "  fdiv f3, f1, f2", "  blr", f"  #_ REGISTER_OUT f3 {lit(1.0 / 3.0)}"])
test("test_kuf_fmsub_double", [f"  #_ REGISTER_IN f1 {lit(1.5)}", f"  #_ REGISTER_IN f2 {lit(2.5)}",
                               f"  #_ REGISTER_IN f4 {lit(0.25)}", "  fmsub f3, f1, f2, f4", "  blr",
                               f"  #_ REGISTER_OUT f3 {lit(3.5)}"])
test("test_kuf_fnmsub_double", [f"  #_ REGISTER_IN f1 {lit(1.5)}", f"  #_ REGISTER_IN f2 {lit(2.5)}",
                                f"  #_ REGISTER_IN f4 {lit(0.25)}", "  fnmsub f3, f1, f2, f4", "  blr",
                                f"  #_ REGISTER_OUT f3 {lit(-3.5)}"])
test("test_kuf_fnmadds", [f"  #_ REGISTER_IN f1 {lit(1.5)}", f"  #_ REGISTER_IN f2 {lit(2.5)}",
                          f"  #_ REGISTER_IN f4 {lit(0.25)}", "  fnmadds f3, f1, f2, f4", "  blr",
                          f"  #_ REGISTER_OUT f3 {lit(-4.0)}"])
# fctid: double -> int64 with the FPSCR rounding mode (default: nearest, ties to even); saturates.
for i, (x, want) in enumerate(((2.5, 2), (-1.5, -2), (3.5, 4), (1e20, 2 ** 63 - 1), (-1e20, -2 ** 63))):
    test(f"test_kuf_fctid_{i}", [f"  #_ REGISTER_IN f1 {lit(x)}", "  fctid f3, f1", "  blr",
                                 f"  #_ REGISTER_OUT f3 0x{want & (2 ** 64 - 1):016X}"])
# mtfsf: rounding mode toward zero (RN=01) changes fctid; the mode is restored before returning.
test("test_kuf_mtfsf_rounding", ["  #_ REGISTER_IN f1 0x0000000000000001", "  #_ REGISTER_IN f4 0x0000000000000000",
                                 f"  #_ REGISTER_IN f2 {lit(2.75)}", "  mtfsf 255, f1", "  fctid f3, f2",
                                 "  mtfsf 255, f4", "  blr", "  #_ REGISTER_OUT f3 0x0000000000000002"])
# integer logic
test("test_kuf_xori", ["  #_ REGISTER_IN r4 0x12345678", "  xori r3, r4, 0xFFFF", "  blr",
                       "  #_ REGISTER_OUT r3 0x1234A987"])
test("test_kuf_xoris", ["  #_ REGISTER_IN r4 0x12345678", "  xoris r3, r4, 0xFFFF", "  blr",
                        "  #_ REGISTER_OUT r3 0xEDCB5678"])
test("test_kuf_orc", ["  #_ REGISTER_IN r4 0x0F0F", "  #_ REGISTER_IN r5 0x00FF", "  orc r3, r4, r5", "  blr",
                      "  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFF0F"])
test("test_kuf_nand", ["  #_ REGISTER_IN r4 0xFF00", "  #_ REGISTER_IN r5 0x0FF0", "  nand r3, r4, r5", "  blr",
                       "  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFF0FF"])
test("test_kuf_rldimi", ["  #_ REGISTER_IN r3 0x1111111111111111", "  #_ REGISTER_IN r4 0xAB",
                         "  rldimi r3, r4, 8, 48", "  blr", "  #_ REGISTER_OUT r3 0x111111111111AB11"])
# condition register logic (cr6: LT=24 GT=25 EQ=26 SO=27)
test("test_kuf_cror", ["  #_ REGISTER_IN cr 0x00000020", "  cror 0, 25, 26", "  blr",
                       "  #_ REGISTER_OUT cr 0x80000020"])
test("test_kuf_crorc_set", ["  #_ REGISTER_IN cr 0x00000000", "  crorc 0, 25, 26", "  blr",
                            "  #_ REGISTER_OUT cr 0x80000000"])
test("test_kuf_crorc_clear", ["  #_ REGISTER_IN cr 0x00000020", "  crorc 0, 25, 26", "  blr",
                              "  #_ REGISTER_OUT cr 0x00000020"])
test("test_kuf_mtcr", ["  #_ REGISTER_IN r5 0x12345678", "  mtcr r5", "  blr", "  #_ REGISTER_OUT cr 0x12345678"])
# mtxer feeds the carry: XER[CA] = 1, then addze adds it.
test("test_kuf_mtxer_carry", ["  #_ REGISTER_IN r6 0x20000000", "  #_ REGISTER_IN r5 10", "  mtxer r6",
                              "  addze r4, r5", "  blr", "  #_ REGISTER_OUT r4 11"])
# dcbzl (Xenon 128-byte cache line zero): game word 0x82A56BE8 dcbzl r0,r10. Zeroes the aligned 128 bytes
# containing r10, nothing outside.
cc = " ".join(["CCCCCCCC"] * 34)
zero_line = " ".join(["CCCCCCCC"] + ["00000000"] * 32 + ["CCCCCCCC"])
test("test_kuf_dcbzl", [f"  #_ MEMORY_IN 10001FFC {cc}", "  #_ REGISTER_IN r10 0x10002044",
                        "  # tests: dcbzl", "  .long 0x7C2057EC", "  blr", f"  #_ MEMORY_OUT 10001FFC {zero_line}"])

with open(os.path.join(out_dir, "instr_kuf_fp.s"), "w") as f:
    f.write("# Generated by tools/gen_insn_tests.py -- do not edit.\n\n")
    for name, lines in tests:
        f.write(f"{name}:\n" + "\n".join(lines) + "\n\n")

# ---------------------------------------------------------------- VMX / VMX128
tests.clear()


def vf(vals):
    return [f32_bits(v) for v in vals]


def vtest(name, insn, ins, out, extra_out=()):
    lines = [f"  #_ REGISTER_IN {r} {vec(w)}" for r, w in ins]
    lines += [f"  {i}" for i in (insn if isinstance(insn, list) else [insn])] + ["  blr"]
    lines += [f"  #_ REGISTER_OUT {out[0]} {vec(out[1])}"]
    lines += [f"  #_ REGISTER_OUT {r} {v}" for r, v in extra_out]
    test(name, lines)


A = [1.5, -3.0, 1e10, one_ulp]
Bv = [2.0, 0.5, 1e10, one_ulp]
vtest("test_kuf_vmulfp128", "vmulfp128 v3, v4, v5", [("v4", vf(A)), ("v5", vf(Bv))],
      ("v3", [f32_bits(f32(x) * f32(y)) for x, y in zip(A, Bv)]))
# vnmsubfp vD, vA, vC, vB = -(vA*vC - vB)
Av, Cv, Bb = [1.5, 2.0, -4.0, 0.5], [2.5, 3.0, 0.25, 8.0], [0.25, 10.0, 1.0, -2.0]
vtest("test_kuf_vnmsubfp", "vnmsubfp v3, v4, v5, v6", [("v4", vf(Av)), ("v5", vf(Cv)), ("v6", vf(Bb))],
      ("v3", [f32_bits(round_single(-(Fraction(a) * Fraction(c) - Fraction(b)))) for a, c, b in zip(Av, Cv, Bb)]))
# vrsqrtefp: only the exactly-defined special cases (the estimate's precision is implementation-defined).
vtest("test_kuf_vrsqrtefp_special", "vrsqrtefp v3, v4",
      [("v4", [0x7F800000, 0x00000000, 0x80000000, 0x7F800000])],
      ("v3", [0x00000000, 0x7F800000, 0xFF800000, 0x00000000]))
# vcmpgtfp. with cr6 (mfocrf r3, 2 reads cr6 into bits 4..7): all true 0x80, all false 0x20, mixed 0.
gt_a, gt_b = [2.0, 1.0, float("nan"), 1.0], [1.0, 2.0, 1.0, float("-inf")]
vtest("test_kuf_vcmpgtfp_mixed", ["vcmpgtfp. v3, v4, v5", "mfocrf r3, 2"], [("v4", vf(gt_a)), ("v5", vf(gt_b))],
      ("v3", [0xFFFFFFFF if x > y else 0 for x, y in zip(gt_a, gt_b)]), [("r3", "0x00000000")])
vtest("test_kuf_vcmpgtfp_all", ["vcmpgtfp. v3, v4, v5", "mfocrf r3, 2"], [("v4", vf([2.0] * 4)), ("v5", vf([1.0] * 4))],
      ("v3", [0xFFFFFFFF] * 4), [("r3", "0x00000080")])
vtest("test_kuf_vcmpgtfp_none", ["vcmpgtfp. v3, v4, v5", "mfocrf r3, 2"], [("v4", vf([1.0] * 4)), ("v5", vf([1.0] * 4))],
      ("v3", [0] * 4), [("r3", "0x00000020")])
# vcsxwfp128 vD, vB, uimm: signed int / 2^uimm, rounded to single.
ints = [8, -8 & 0xFFFFFFFF, 1, 0x7FFFFFFF]
vtest("test_kuf_vcsxwfp128", "vcsxwfp128 v3, v4, 2", [("v4", ints)],
      ("v3", [f32_bits(round_single(Fraction(struct.unpack('>i', struct.pack('>I', w))[0], 4))) for w in ints]))
# vcfux vD, vB, uimm: unsigned int / 2^uimm.
uints = [0xFFFFFFFF, 1, 0x80000000, 0]
vtest("test_kuf_vcfux", "vcfux v3, v4, 0", [("v4", uints)],
      ("v3", [f32_bits(round_single(Fraction(w))) for w in uints]))
vtest("test_kuf_vcfux_scaled", "vcfux v3, v4, 3", [("v4", [8, 12, 1, 0x80000000])],
      ("v3", [f32_bits(round_single(Fraction(w, 8))) for w in [8, 12, 1, 0x80000000]]))
# integer lanes
x, y = [0xFFFFFFFF, 0x7FFFFFFF, 0x12345678, 0], [1, 1, 0x11111111, 0xFFFFFFFF]
vtest("test_kuf_vadduwm", "vadduwm v3, v4, v5", [("v4", x), ("v5", y)], ("v3", [(p + q) & 0xFFFFFFFF for p, q in zip(x, y)]))


def halves(w):
    return [(w >> 16) & 0xFFFF, w & 0xFFFF]


def from_halves(h):
    return (h[0] << 16) | h[1]


x, y = [0xFFFF0001, 0x80007FFF, 0x00000000, 0x12341234], [0x00010001, 0x7FFF8000, 0x00000001, 0x12351233]
vtest("test_kuf_vcmpgtuh", "vcmpgtuh v3, v4, v5", [("v4", x), ("v5", y)],
      ("v3", [from_halves([0xFFFF if p > q else 0 for p, q in zip(halves(a), halves(b))]) for a, b in zip(x, y)]))


def bytes_of(w):
    return list(struct.pack(">I", w))


x, y = [0xFF000102, 0xFE7F8081, 0x00000000, 0x01020304], [0x01010101, 0xFF808080, 0x01FF00FF, 0x04030201]
vtest("test_kuf_vavgub", "vavgub v3, v4, v5", [("v4", x), ("v5", y)],
      ("v3", [struct.unpack(">I", bytes((p + q + 1) >> 1 for p, q in zip(bytes_of(a), bytes_of(b))))[0]
              for a, b in zip(x, y)]))

# ---------------------------------------------------------------- remaining vector instructions the game uses
# VMX128 operand conventions follow Xenia's ppc_emit_altivec.cc (the public reference implementation):
# vsel128 uses VD as the mask, vmaddfp128 VD=VA*VB+VD, vmaddcfp128 VD=VA*VD+VB, vnmsubfp128 VD=-(VA*VB-VD),
# vperm128's control vector is one of v0..v7, vcfpuxws128 is vctuxs. Assembly spelling matches the game's
# own instructions (VD repeated as the last operand). Where the SDK's assembler rejects a form the game
# uses (vperm128 with vC > v3, vspltisw128), the test embeds the game's exact instruction word.


def s32(w):
    return struct.unpack(">i", struct.pack(">I", w & 0xFFFFFFFF))[0]


def lanes(w, size):
    """Split a 32-bit word into big-endian lanes of `size` bytes."""
    n = 4 // size
    return [(w >> (8 * size * (n - 1 - i))) & ((1 << (8 * size)) - 1) for i in range(n)]


def join(ls, size):
    w = 0
    for v in ls:
        w = (w << (8 * size)) | (v & ((1 << (8 * size)) - 1))
    return w


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def lanewise(x, y, size, fn):
    return [join([fn(p, q) for p, q in zip(lanes(a, size), lanes(b, size))], size) for a, b in zip(x, y)]


def sat(v, lo, hi):
    return max(lo, min(hi, v))


X = [0xFFFF7FFF, 0x80000001, 0x7FFFFFFF, 0x00FF8001]
Y = [0x0001FFFF, 0x80000001, 0x00000001, 0xFF017FFF]
mask = lambda c: lambda p, q: (1 << 32) - 1 if c(p, q) else 0
int_cases = [
    ("vcmpequw", 4, lambda p, q: 0xFFFFFFFF if p == q else 0),
    ("vcmpequh", 2, lambda p, q: 0xFFFF if p == q else 0),
    ("vcmpequb", 1, lambda p, q: 0xFF if p == q else 0),
    ("vcmpgtsw", 4, lambda p, q: 0xFFFFFFFF if sx(p, 32) > sx(q, 32) else 0),
    ("vcmpgtsh", 2, lambda p, q: 0xFFFF if sx(p, 16) > sx(q, 16) else 0),
    ("vcmpgtub", 1, lambda p, q: 0xFF if p > q else 0),
    ("vaddubm", 1, lambda p, q: (p + q) & 0xFF),
    ("vsububm", 1, lambda p, q: (p - q) & 0xFF),
    ("vaddubs", 1, lambda p, q: sat(p + q, 0, 0xFF)),
    ("vaddsbs", 1, lambda p, q: sat(sx(p, 8) + sx(q, 8), -128, 127)),
    ("vadduws", 4, lambda p, q: sat(p + q, 0, 0xFFFFFFFF)),
    ("vaddsws", 4, lambda p, q: sat(sx(p, 32) + sx(q, 32), -2 ** 31, 2 ** 31 - 1)),
    ("vsubsws", 4, lambda p, q: sat(sx(p, 32) - sx(q, 32), -2 ** 31, 2 ** 31 - 1)),
    ("vmaxsw", 4, lambda p, q: p if sx(p, 32) > sx(q, 32) else q),
    ("vavgsb", 1, lambda p, q: (sx(p, 8) + sx(q, 8) + 1) >> 1),
    ("vnor", 4, lambda p, q: ~(p | q) & 0xFFFFFFFF),
    ("vcmpequw128", 4, lambda p, q: 0xFFFFFFFF if p == q else 0),
]
for name, size, fn in int_cases:
    vtest(f"test_kuf_{name}", f"{name} v3, v4, v5", [("v4", X), ("v5", Y)], ("v3", lanewise(X, Y, size, fn)))

SH = [0x00000004, 0x0000001F, 0x00000020, 0x03030303]  # only the low 5 (word) / 3 (byte) bits count
vtest("test_kuf_vsrw", "vsrw v3, v4, v5", [("v4", X), ("v5", SH)],
      ("v3", [(a >> (b & 31)) for a, b in zip(X, SH)]))
vtest("test_kuf_vsraw", "vsraw v3, v4, v5", [("v4", X), ("v5", SH)],
      ("v3", [(sx(a, 32) >> (b & 31)) & 0xFFFFFFFF for a, b in zip(X, SH)]))
vtest("test_kuf_vslw128", "vslw128 v3, v4, v5", [("v4", X), ("v5", SH)],
      ("v3", [(a << (b & 31)) & 0xFFFFFFFF for a, b in zip(X, SH)]))
vtest("test_kuf_vsrw128", "vsrw128 v3, v4, v5", [("v4", X), ("v5", SH)],
      ("v3", [(a >> (b & 31)) for a, b in zip(X, SH)]))
vtest("test_kuf_vsrab", "vsrab v3, v4, v5", [("v4", X), ("v5", SH)],
      ("v3", lanewise(X, SH, 1, lambda p, q: (sx(p, 8) >> (q & 7)) & 0xFF)))

# VMX128 register-reusing forms
M = [0xFFFFFFFF, 0x00000000, 0x0F0F0F0F, 0xFF00FF00]
P, Q = [0x11111111, 0x22222222, 0x33333333, 0x44444444], [0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD]
vtest("test_kuf_vsel128", "vsel128 v3, v4, v5, v3", [("v3", M), ("v4", P), ("v5", Q)],
      ("v3", [(a & ~m | b & m) & 0xFFFFFFFF for a, b, m in zip(P, Q, M)]))
fa, fb, fd = [1.5, 2.0, -3.0, 0.5], [2.0, 0.25, 4.0, -8.0], [0.5, 1.0, 2.0, 3.0]
vtest("test_kuf_vmaddfp128", "vmaddfp128 v3, v4, v5, v3", [("v3", vf(fd)), ("v4", vf(fa)), ("v5", vf(fb))],
      ("v3", [f32_bits(round_single(Fraction(a) * Fraction(b) + Fraction(d))) for a, b, d in zip(fa, fb, fd)]))
vtest("test_kuf_vmaddcfp128", "vmaddcfp128 v3, v4, v3, v5", [("v3", vf(fd)), ("v4", vf(fa)), ("v5", vf(fb))],
      ("v3", [f32_bits(round_single(Fraction(a) * Fraction(d) + Fraction(b))) for a, b, d in zip(fa, fb, fd)]))
vtest("test_kuf_vnmsubfp128", "vnmsubfp128 v3, v4, v5, v3", [("v3", vf(fd)), ("v4", vf(fa)), ("v5", vf(fb))],
      ("v3", [f32_bits(round_single(-(Fraction(a) * Fraction(b) - Fraction(d)))) for a, b, d in zip(fa, fb, fd)]))

# permutes, splats, merges
A16 = [0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F]
B16 = [0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F]
ctl = [0x1F001E01, 0x02100311, 0x0F0E0D0C, 0x08180919]
cat = bytes().join(struct.pack(">I", w) for w in A16 + B16)
perm = [struct.unpack(">I", bytes(cat[b & 0x1F] for b in struct.pack(">I", c)))[0] for c in ctl]
vtest("test_kuf_vperm128", "vperm128 v3, v4, v5, v2", [("v4", A16), ("v5", B16), ("v2", ctl)], ("v3", perm))
# game words: 0x824884E4 vperm128 v126,v11,v12,v6 and 0x821BC794 vperm128 v0,v0,v127,v7
vtest("test_kuf_vperm128_game_vc6", ["# tests: vperm128", ".long 0x17CB618C"], [("v11", A16), ("v12", B16), ("v6", ctl)], ("v126", perm))
vtest("test_kuf_vperm128_game_vc7", ["# tests: vperm128", ".long 0x1400F9C3"], [("v0", A16), ("v127", B16), ("v7", ctl)], ("v0", perm))
vtest("test_kuf_vsldoi128", "vsldoi128 v3, v4, v5, 5", [("v4", A16), ("v5", B16)],
      ("v3", [struct.unpack(">I", cat[5 + 4 * i:9 + 4 * i])[0] for i in range(4)]))
vtest("test_kuf_vspltw128", "vspltw128 v3, v4, 2", [("v4", A16)], ("v3", [A16[2]] * 4))
# game words: 0x823941E0 vspltisw128 v127,-1 ; 0x827D4F48 vspltisw128 v63,4 ; 0x826973B4 vspltisw128 v126,1
vtest("test_kuf_vspltisw128_game_m1", ["# tests: vspltisw128", ".long 0x1BFF077C"], [], ("v127", [0xFFFFFFFF] * 4))
vtest("test_kuf_vspltisw128_game_4", ["# tests: vspltisw128", ".long 0x1BE40774"], [], ("v63", [4] * 4))
vtest("test_kuf_vspltisw128_game_1", ["# tests: vspltisw128", ".long 0x1BC1077C"], [], ("v126", [1] * 4))
vtest("test_kuf_vmrghw128", "vmrghw128 v3, v4, v5", [("v4", A16), ("v5", B16)],
      ("v3", [A16[0], B16[0], A16[1], B16[1]]))
vtest("test_kuf_vmrglw128", "vmrglw128 v3, v4, v5", [("v4", A16), ("v5", B16)],
      ("v3", [A16[2], B16[2], A16[3], B16[3]]))

# float compares and rounding
ca, cb = [1.0, 2.0, float("nan"), -0.0], [1.0, 3.0, 1.0, 0.0]
vtest("test_kuf_vcmpgefp", "vcmpgefp v3, v4, v5", [("v4", vf(ca)), ("v5", vf(cb))],
      ("v3", [0xFFFFFFFF if x >= y else 0 for x, y in zip(ca, cb)]))
vtest("test_kuf_vcmpgefp128", "vcmpgefp128 v3, v4, v5", [("v4", vf(ca)), ("v5", vf(cb))],
      ("v3", [0xFFFFFFFF if x >= y else 0 for x, y in zip(ca, cb)]))
vtest("test_kuf_vcmpgtfp128", "vcmpgtfp128 v3, v4, v5", [("v4", vf(ca)), ("v5", vf(cb))],
      ("v3", [0xFFFFFFFF if x > y else 0 for x, y in zip(ca, cb)]))
rv = [1.5, -1.5, 2.0, -0.25]
vtest("test_kuf_vrfiz", "vrfiz v3, v4", [("v4", vf(rv))], ("v3", [f32_bits(np.trunc(f32(v))) for v in rv]))
vtest("test_kuf_vrfim", "vrfim v3, v4", [("v4", vf(rv))], ("v3", [f32_bits(np.floor(f32(v))) for v in rv]))
# estimates: exactly-defined special values only
vtest("test_kuf_vrefp_special", "vrefp v3, v4", [("v4", [0x00000000, 0x80000000, 0x7F800000, 0xFF800000])],
      ("v3", [0x7F800000, 0xFF800000, 0x00000000, 0x80000000]))
vtest("test_kuf_vrefp128_special", "vrefp128 v3, v4", [("v4", [0x00000000, 0x80000000, 0x7F800000, 0xFF800000])],
      ("v3", [0x7F800000, 0xFF800000, 0x00000000, 0x80000000]))
vtest("test_kuf_vrsqrtefp128_special", "vrsqrtefp128 v3, v4", [("v4", [0x7F800000, 0x00000000, 0x80000000, 0x7F800000])],
      ("v3", [0x00000000, 0x7F800000, 0xFF800000, 0x00000000]))
vtest("test_kuf_vlogefp_special", "vlogefp v3, v4", [("v4", [0x7F800000, 0x00000000, 0x7F800000, 0x00000000])],
      ("v3", [0x7F800000, 0xFF800000, 0x7F800000, 0xFF800000]))
# float -> unsigned int, saturating, scaled by 2^uimm; NaN -> 0
vtest("test_kuf_vcfpuxws128", "vcfpuxws128 v3, v4, 0", [("v4", vf([1.5, -1.0, 4.5e9]) + [0x7FC00000])],
      ("v3", [1, 0, 0xFFFFFFFF, 0]))
vtest("test_kuf_vcfpuxws128_scaled", "vcfpuxws128 v3, v4, 2", [("v4", vf([1.25, 0.1, 3.0, 0.0]))],
      ("v3", [5, 0, 12, 0]))

# unaligned and element stores (memory semantics mirror the SDK's lvlx/stvlx/stvewx cases)
test("test_kuf_lvlx128", ["  #_ MEMORY_IN 10001077 0a 0b 0c 0d 0e 0f 10 13 0c 0d 0e 10 11 12 13 14 ff ff ff ff ff ff",
                          "  #_ REGISTER_IN r4 0x10001077", "  lvlx128 v3, r4, r0", "  blr",
                          "  #_ REGISTER_OUT v3 [0A0B0C0D, 0E0F1013, 0C000000, 00000000]"])
test("test_kuf_stvlx128", ["  #_ MEMORY_IN 10001040 00000000 00000000 00000000 3F800000",
                           "  #_ REGISTER_IN r4 0x10001044",
                           "  #_ REGISTER_IN v3 [BE74FCBD, BD912ABA, BF317BBB, BF2D135F]",
                           "  stvlx128 v3, r4, r0", "  blr",
                           "  #_ MEMORY_OUT 10001040 00000000 BE74FCBD BD912ABA BF317BBB"])
test("test_kuf_stvehx", ["  #_ MEMORY_IN 10001050 CCCCCCCC CCCCCCCC CCCCCCCC CCCCCCCC",
                         "  #_ REGISTER_IN r4 0x10001056",
                         "  #_ REGISTER_IN v3 [00010203, 04050607, 08090A0B, 0C0D0E0F]",
                         "  stvehx v3, r0, r4", "  blr",
                         "  #_ MEMORY_OUT 10001050 CCCCCCCC CCCC0607 CCCCCCCC CCCCCCCC"])

with open(os.path.join(out_dir, "instr_kuf_vmx.s"), "w") as f:
    f.write("# Generated by tools/gen_insn_tests.py -- do not edit.\n\n")
    for name, lines in tests:
        f.write(f"{name}:\n" + "\n".join(lines) + "\n\n")

# ---------------------------------------------------------------- known divergences (docs/KNOWN_ISSUES.md)
# PowerPC generates a *positive* default QNaN for invalid operations (0x7FF8000000000000 double,
# 0x7FC00000 single/VMX); x86 SSE produces the negative "real indefinite". These cases hold hardware
# semantics and are expected to fail until the runtime canonicalises generated NaNs.
tests.clear()
test("test_kuf_nan_fdiv_zero_by_zero", [f"  #_ REGISTER_IN f1 {lit(0.0)}", f"  #_ REGISTER_IN f2 {lit(0.0)}",
                                        "  fdiv f3, f1, f2", "  blr", "  #_ REGISTER_OUT f3 0x7FF8000000000000"])
test("test_kuf_nan_fsubs_inf_minus_inf", ["  #_ REGISTER_IN f1 0x7FF0000000000000",
                                          "  #_ REGISTER_IN f2 0x7FF0000000000000",
                                          "  fsubs f3, f1, f2", "  blr", "  #_ REGISTER_OUT f3 0x7FF8000000000000"])
vtest("test_kuf_nan_vrsqrtefp_negative", "vrsqrtefp v3, v4", [("v4", [0xBF800000] * 4)], ("v3", [0x7FC00000] * 4))
vtest("test_kuf_nan_vsubfp_inf_minus_inf", "vsubfp v3, v4, v5", [("v4", [0x7F800000] * 4), ("v5", [0x7F800000] * 4)],
      ("v3", [0x7FC00000] * 4))
div_dir = os.path.join(os.path.dirname(os.path.abspath(out_dir)), "ppc_asm_divergence")
os.makedirs(div_dir, exist_ok=True)
with open(os.path.join(div_dir, "instr_kuf_nan.s"), "w") as f:
    f.write("# Generated by tools/gen_insn_tests.py -- do not edit. Expected to FAIL (known divergence).\n\n")
    for name, lines in tests:
        f.write(f"{name}:\n" + "\n".join(lines) + "\n\n")
print("wrote instr_kuf_fp.s, instr_kuf_vmx.s to", out_dir, "and instr_kuf_nan.s to", div_dir)
