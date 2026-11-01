#include "net/steam_backend.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>

#include "net/steam_lobby.h"

#if KUF_HAVE_STEAMWORKS
#include <dlfcn.h>

#include <steam/steam_api_flat.h>
#endif

namespace kuf::net::steam {

namespace {
std::mutex g_mutex;
std::vector<SteamEvent> g_events;
std::optional<uint64_t> g_launch_lobby;

void Post(SteamEvent e) {
  std::lock_guard lock(g_mutex);
  if (g_events.size() < 64) g_events.push_back(std::move(e));
}
}  // namespace

std::vector<SteamEvent> TakeEvents() {
  std::lock_guard lock(g_mutex);
  std::vector<SteamEvent> out;
  out.swap(g_events);
  return out;
}

void NoteLaunchArguments(int argc, char** argv) {
  std::vector<std::string> args(argv, argv + argc);
  g_launch_lobby = ConnectLobbyArgument(args);
}

#if KUF_HAVE_STEAMWORKS

namespace {

// The flat API's functions, looked up in the library at run time; their types come from the SDK's own declarations
// (decltype), so a signature mismatch is a build error.
#define KUF_STEAM_FUNCTIONS(X)                                \
  X(SteamAPI_InitFlat)                                        \
  X(SteamAPI_Shutdown)                                        \
  X(SteamAPI_GetHSteamPipe)                                   \
  X(SteamAPI_ManualDispatch_Init)                             \
  X(SteamAPI_ManualDispatch_RunFrame)                         \
  X(SteamAPI_ManualDispatch_GetNextCallback)                  \
  X(SteamAPI_ManualDispatch_FreeLastCallback)                 \
  X(SteamAPI_ManualDispatch_GetAPICallResult)                 \
  X(SteamAPI_SteamMatchmaking_v009)                           \
  X(SteamAPI_SteamFriends_v018)                               \
  X(SteamAPI_SteamUser_v023)                                  \
  X(SteamAPI_SteamUtils_v011)                                 \
  X(SteamAPI_ISteamMatchmaking_CreateLobby)                   \
  X(SteamAPI_ISteamMatchmaking_SetLobbyData)                  \
  X(SteamAPI_ISteamMatchmaking_GetLobbyDataCount)             \
  X(SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex)           \
  X(SteamAPI_ISteamMatchmaking_RequestLobbyList)              \
  X(SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter) \
  X(SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter) \
  X(SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter) \
  X(SteamAPI_ISteamMatchmaking_GetLobbyByIndex)               \
  X(SteamAPI_ISteamMatchmaking_JoinLobby)                     \
  X(SteamAPI_ISteamMatchmaking_LeaveLobby)                    \
  X(SteamAPI_ISteamMatchmaking_GetNumLobbyMembers)            \
  X(SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit)           \
  X(SteamAPI_ISteamMatchmaking_GetLobbyOwner)                 \
  X(SteamAPI_ISteamMatchmaking_SetLobbyJoinable)              \
  X(SteamAPI_ISteamMatchmaking_SetLobbyMemberData)            \
  X(SteamAPI_ISteamMatchmaking_GetLobbyMemberData)            \
  X(SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog)   \
  X(SteamAPI_ISteamFriends_GetPersonaName)                    \
  X(SteamAPI_ISteamUser_GetSteamID)                           \
  X(SteamAPI_ISteamUtils_IsOverlayEnabled)

struct Api {
#define KUF_DECLARE(name) decltype(&::name) name = nullptr;
  KUF_STEAM_FUNCTIONS(KUF_DECLARE)
#undef KUF_DECLARE
};

void* g_library = nullptr;
Api g_api;
bool g_running = false;
HSteamPipe g_pipe = 0;
ISteamMatchmaking* g_mm = nullptr;
ISteamFriends* g_friends = nullptr;
ISteamUser* g_user = nullptr;
ISteamUtils* g_utils = nullptr;
uint64_t g_lobby = 0;  // the lobby we are in
enum class Call { kCreate, kList, kJoin };
std::map<SteamAPICall_t, Call> g_calls;

std::map<std::string, std::string> Metadata(uint64_t lobby) {
  std::map<std::string, std::string> out;
  const int n = g_api.SteamAPI_ISteamMatchmaking_GetLobbyDataCount(g_mm, lobby);
  for (int i = 0; i < n && i < 64; ++i) {
    char key[256] = {}, value[1024] = {};
    if (g_api.SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex(g_mm, lobby, i, key, sizeof(key), value, sizeof(value))) {
      out[key] = value;
    }
  }
  return out;
}

void HandleCallResult(Call call, const void* data, bool failed) {
  switch (call) {
    case Call::kCreate: {
      const auto* r = static_cast<const LobbyCreated_t*>(data);
      if (failed || r->m_eResult != k_EResultOK) {
        Post({SteamEvent::kCreateFailed, 0, {}, "Steam could not create the lobby (result " +
                                                     std::to_string(failed ? -1 : int(r->m_eResult)) + ")."});
        return;
      }
      g_lobby = r->m_ulSteamIDLobby;
      Post({SteamEvent::kCreated, g_lobby, {}, ""});
      return;
    }
    case Call::kList: {
      const auto* r = static_cast<const LobbyMatchList_t*>(data);
      SteamEvent e{SteamEvent::kList, 0, {}, ""};
      const uint32_t n = failed ? 0 : std::min<uint32_t>(r->m_nLobbiesMatching, 50);
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t id = g_api.SteamAPI_ISteamMatchmaking_GetLobbyByIndex(g_mm, int(i));
        const int members = g_api.SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(g_mm, id);
        const int limit = g_api.SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(g_mm, id);
        // Steam's filter already asked for this project's key; the metadata is checked again here (it is data
        // from other players: never trusted as it comes).
        if (auto info = InfoFromMetadata(Metadata(id), id, members, limit)) e.lobbies.push_back(*info);
      }
      Post(std::move(e));
      return;
    }
    case Call::kJoin: {
      const auto* r = static_cast<const LobbyEnter_t*>(data);
      if (failed || r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
        Post({SteamEvent::kEnterFailed, r ? r->m_ulSteamIDLobby : 0, {},
              "The Steam lobby could not be joined (it may be full, closed or friends-only)."});
        return;
      }
      g_lobby = r->m_ulSteamIDLobby;
      Post({SteamEvent::kEntered, g_lobby, {}, ""});
      return;
    }
  }
}

}  // namespace

bool Built() { return true; }

bool Start(uint32_t app_id, const std::string& library, std::string* error) {
  if (g_running) return true;
  if (app_id == 0) {
    *error = "Steam is off (no App ID configured).";
    return false;
  }
  if (library.empty()) {
    *error = "No Steam API library configured (--kuf_steam_api_library: libsteam_api.so from your Steamworks SDK).";
    return false;
  }
  g_library = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!g_library) {
    *error = std::string("The Steam API library could not be loaded: ") + dlerror();
    return false;
  }
  bool complete = true;
#define KUF_LOAD(name)                                                         \
  g_api.name = reinterpret_cast<decltype(g_api.name)>(dlsym(g_library, #name)); \
  complete &= g_api.name != nullptr;
  KUF_STEAM_FUNCTIONS(KUF_LOAD)
#undef KUF_LOAD
  if (!complete) {
    *error = "The Steam API library is not the version this game was built against (SDK 1.65).";
    dlclose(g_library);
    g_library = nullptr;
    return false;
  }
  // Steam takes the application from the environment (no steam_appid.txt is written anywhere).
  const std::string id = std::to_string(app_id);
  setenv("SteamAppId", id.c_str(), 1);
  setenv("SteamGameId", id.c_str(), 1);
  SteamErrMsg message = {};
  if (g_api.SteamAPI_InitFlat(&message) != k_ESteamAPIInitResult_OK) {
    *error = std::string("Steam did not start (is the Steam client running and signed in?): ") + message;
    dlclose(g_library);
    g_library = nullptr;
    return false;
  }
  g_api.SteamAPI_ManualDispatch_Init();
  g_pipe = g_api.SteamAPI_GetHSteamPipe();
  g_mm = g_api.SteamAPI_SteamMatchmaking_v009();
  g_friends = g_api.SteamAPI_SteamFriends_v018();
  g_user = g_api.SteamAPI_SteamUser_v023();
  g_utils = g_api.SteamAPI_SteamUtils_v011();
  if (!g_mm || !g_friends || !g_user || !g_utils) {
    *error = "Steam's interfaces are not available.";
    g_api.SteamAPI_Shutdown();
    return false;
  }
  g_running = true;
  if (g_launch_lobby) {  // started from an accepted invitation
    Post({SteamEvent::kInviteAccepted, *g_launch_lobby, {}, ""});
    g_launch_lobby.reset();
  }
  return true;
}

void Shutdown() {
  if (!g_running) return;
  LeaveLobby();
  g_api.SteamAPI_Shutdown();
  g_running = false;
  g_calls.clear();
}

bool Running() { return g_running; }

std::string PlayerName() {
  if (!g_running) return {};
  const char* n = g_api.SteamAPI_ISteamFriends_GetPersonaName(g_friends);
  return CleanName(n ? n : "", "Player");
}

void RunCallbacks() {
  if (!g_running) return;
  g_api.SteamAPI_ManualDispatch_RunFrame(g_pipe);
  CallbackMsg_t msg;
  while (g_api.SteamAPI_ManualDispatch_GetNextCallback(g_pipe, &msg)) {
    if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback) {
      const auto* done = reinterpret_cast<const SteamAPICallCompleted_t*>(msg.m_pubParam);
      const auto it = g_calls.find(done->m_hAsyncCall);
      if (it != g_calls.end() && done->m_cubParam > 0 && done->m_cubParam <= 4096) {
        std::vector<uint8_t> buf(done->m_cubParam);
        bool failed = false;
        const bool got = g_api.SteamAPI_ManualDispatch_GetAPICallResult(
            g_pipe, done->m_hAsyncCall, buf.data(), int(buf.size()), done->m_iCallback, &failed);
        HandleCallResult(it->second, got ? buf.data() : nullptr, failed || !got);
        g_calls.erase(it);
      }
    } else if (msg.m_iCallback == GameLobbyJoinRequested_t::k_iCallback) {
      const auto* r = reinterpret_cast<const GameLobbyJoinRequested_t*>(msg.m_pubParam);
      Post({SteamEvent::kInviteAccepted, r->m_steamIDLobby.ConvertToUint64(), {}, ""});
    } else if (msg.m_iCallback == LobbyChatUpdate_t::k_iCallback) {
      const auto* r = reinterpret_cast<const LobbyChatUpdate_t*>(msg.m_pubParam);
      if (r->m_ulSteamIDLobby == g_lobby) Post({SteamEvent::kMembersChanged, g_lobby, {}, ""});
    } else if (msg.m_iCallback == LobbyDataUpdate_t::k_iCallback) {
      const auto* r = reinterpret_cast<const LobbyDataUpdate_t*>(msg.m_pubParam);
      if (r->m_ulSteamIDLobby == g_lobby) Post({SteamEvent::kDataChanged, g_lobby, {}, ""});
    }
    g_api.SteamAPI_ManualDispatch_FreeLastCallback(g_pipe);
  }
}

bool CreateLobby(const LobbyInfo& info) {
  if (!g_running) return false;
  const SteamAPICall_t call = g_api.SteamAPI_ISteamMatchmaking_CreateLobby(
      g_mm, ELobbyType(LobbyTypeFor(info.visibility)), int(std::clamp<uint8_t>(info.max_players, kMinPlayers, kMaxPlayers)));
  if (call == k_uAPICallInvalid) return false;
  g_calls[call] = Call::kCreate;
  return true;
}

bool UpdateLobby(const LobbyInfo& info) {
  if (!g_running || !g_lobby) return false;
  if (g_api.SteamAPI_ISteamMatchmaking_GetLobbyOwner(g_mm, g_lobby) !=
      g_api.SteamAPI_ISteamUser_GetSteamID(g_user)) {
    return false;
  }
  bool ok = true;
  for (const auto& [k, v] : LobbyMetadata(info)) {
    ok &= g_api.SteamAPI_ISteamMatchmaking_SetLobbyData(g_mm, g_lobby, k.c_str(), v.c_str());
  }
  g_api.SteamAPI_ISteamMatchmaking_SetLobbyJoinable(g_mm, g_lobby, !info.started);
  return ok;
}

bool RequestLobbies() {
  if (!g_running) return false;
  g_api.SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(g_mm, kProjectKey, kProjectValue,
                                                                  k_ELobbyComparisonEqual);
  g_api.SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(
      g_mm, "protocol", std::to_string(kProtocolVersion).c_str(), k_ELobbyComparisonEqual);
  g_api.SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(g_mm, k_ELobbyDistanceFilterWorldwide);
  g_api.SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(g_mm, 50);
  const SteamAPICall_t call = g_api.SteamAPI_ISteamMatchmaking_RequestLobbyList(g_mm);
  if (call == k_uAPICallInvalid) return false;
  g_calls[call] = Call::kList;
  return true;
}

bool JoinLobby(uint64_t lobby) {
  if (!g_running || lobby == 0) return false;
  LeaveLobby();
  const SteamAPICall_t call = g_api.SteamAPI_ISteamMatchmaking_JoinLobby(g_mm, lobby);
  if (call == k_uAPICallInvalid) return false;
  g_calls[call] = Call::kJoin;
  return true;
}

void LeaveLobby() {
  if (!g_running || !g_lobby) return;
  g_api.SteamAPI_ISteamMatchmaking_LeaveLobby(g_mm, g_lobby);
  g_lobby = 0;
}

bool SetOwnInvite(const std::string& code) {
  if (!g_running || !g_lobby) return false;
  g_api.SteamAPI_ISteamMatchmaking_SetLobbyMemberData(g_mm, g_lobby, kInviteKey, code.c_str());
  return true;
}

std::string OwnerInvite() {
  if (!g_running || !g_lobby) return {};
  const uint64_t owner = g_api.SteamAPI_ISteamMatchmaking_GetLobbyOwner(g_mm, g_lobby);
  const char* v = g_api.SteamAPI_ISteamMatchmaking_GetLobbyMemberData(g_mm, g_lobby, owner, kInviteKey);
  return v ? std::string(v).substr(0, 64) : std::string();
}

bool OpenInviteDialog() {
  if (!g_running || !g_lobby || !g_api.SteamAPI_ISteamUtils_IsOverlayEnabled(g_utils)) return false;
  g_api.SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(g_friends, g_lobby);
  return true;
}

#else  // built without the Steamworks SDK

bool Built() { return false; }
bool Start(uint32_t, const std::string&, std::string* error) {
  *error = "This build has no Steam support (built without the Steamworks SDK).";
  return false;
}
void Shutdown() {}
bool Running() { return false; }
std::string PlayerName() { return {}; }
void RunCallbacks() {}
bool CreateLobby(const LobbyInfo&) { return false; }
bool RequestLobbies() { return false; }
bool JoinLobby(uint64_t) { return false; }
void LeaveLobby() {}
bool UpdateLobby(const LobbyInfo&) { return false; }
bool SetOwnInvite(const std::string&) { return false; }
std::string OwnerInvite() { return {}; }
bool OpenInviteDialog() { return false; }

#endif

}  // namespace kuf::net::steam
