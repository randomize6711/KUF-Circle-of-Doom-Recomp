#include "runtime/png_writer.h"

#include <array>
#include <fstream>
#include <string>
#include <vector>

namespace kuf::png {
namespace {

const std::array<uint32_t, 256>& CrcTable() {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  return table;
}

uint32_t Crc(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
  for (size_t i = 0; i < size; ++i) crc = CrcTable()[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void Chunk(std::ofstream& file, const char type[4], const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> buf;
  Put32(buf, static_cast<uint32_t>(payload.size()));
  buf.insert(buf.end(), type, type + 4);
  buf.insert(buf.end(), payload.begin(), payload.end());
  uint32_t crc = Crc(buf.data() + 4, buf.size() - 4) ^ 0xFFFFFFFFu;
  Put32(buf, crc);
  file.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
}

}  // namespace

bool WriteRgb(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* rgbx,
              size_t stride) {
  // Raw scanlines: filter byte 0 + RGB.
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (1 + size_t(width) * 3));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const uint8_t* row = rgbx + size_t(y) * stride;
    for (uint32_t x = 0; x < width; ++x) {
      raw.push_back(row[x * 4 + 0]);
      raw.push_back(row[x * 4 + 1]);
      raw.push_back(row[x * 4 + 2]);
    }
  }
  // zlib stream of stored blocks (max 65535 bytes each) + Adler-32.
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  size_t pos = 0;
  do {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  } while (pos < raw.size());
  Put32(z, (b << 16) | a);

  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  file.write(reinterpret_cast<const char*>(kSignature), 8);
  std::vector<uint8_t> ihdr;
  Put32(ihdr, width);
  Put32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, truecolor RGB, deflate, no filter, no interlace
  Chunk(file, "IHDR", ihdr);
  Chunk(file, "IDAT", z);
  Chunk(file, "IEND", {});
  return static_cast<bool>(file);
}

}  // namespace kuf::png
