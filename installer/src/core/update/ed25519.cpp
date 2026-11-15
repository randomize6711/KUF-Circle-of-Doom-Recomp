#include "core/update/ed25519.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "core/update/sha512.h"

namespace kuf::setup::update {
namespace {

// Field elements mod p = 2^255 - 19: 16 limbs of 16 bits (TweetNaCl's representation).
using i64 = long long;
using Gf = i64[16];

struct Constants {
  Gf zero{}, one{}, d{}, d2{}, i{}, bx{}, by{};
};

void Set(Gf r, const Gf a) {
  for (int k = 0; k < 16; ++k) r[k] = a[k];
}

void Carry(Gf o) {
  for (int k = 0; k < 16; ++k) {
    o[k] += (i64(1) << 16);
    const i64 c = o[k] >> 16;
    o[(k + 1) * (k < 15)] += c - 1 + 37 * (c - 1) * (k == 15);
    o[k] -= c << 16;
  }
}

void Select(Gf p, Gf q, int b) {
  const i64 c = ~(i64(b) - 1);
  for (int k = 0; k < 16; ++k) {
    const i64 t = c & (p[k] ^ q[k]);
    p[k] ^= t;
    q[k] ^= t;
  }
}

void Pack(uint8_t* o, const Gf n) {
  Gf m, t;
  Set(t, n);
  Carry(t);
  Carry(t);
  Carry(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int k = 1; k < 15; ++k) {
      m[k] = t[k] - 0xffff - ((m[k - 1] >> 16) & 1);
      m[k - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    const int b = int((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    Select(t, m, 1 - b);
  }
  for (int k = 0; k < 16; ++k) {
    o[2 * k] = uint8_t(t[k] & 0xff);
    o[2 * k + 1] = uint8_t(t[k] >> 8);
  }
}

bool Equal32(const uint8_t* a, const uint8_t* b) {
  uint8_t d = 0;
  for (int k = 0; k < 32; ++k) d |= uint8_t(a[k] ^ b[k]);
  return d == 0;
}

bool FieldEqual(const Gf a, const Gf b) {
  uint8_t c[32], d[32];
  Pack(c, a);
  Pack(d, b);
  return Equal32(c, d);
}

int Parity(const Gf a) {
  uint8_t d[32];
  Pack(d, a);
  return d[0] & 1;
}

void Unpack(Gf o, const uint8_t* n) {
  for (int k = 0; k < 16; ++k) o[k] = n[2 * k] + (i64(n[2 * k + 1]) << 8);
  o[15] &= 0x7fff;
}

void Add(Gf o, const Gf a, const Gf b) {
  for (int k = 0; k < 16; ++k) o[k] = a[k] + b[k];
}
void Sub(Gf o, const Gf a, const Gf b) {
  for (int k = 0; k < 16; ++k) o[k] = a[k] - b[k];
}
void Mul(Gf o, const Gf a, const Gf b) {
  i64 t[31] = {};
  for (int k = 0; k < 16; ++k) {
    for (int j = 0; j < 16; ++j) t[k + j] += a[k] * b[j];
  }
  for (int k = 0; k < 15; ++k) t[k] += 38 * t[k + 16];
  for (int k = 0; k < 16; ++k) o[k] = t[k];
  Carry(o);
  Carry(o);
}
void Square(Gf o, const Gf a) { Mul(o, a, a); }

// a^e for a little-endian exponent of 32 bytes.
void Power(Gf o, const Gf a, const uint8_t e[32]) {
  Gf r, base;
  Set(base, a);
  for (int k = 0; k < 16; ++k) r[k] = k == 0;
  for (int bit = 255; bit >= 0; --bit) {
    Square(r, r);
    if ((e[bit / 8] >> (bit % 8)) & 1) Mul(r, r, base);
  }
  Set(o, r);
}

// p - 2 (inverse), (p - 5) / 8 (square root helper), (p - 1) / 4 (sqrt(-1)): little-endian.
void Exponent(uint8_t e[32], int which) {
  // p = 2^255 - 19 = 0x7fff...ffed
  uint8_t p[32];
  std::memset(p, 0xff, 32);
  p[0] = 0xed;
  p[31] = 0x7f;
  std::memcpy(e, p, 32);
  if (which == 0) {  // p - 2
    e[0] = 0xeb;
  } else if (which == 1) {  // (p - 5) / 8 = 2^252 - 3
    std::memset(e, 0xff, 32);
    e[0] = 0xfd;
    e[31] = 0x0f;
  } else {  // (p - 1) / 4 = 2^253 - 5
    std::memset(e, 0xff, 32);
    e[0] = 0xfb;
    e[31] = 0x1f;
  }
}

void Invert(Gf o, const Gf a) {
  uint8_t e[32];
  Exponent(e, 0);
  Power(o, a, e);
}

void Pow2523(Gf o, const Gf a) {
  uint8_t e[32];
  Exponent(e, 1);
  Power(o, a, e);
}

void Small(Gf o, i64 v) {
  for (int k = 0; k < 16; ++k) o[k] = 0;
  o[0] = v & 0xffff;
  o[1] = v >> 16;
}

// x from y (and the sign of x) on the curve: -x^2 + y^2 = 1 + d x^2 y^2; false if there is none.
bool RecoverX(const Constants& c, Gf x, const Gf y, int sign) {
  Gf y2, num, den, den2, den4, den6, t, chk;
  Square(y2, y);
  Mul(den, y2, c.d);
  Sub(num, y2, c.one);
  Add(den, den, c.one);
  Square(den2, den);
  Square(den4, den2);
  Mul(den6, den4, den2);
  Mul(t, den6, num);
  Mul(t, t, den);
  Pow2523(t, t);
  Mul(t, t, num);
  Mul(t, t, den);
  Mul(t, t, den);
  Mul(x, t, den);
  Square(chk, x);
  Mul(chk, chk, den);
  if (!FieldEqual(chk, num)) Mul(x, x, c.i);
  Square(chk, x);
  Mul(chk, chk, den);
  if (!FieldEqual(chk, num)) return false;
  if (Parity(x) != sign) Sub(x, c.zero, x);
  return true;
}

const Constants& K() {
  static Constants c;
  static std::once_flag once;
  std::call_once(once, [] {
    Small(c.one, 1);
    Gf a, b;
    Small(a, 121665);
    Small(b, 121666);
    Invert(b, b);
    Mul(c.d, a, b);
    Sub(c.d, c.zero, c.d);  // d = -121665 / 121666
    Add(c.d2, c.d, c.d);
    Gf two;
    Small(two, 2);
    uint8_t e[32];
    Exponent(e, 2);
    Power(c.i, two, e);  // sqrt(-1) = 2^((p - 1) / 4)
    Gf four, five;
    Small(four, 4);
    Small(five, 5);
    Invert(five, five);
    Mul(c.by, four, five);  // the base point: y = 4/5, x even
    RecoverX(c, c.bx, c.by, 0);
  });
  return c;
}

// Points in extended coordinates (X, Y, Z, T).
using Point = Gf[4];

void PointAdd(Point p, Point q) {
  const Constants& c = K();
  Gf a, b, cc, d, t, e, f, g, h;
  Sub(a, p[1], p[0]);
  Sub(t, q[1], q[0]);
  Mul(a, a, t);
  Add(b, p[0], p[1]);
  Add(t, q[0], q[1]);
  Mul(b, b, t);
  Mul(cc, p[3], q[3]);
  Mul(cc, cc, c.d2);
  Mul(d, p[2], q[2]);
  Add(d, d, d);
  Sub(e, b, a);
  Sub(f, d, cc);
  Add(g, d, cc);
  Add(h, b, a);
  Mul(p[0], e, f);
  Mul(p[1], h, g);
  Mul(p[2], g, f);
  Mul(p[3], e, h);
}

void Swap(Point p, Point q, int b) {
  for (int k = 0; k < 4; ++k) Select(p[k], q[k], b);
}

void PackPoint(uint8_t* r, Point p) {
  Gf tx, ty, zi;
  Invert(zi, p[2]);
  Mul(tx, p[0], zi);
  Mul(ty, p[1], zi);
  Pack(r, ty);
  r[31] ^= uint8_t(Parity(tx) << 7);
}

void ScalarMult(Point p, Point q, const uint8_t* s) {
  const Constants& c = K();
  Set(p[0], c.zero);
  Set(p[1], c.one);
  Set(p[2], c.one);
  Set(p[3], c.zero);
  for (int k = 255; k >= 0; --k) {
    const int b = (s[k / 8] >> (k & 7)) & 1;
    Swap(p, q, b);
    PointAdd(q, p);
    PointAdd(p, p);
    Swap(p, q, b);
  }
}

void ScalarBase(Point p, const uint8_t* s) {
  const Constants& c = K();
  Point q;
  Set(q[0], c.bx);
  Set(q[1], c.by);
  Set(q[2], c.one);
  Mul(q[3], c.bx, c.by);
  ScalarMult(p, q, s);
}

constexpr i64 kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                        0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                        0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

void ModL(uint8_t* r, i64 x[64]) {
  i64 carry;
  for (int k = 63; k >= 32; --k) {
    carry = 0;
    int j;
    for (j = k - 32; j < k - 12; ++j) {
      x[j] += carry - 16 * x[k] * kL[j - (k - 32)];
      carry = (x[j] + 128) >> 8;
      x[j] -= carry * 256;
    }
    x[j] += carry;
    x[k] = 0;
  }
  carry = 0;
  for (int j = 0; j < 32; ++j) {
    x[j] += carry - (x[31] >> 4) * kL[j];
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  for (int j = 0; j < 32; ++j) x[j] -= carry * kL[j];
  for (int k = 0; k < 32; ++k) {
    x[k + 1] += x[k] >> 8;
    r[k] = uint8_t(x[k] & 255);
  }
}

void Reduce(uint8_t* r) {  // r: 64 bytes in, its value mod L in the first 32
  i64 x[64];
  for (int k = 0; k < 64; ++k) x[k] = i64(r[k]);
  std::memset(r, 0, 64);
  ModL(r, x);
}

// S < L (RFC 8032 5.1.7: a non-canonical S is rejected).
bool CanonicalScalar(const uint8_t s[32]) {
  for (int k = 31; k >= 0; --k) {
    if (s[k] < kL[k]) return true;
    if (s[k] > kL[k]) return false;
  }
  return false;  // equal to L
}

}  // namespace

bool Ed25519Verify(const uint8_t signature[64], const uint8_t* message, size_t size, const uint8_t public_key[32]) {
  if (!CanonicalScalar(signature + 32)) return false;
  // -A, the public key's point negated.
  Point a;
  Gf ay;
  Unpack(ay, public_key);
  const Constants& c = K();
  {
    uint8_t check[32];
    Pack(check, ay);
    check[31] |= uint8_t(public_key[31] & 0x80);
    if (!Equal32(check, public_key)) return false;  // y >= p: not a canonical encoding
  }
  if (!RecoverX(c, a[0], ay, (public_key[31] >> 7) ^ 1)) return false;
  Set(a[1], ay);
  Set(a[2], c.one);
  Mul(a[3], a[0], a[1]);
  // h = SHA-512(R || A || M) mod L; check [S]B - [h]A == R.
  Sha512 sha;
  sha.Update(signature, 32);
  sha.Update(public_key, 32);
  sha.Update(message, size);
  auto digest = sha.Final();
  uint8_t h[64];
  std::memcpy(h, digest.data(), 64);
  Reduce(h);
  Point p, q;
  ScalarMult(p, a, h);
  ScalarBase(q, signature + 32);
  PointAdd(p, q);
  uint8_t r[32];
  PackPoint(r, p);
  return Equal32(r, signature);
}

}  // namespace kuf::setup::update
