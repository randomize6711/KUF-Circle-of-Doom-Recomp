#include "core/update/zip.h"

#include <array>
#include <cstring>
#include <fstream>

#include "core/platform.h"
#include "core/util.h"

namespace kuf::setup::update {
namespace fs = std::filesystem;

// ---- inflate (RFC 1951), after Mark Adler's puff.c design ----------------------------------------------------------

namespace {

struct Bits {
  const uint8_t* in;
  size_t size, at = 0;
  uint32_t buf = 0;
  int count = 0;
  bool error = false;
  int Take(int need) {
    uint32_t v = buf;
    while (count < need) {
      if (at >= size) {
        error = true;
        return 0;
      }
      v |= uint32_t(in[at++]) << count;
      count += 8;
    }
    buf = v >> need;
    count -= need;
    return int(v & ((1u << need) - 1));
  }
};

struct Huffman {
  std::array<short, 16> count{};
  std::array<short, 288> symbol{};
};

int Decode(Bits& b, const Huffman& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; ++len) {
    code |= b.Take(1);
    if (b.error) return -1;
    const int n = h.count[size_t(len)];
    if (code - n < first) return h.symbol[size_t(index + (code - first))];
    index += n;
    first += n;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

// Builds a canonical code from lengths; false if over-subscribed (incomplete codes are allowed where deflate does).
bool Build(Huffman& h, const short* lengths, int n) {
  h.count.fill(0);
  for (int s = 0; s < n; ++s) h.count[size_t(lengths[s])]++;
  if (h.count[0] == n) return true;
  int left = 1;
  for (int len = 1; len < 16; ++len) {
    left <<= 1;
    left -= h.count[size_t(len)];
    if (left < 0) return false;
  }
  short offs[16];
  offs[1] = 0;
  for (int len = 1; len < 15; ++len) offs[len + 1] = short(offs[len] + h.count[size_t(len)]);
  for (int s = 0; s < n; ++s) {
    if (lengths[s] != 0) h.symbol[size_t(offs[lengths[s]]++)] = short(s);
  }
  return true;
}

constexpr short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Bits& b, std::string* out, size_t limit, const Huffman& lencode, const Huffman& distcode) {
  for (;;) {
    int sym = Decode(b, lencode);
    if (sym < 0) return false;
    if (sym < 256) {
      if (out->size() >= limit) return false;
      out->push_back(char(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const int len = kLenBase[sym] + b.Take(kLenExtra[sym]);
      const int dsym = Decode(b, distcode);
      if (dsym < 0 || dsym >= 30) return false;
      const size_t dist = size_t(kDistBase[dsym] + b.Take(kDistExtra[dsym]));
      if (b.error || dist > out->size() || out->size() + size_t(len) > limit) return false;
      const size_t from = out->size() - dist;
      for (int k = 0; k < len; ++k) out->push_back((*out)[from + size_t(k)]);
    }
  }
}

bool Fixed(Bits& b, std::string* out, size_t limit) {
  static Huffman lencode, distcode;
  static bool built = [] {
    short lengths[288 + 30];
    int s = 0;
    for (; s < 144; ++s) lengths[s] = 8;
    for (; s < 256; ++s) lengths[s] = 9;
    for (; s < 280; ++s) lengths[s] = 7;
    for (; s < 288; ++s) lengths[s] = 8;
    Build(lencode, lengths, 288);
    for (s = 0; s < 30; ++s) lengths[s] = 5;
    Build(distcode, lengths, 30);
    return true;
  }();
  (void)built;
  return Codes(b, out, limit, lencode, distcode);
}

bool Dynamic(Bits& b, std::string* out, size_t limit) {
  static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  const int nlen = b.Take(5) + 257, ndist = b.Take(5) + 1, ncode = b.Take(4) + 4;
  if (b.error || nlen > 286 || ndist > 30) return false;
  short lengths[320] = {};
  for (int k = 0; k < ncode; ++k) lengths[order[k]] = short(b.Take(3));
  Huffman lencode, distcode;
  if (b.error || !Build(lencode, lengths, 19)) return false;
  int index = 0;
  while (index < nlen + ndist) {
    int sym = Decode(b, lencode);
    if (sym < 0) return false;
    if (sym < 16) {
      lengths[index++] = short(sym);
    } else {
      short len = 0;
      int repeat;
      if (sym == 16) {
        if (index == 0) return false;
        len = lengths[index - 1];
        repeat = 3 + b.Take(2);
      } else if (sym == 17) {
        repeat = 3 + b.Take(3);
      } else {
        repeat = 11 + b.Take(7);
      }
      if (b.error || index + repeat > nlen + ndist) return false;
      while (repeat--) lengths[index++] = len;
    }
  }
  if (lengths[256] == 0) return false;  // no end-of-block code
  if (!Build(lencode, lengths, nlen) || !Build(distcode, lengths + nlen, ndist)) return false;
  return Codes(b, out, limit, lencode, distcode);
}

}  // namespace

bool Inflate(const uint8_t* data, size_t size, std::string* out, size_t limit) {
  out->clear();
  Bits b{data, size};
  int last;
  do {
    last = b.Take(1);
    const int type = b.Take(2);
    if (b.error) return false;
    if (type == 0) {  // stored
      b.buf = 0;
      b.count = 0;
      if (b.at + 4 > size) return false;
      const size_t len = size_t(data[b.at]) | size_t(data[b.at + 1]) << 8;
      const size_t nlen = size_t(data[b.at + 2]) | size_t(data[b.at + 3]) << 8;
      b.at += 4;
      if (len != (~nlen & 0xffff) || b.at + len > size || out->size() + len > limit) return false;
      out->append(reinterpret_cast<const char*>(data + b.at), len);
      b.at += len;
    } else if (type == 1) {
      if (!Fixed(b, out, limit)) return false;
    } else if (type == 2) {
      if (!Dynamic(b, out, limit)) return false;
    } else {
      return false;
    }
  } while (!last);
  return true;
}

uint32_t Crc32(const void* data, size_t size, uint32_t crc) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  crc = ~crc;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

// ---- the archive ---------------------------------------------------------------------------------------------------

namespace {

uint16_t U16(const std::string& s, size_t at) { return uint16_t(uint8_t(s[at]) | uint8_t(s[at + 1]) << 8); }
uint32_t U32(const std::string& s, size_t at) { return uint32_t(U16(s, at)) | uint32_t(U16(s, at + 2)) << 16; }

bool SafeName(const std::string& n) {
  if (n.empty() || n.size() > 512 || n[0] == '/' || n.find('\\') != std::string::npos ||
      n.find(':') != std::string::npos) {
    return false;
  }
  for (char c : n) {
    if (uint8_t(c) < 0x20) return false;
  }
  size_t start = 0;
  while (start <= n.size()) {
    const size_t end = n.find('/', start);
    const std::string part = n.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (part == ".." || part == "." || (part.empty() && end != std::string::npos)) return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

bool ReadAt(std::ifstream& f, uint64_t at, size_t n, std::string* out) {
  out->assign(n, '\0');
  f.clear();
  f.seekg(std::streamoff(at));
  return bool(f.read(out->data(), std::streamsize(n)));
}

}  // namespace

bool ReadZipDirectory(const fs::path& file, std::vector<ZipEntry>* entries, std::string* why,
                      const ZipLimits& limits) {
  entries->clear();
  const auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  std::ifstream f(file, std::ios::binary);
  std::error_code ec;
  const uint64_t size = fs::file_size(file, ec);
  if (!f || ec || size < 22) return fail("not a ZIP file");
  // The end record: within the last 64 KiB + 22 bytes (its comment).
  const uint64_t tail_size = std::min<uint64_t>(size, 65557);
  std::string tail;
  if (!ReadAt(f, size - tail_size, size_t(tail_size), &tail)) return fail("cannot read the ZIP file");
  size_t eocd = std::string::npos;
  for (size_t k = tail.size() - 22 + 1; k-- > 0;) {
    if (U32(tail, k) == 0x06054b50) {
      eocd = k;
      break;
    }
  }
  if (eocd == std::string::npos) return fail("no ZIP directory (damaged or not a ZIP file)");
  const size_t count = U16(tail, eocd + 10);
  const uint32_t dir_size = U32(tail, eocd + 12), dir_offset = U32(tail, eocd + 16);
  if (count == 0xffff || dir_offset == 0xffffffffu) return fail("ZIP64 archives are not supported");
  if (count > limits.max_entries) return fail("too many entries in the ZIP file");
  if (uint64_t(dir_offset) + dir_size > size) return fail("the ZIP directory is outside the file");
  std::string dir;
  if (!ReadAt(f, dir_offset, dir_size, &dir)) return fail("cannot read the ZIP directory");
  size_t at = 0;
  uint64_t total = 0;
  for (size_t k = 0; k < count; ++k) {
    if (at + 46 > dir.size() || U32(dir, at) != 0x02014b50) return fail("the ZIP directory is damaged");
    ZipEntry e;
    const uint16_t flags = U16(dir, at + 8);
    e.method = U16(dir, at + 10);
    e.crc = U32(dir, at + 16);
    e.packed = U32(dir, at + 20);
    e.size = U32(dir, at + 24);
    const size_t name_len = U16(dir, at + 28), extra_len = U16(dir, at + 30), comment_len = U16(dir, at + 32);
    e.local_offset = U32(dir, at + 42);
    if (at + 46 + name_len + extra_len + comment_len > dir.size()) return fail("the ZIP directory is damaged");
    e.name = dir.substr(at + 46, name_len);
    at += 46 + name_len + extra_len + comment_len;
    if (flags & 1) return fail("encrypted ZIP entries are not supported");
    if (e.method != 0 && e.method != 8) return fail("ZIP entry " + e.name + ": unsupported compression");
    if (!SafeName(e.name)) return fail("unsafe name in the ZIP file: " + e.name);
    if (e.size > limits.max_entry || (total += e.size) > limits.max_total) return fail("the ZIP file is too large");
    if (e.local_offset + 30 + e.packed > size) return fail("ZIP entry " + e.name + " is outside the file");
    entries->push_back(std::move(e));
  }
  return true;
}

bool ExtractZip(const fs::path& file, const std::vector<ZipEntry>& entries,
                const std::function<std::string(const std::string& name)>& map, const fs::path& dest,
                std::string* why) {
  const auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  std::ifstream f(file, std::ios::binary);
  if (!f) return fail("cannot open the ZIP file");
  for (const ZipEntry& e : entries) {
    const std::string rel = map(e.name);
    if (rel.empty()) continue;
    if (!SafeName(rel)) return fail("unsafe destination " + rel);
    const fs::path out = dest / PathFromUtf8(rel);
    std::error_code ec;
    if (!e.name.empty() && e.name.back() == '/') {
      fs::create_directories(out, ec);
      if (ec) return fail("cannot create " + PathUtf8(out));
      continue;
    }
    std::string local;
    if (!ReadAt(f, e.local_offset, 30, &local) || U32(local, 0) != 0x04034b50) {
      return fail("ZIP entry " + e.name + ": damaged");
    }
    const uint64_t data_at = e.local_offset + 30 + U16(local, 26) + U16(local, 28);
    std::string packed, data;
    if (!ReadAt(f, data_at, size_t(e.packed), &packed)) return fail("ZIP entry " + e.name + ": truncated");
    if (e.method == 0) {
      data = std::move(packed);
    } else if (!Inflate(reinterpret_cast<const uint8_t*>(packed.data()), packed.size(), &data, size_t(e.size))) {
      return fail("ZIP entry " + e.name + ": damaged compressed data");
    }
    if (data.size() != e.size || Crc32(data.data(), data.size()) != e.crc) {
      return fail("ZIP entry " + e.name + ": the content does not match its checksum");
    }
    fs::create_directories(out.parent_path(), ec);
    if (!AtomicWrite(out, data)) return fail("cannot write " + PathUtf8(out));
  }
  return true;
}

}  // namespace kuf::setup::update
