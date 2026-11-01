#include "coop/coop_xsession.h"

#include <algorithm>
#include <cstring>

namespace kuf::coop {
namespace {

constexpr uint8_t kTypeUnicode = 4, kTypeBinary = 6;  // XUSER_DATA types that carry a pointer

uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Put16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
void Put64(uint8_t* p, uint64_t v) {
  Put32(p, uint32_t(v >> 32));
  Put32(p + 4, uint32_t(v));
}

// The query's properties that can be given back (no pointers).
std::vector<uint32_t> CopyableProps(const uint8_t* base, const SearchQuery& q) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; q.props && i < q.num_props; ++i) {
    const uint32_t p = q.props + i * kPropertySize;
    const uint8_t type = base[p + 8];
    if (type != kTypeUnicode && type != kTypeBinary) out.push_back(p);
  }
  return out;
}

uint32_t NumContexts(const SearchQuery& q) { return q.ctx ? q.num_ctx : 0; }

const SessionAd* Find(const std::vector<SessionAd>& known, const XnAddr& target) {
  for (const SessionAd& ad : known) {
    if (ad.host == target) return &ad;
  }
  return nullptr;
}

}  // namespace

void WriteSessionInfo(uint8_t* base, uint32_t info, const SessionAd& ad) {
  std::memcpy(base + info, ad.id.data(), kXnKidSize);
  std::memcpy(base + info + kXnKidSize, ad.host.data(), kXnAddrSize);
  std::memcpy(base + info + kXnKidSize + kXnAddrSize, ad.key.data(), kXnKeySize);
}

SessionAd ReadSessionInfo(const uint8_t* base, uint32_t info) {
  SessionAd ad;
  std::memcpy(ad.id.data(), base + info, kXnKidSize);
  std::memcpy(ad.host.data(), base + info + kXnKidSize, kXnAddrSize);
  std::memcpy(ad.key.data(), base + info + kXnKidSize + kXnAddrSize, kXnKeySize);
  return ad;
}

uint32_t SearchResultsSize(const uint8_t* base, size_t results, const SearchQuery& query) {
  const uint32_t per = kSearchResultSize + NumContexts(query) * kContextSize +
                       uint32_t(CopyableProps(base, query).size()) * kPropertySize;
  return kSearchHeaderSize + uint32_t(results) * per;
}

uint32_t WriteSearchResults(uint8_t* base, uint32_t out, uint32_t cb, const std::vector<SessionAd>& ads,
                            const SearchQuery& query) {
  if (cb < SearchResultsSize(base, ads.size(), query)) return kErrorInsufficientBuffer;
  const std::vector<uint32_t> props = CopyableProps(base, query);
  const uint32_t nctx = NumContexts(query);
  const uint32_t results = out + kSearchHeaderSize;
  uint32_t extra = results + uint32_t(ads.size()) * kSearchResultSize;  // contexts and properties follow
  Put32(base + out, uint32_t(ads.size()));
  Put32(base + out + 4, ads.empty() ? 0 : results);
  for (size_t i = 0; i < ads.size(); ++i) {
    const SessionAd& ad = ads[i];
    uint8_t* r = base + results + uint32_t(i) * kSearchResultSize;
    std::memset(r, 0, kSearchResultSize);
    WriteSessionInfo(base, results + uint32_t(i) * kSearchResultSize, ad);
    Put32(r + 60, ad.public_slots - ad.filled_public);
    Put32(r + 64, ad.private_slots - ad.filled_private);
    Put32(r + 68, ad.filled_public);
    Put32(r + 72, ad.filled_private);
    Put32(r + 76, uint32_t(props.size()));
    Put32(r + 80, nctx);
    if (!props.empty()) {
      Put32(r + 84, extra);
      for (uint32_t p : props) {
        std::memcpy(base + extra, base + p, kPropertySize);
        extra += kPropertySize;
      }
    }
    if (nctx) {
      Put32(r + 88, extra);
      std::memcpy(base + extra, base + query.ctx, nctx * kContextSize);
      extra += nctx * kContextSize;
    }
  }
  return 0;
}

uint32_t QosSize(const std::vector<XnAddr>& targets, const std::vector<SessionAd>& known) {
  uint32_t size = 8 + uint32_t(targets.size()) * kQosInfoSize;
  for (const XnAddr& t : targets) {
    if (const SessionAd* ad = Find(known, t)) size += uint32_t(ad->qos.size());
  }
  return size;
}

void WriteQos(uint8_t* base, uint32_t out, const std::vector<XnAddr>& targets, const std::vector<SessionAd>& known,
              uint16_t nominal_rtt_ms) {
  Put32(base + out, uint32_t(targets.size()));
  Put32(base + out + 4, 0);  // nothing pending: every answer is final
  uint32_t data = out + 8 + uint32_t(targets.size()) * kQosInfoSize;
  for (size_t i = 0; i < targets.size(); ++i) {
    uint8_t* q = base + out + 8 + uint32_t(i) * kQosInfoSize;
    std::memset(q, 0, kQosInfoSize);
    const SessionAd* ad = Find(known, targets[i]);
    const bool ours = IpOfXnAddr(targets[i].data()).has_value();
    uint8_t flags = kQosComplete;
    if (ad || ours) {
      flags |= kQosContacted;
      Put16(q + 2, 1);  // probes sent / received
      Put16(q + 4, 1);
      Put16(q + 12, nominal_rtt_ms);
      Put16(q + 14, nominal_rtt_ms);
      Put32(q + 16, 10000000);  // 10 Mbit/s each way
      Put32(q + 20, 10000000);
    }
    if (ad && !ad->qos.empty()) {
      flags |= kQosDataReceived;
      Put16(q + 6, uint16_t(ad->qos.size()));
      Put32(q + 8, data);
      std::memcpy(base + data, ad->qos.data(), ad->qos.size());
      data += uint32_t(ad->qos.size());
    }
    q[0] = flags;
  }
}

uint32_t RegistrationSize(size_t machines) { return 8 + uint32_t(machines) * (kRegistrantSize + 8); }

uint32_t WriteRegistration(uint8_t* base, uint32_t out, uint32_t cb, const std::vector<uint32_t>& machine_ips) {
  if (cb < RegistrationSize(machine_ips.size())) return kErrorInsufficientBuffer;
  const uint32_t n = uint32_t(machine_ips.size());
  const uint32_t registrants = out + 8, users = registrants + n * kRegistrantSize;
  Put32(base + out, n);
  Put32(base + out + 4, n ? registrants : 0);
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* r = base + registrants + i * kRegistrantSize;
    std::memset(r, 0, kRegistrantSize);
    Put64(r, MachineIdOf(machine_ips[i]));
    Put32(r + 8, 1);   // trustworthy
    Put32(r + 12, 1);  // one user per machine
    Put32(r + 16, users + i * 8);
    Put64(base + users + i * 8, XuidOf(machine_ips[i]));
  }
  return 0;
}

}  // namespace kuf::coop
