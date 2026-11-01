// Native co-op: the Xbox LIVE session structures the game reads, written from the co-op session advertisement
// (docs/MULTIPLAYER.md "Co-op runtime"). Every function takes guest memory as `base` (guest address a is base + a,
// big-endian), so tests run them on a plain buffer.
//
// Layouts (XDK, 32-bit pointers):
//   XSESSION_INFO (60): XNKID sessionID, XNADDR hostAddress, XNKEY keyExchangeKey.
//   XSESSION_SEARCHRESULT_HEADER (8): dwSearchResults, pResults; XSESSION_SEARCHRESULT (92): info, open public /
//     private slots, filled public / private slots, cProperties, cContexts, pProperties, pContexts.
//   XUSER_CONTEXT (8): id, value. XUSER_PROPERTY (24): id, pad, XUSER_DATA (type at +8, value at +16).
//   XNQOS: cxnqos, cxnqosPending, XNQOSINFO[] (24 each): bFlags, bReserved, cProbesXmit, cProbesRecv, cbData,
//     pbData, wRttMinInMsecs, wRttMedInMsecs, dwUpBitsPerSec, dwDnBitsPerSec.
//   XSESSION_REGISTRATION_RESULTS (8): wNumRegistrants, rgRegistrants; XSESSION_REGISTRANT (24): qwMachineID,
//     bTrustworthiness, bNumUsers, rgUsers.
#pragma once

#include <cstdint>
#include <vector>

#include "coop/coop_addr.h"

namespace kuf::coop {

constexpr uint32_t kSearchHeaderSize = 8, kSearchResultSize = 92, kContextSize = 8, kPropertySize = 24;
constexpr uint32_t kQosInfoSize = 24, kRegistrantSize = 24;
constexpr uint8_t kQosComplete = 0x01, kQosContacted = 0x02, kQosDataReceived = 0x08;
constexpr uint32_t kErrorInsufficientBuffer = 122;

void WriteSessionInfo(uint8_t* base, uint32_t info, const SessionAd& ad);
SessionAd ReadSessionInfo(const uint8_t* base, uint32_t info);  // id, host and key only

// What the game searched with: its contexts and properties are given back as each result's (the host is
// configured from the same choices; only numeric properties, never ones that point into the game's memory).
struct SearchQuery {
  uint32_t num_props = 0, props = 0, num_ctx = 0, ctx = 0;
};
uint32_t SearchResultsSize(const uint8_t* base, size_t results, const SearchQuery& query);
// Writes the header and results at `out` if `cb` is enough: 0, else kErrorInsufficientBuffer (nothing written).
uint32_t WriteSearchResults(uint8_t* base, uint32_t out, uint32_t cb, const std::vector<SessionAd>& ads,
                            const SearchQuery& query);

// XNetQosLookup's answer for `targets` (the XNADDRs looked up): a target that is a known session's host answers
// with that session's QoS data; another address of this scheme is contacted without data; any other address is not
// reachable. `nominal_rtt_ms` stands in for a measurement.
uint32_t QosSize(const std::vector<XnAddr>& targets, const std::vector<SessionAd>& known);
void WriteQos(uint8_t* base, uint32_t out, const std::vector<XnAddr>& targets, const std::vector<SessionAd>& known,
              uint16_t nominal_rtt_ms);

// Arbitration: every machine in the session (the host first) with its one user.
uint32_t RegistrationSize(size_t machines);
uint32_t WriteRegistration(uint8_t* base, uint32_t out, uint32_t cb, const std::vector<uint32_t>& machine_ips);

}  // namespace kuf::coop
