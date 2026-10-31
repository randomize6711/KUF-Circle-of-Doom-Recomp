// Diagnostics for the camera investigation (M6A, preparing M6B), off by default.
//
// The game's camera definitions (d:\data\camera\*.cam, "GenericCamVer3.0") are loaded by sub_8252D0C8(set,
// first, count, path): the whole file read with sub_82190C60, then parsed by version (3.0: sub_8252C740; the
// stream reader sub_8252CD20 reads the same layout). Per camera an entry {key count, keys} at set+84 + 8 * index,
// per key 348 bytes (21 words, 4 bytes, a word, two 128-byte blocks at +92 and +220). --kuf_trace_camera logs
// the words at +220..+259 of every key read (the free-camera parameters; docs/CAMERA.md). --kuf_camera_poke
// "offset=value[,offset=value]" sets those floats in every key read, to see which parameter they are.
// --kuf_camera_watch "address,bytes" logs which words of that guest memory change, twice a second (M6B: how the
// live camera object, docs/CAMERA.md, was mapped). None of them changes anything unless set.

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(kuf_trace_camera, false, "KUF/Debug",
                    "Log the camera definitions as the game reads them (M6A camera investigation)");
REXCVAR_DEFINE_STRING(kuf_camera_watch, "", "KUF/Debug",
                      "Camera investigation: 'address,bytes' (hex address) of guest memory whose changed words are "
                      "logged every 0.5 s, from the first camera file load");
REXCVAR_DEFINE_STRING(kuf_camera_poke, "", "KUF/Debug",
                      "M6A camera investigation: offset=value pairs set in every camera key read, e.g. 232=0.6");

namespace {

constexpr uint32_t kKeySize = 348, kEntries = 84, kFirstParam = 220, kParamWords = 10;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, sizeof(raw));
  return __builtin_bswap32(raw);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

float AsFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

std::vector<std::pair<uint32_t, float>> Pokes() {
  std::vector<std::pair<uint32_t, float>> out;
  const std::string spec = REXCVAR_GET(kuf_camera_poke);
  size_t start = 0;
  while (start < spec.size()) {
    size_t end = spec.find(',', start);
    if (end == std::string::npos) end = spec.size();
    const std::string item = spec.substr(start, end - start);
    const size_t eq = item.find('=');
    if (eq != std::string::npos) {
      const uint32_t offset = uint32_t(std::stoul(item.substr(0, eq)));
      if (offset + 4 <= kKeySize && offset % 4 == 0) {
        out.emplace_back(offset, std::stof(item.substr(eq + 1)));
      }
    }
    start = end + 1;
  }
  return out;
}

void WatchLoop(uint8_t* base, uint32_t address, uint32_t bytes) {
  std::vector<uint32_t> last;
  for (int i = 0; i < 1200; ++i) {  // ten minutes
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<uint32_t> now(bytes / 4);
    for (uint32_t w = 0; w < now.size(); ++w) {
      now[w] = Load32(base, address + 4 * w);
    }
    std::string changes;
    for (uint32_t w = 0; w < now.size(); ++w) {
      if (last.size() != now.size() || now[w] != last[w]) {
        changes += fmt::format(" +{:X}={:g}", 4 * w, AsFloat(now[w]));
      }
    }
    if (!changes.empty()) {
      REXLOG_INFO("[kuf-cam] watch {:08X}{}:{}", address, last.empty() ? " initial" : "", changes);
    }
    last = std::move(now);
  }
}

void StartWatch(uint8_t* base) {
  static bool started = false;
  const std::string spec = REXCVAR_GET(kuf_camera_watch);
  const size_t comma = spec.find(',');
  if (started || comma == std::string::npos) {
    return;
  }
  started = true;
  const uint32_t address = uint32_t(std::stoul(spec.substr(0, comma), nullptr, 16));
  const uint32_t bytes = std::min<uint32_t>(uint32_t(std::stoul(spec.substr(comma + 1), nullptr, 0)), 0x4000) & ~3u;
  std::thread(WatchLoop, base, address, bytes).detach();
}

}  // namespace

REX_EXTERN(__imp__sub_8252D0C8);
REX_HOOK_RAW(sub_8252D0C8) {  // load a camera definition file into a camera set: (set, first, count, path)
  const uint32_t set = ctx.r3.u32, first = ctx.r4.u32, path = ctx.r6.u32;
  __imp__sub_8252D0C8(ctx, base);
  StartWatch(base);
  const bool trace = REXCVAR_GET(kuf_trace_camera);
  const auto pokes = Pokes();
  if ((!trace && pokes.empty()) || set < 0x10000) {
    return;
  }
  const char* name = path >= 0x10000 ? reinterpret_cast<const char*>(base + path) : "?";
  const uint32_t capacity = Load32(base, set + 80);
  const uint32_t entries = Load32(base, set + kEntries);
  for (uint32_t camera = first; camera < capacity && entries >= 0x10000; ++camera) {
    const uint32_t keys = Load32(base, entries + 8 * camera), key_data = Load32(base, entries + 8 * camera + 4);
    if (keys == 0 || keys > 64 || key_data < 0x10000) {
      continue;
    }
    for (uint32_t k = 0; k < keys; ++k) {
      const uint32_t key = key_data + k * kKeySize;
      if (trace) {
        std::string values;
        for (uint32_t w = 0; w < kParamWords; ++w) {
          values += fmt::format(" {:g}", AsFloat(Load32(base, key + kFirstParam + 4 * w)));
        }
        REXLOG_INFO("[kuf-cam] {} camera {} key {}: +220..{}", name, camera, k, values);
      }
      for (const auto& [offset, value] : pokes) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        Store32(base, key + offset, bits);
      }
    }
  }
  if (!pokes.empty()) {
    REXLOG_INFO("[kuf-cam] {}: {} value(s) set in every key (--kuf_camera_poke)", name, pokes.size());
  }
}
