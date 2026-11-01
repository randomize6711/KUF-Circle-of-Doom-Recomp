#include "coop/steam_provider.h"

namespace kuf::coop {
namespace {

namespace steam = net::steam;

class RealSteam : public SteamApi {
 public:
  RealSteam(uint32_t app_id, const std::string& library) {
    if (!steam::Built()) {
      reason_ = "This build has no Steam support.";
    } else if (app_id == 0) {
      reason_ = "Steam is off (no App ID configured).";
    } else if (!steam::Start(app_id, library, &reason_)) {
      // reason_ set by Start
    }
  }
  bool Running() const override { return steam::Running(); }
  std::string Unavailable() const override { return reason_; }
  bool CreateLobby(const net::LobbyInfo& info) override { return steam::CreateLobby(info); }
  bool UpdateLobby(const net::LobbyInfo& info) override { return steam::UpdateLobby(info); }
  bool SetOwnInvite(const std::string& code) override { return steam::SetOwnInvite(code); }
  std::string OwnerInvite() override { return steam::OwnerInvite(); }
  bool RequestLobbies() override { return steam::RequestLobbies(); }
  bool JoinLobby(uint64_t lobby) override { return steam::JoinLobby(lobby); }
  void LeaveLobby() override { steam::LeaveLobby(); }
  bool OpenInviteDialog() override { return steam::OpenInviteDialog(); }
  void RunCallbacks() override { steam::RunCallbacks(); }
  std::vector<steam::SteamEvent> TakeEvents() override { return steam::TakeEvents(); }

 private:
  std::string reason_;
};

class SteamProvider : public InviteProvider {
 public:
  explicit SteamProvider(std::unique_ptr<SteamApi> api) : api_(std::move(api)) {}

  ProviderStatus Status() const override {
    ProviderStatus s;
    s.backend = net::Backend::kSteam;
    s.name = "Steam";
    s.available = api_->Running();
    s.reason = s.available ? "" : api_->Unavailable();
    s.can_invite = s.available;
    return s;
  }

  void Announce(const LobbySettings& settings, const net::LobbyInfo& info, const std::string& code) override {
    if (!api_->Running() || settings.backend != net::Backend::kSteam) return;
    info_ = info;
    info_.backend = net::Backend::kSteam;
    code_ = code;
    if (!own_lobby_ && !creating_) {
      creating_ = api_->CreateLobby(info_);
      if (!creating_) Error("Steam could not create the lobby.");
      return;
    }
    if (own_lobby_) Publish();
  }

  void Withdraw() override {
    if (own_lobby_ || entered_ || creating_ || resolving_) api_->LeaveLobby();
    own_lobby_ = entered_ = creating_ = false;
    resolving_ = 0;
  }

  void RequestList() override {
    if (!api_->RequestLobbies()) {
      ProviderEvent e;
      e.kind = ProviderEvent::kList;  // an empty answer, so the refresh ends
      events_.push_back(e);
    }
  }

  bool OpenInviteDialog(std::string* error) override {
    if (!own_lobby_) {
      *error = "The Steam lobby is not ready yet.";
      return false;
    }
    if (!api_->OpenInviteDialog()) {
      *error = "The Steam overlay is not available (enable it in Steam's settings).";
      return false;
    }
    return true;
  }

  void Resolve(uint64_t lobby) override {
    resolving_ = lobby;
    entered_ = false;
    if (!api_->JoinLobby(lobby)) {
      resolving_ = 0;
      Error("Steam could not enter that lobby.");
    }
  }

  std::vector<ProviderEvent> Poll() override {
    api_->RunCallbacks();
    for (const steam::SteamEvent& s : api_->TakeEvents()) Handle(s);
    if (resolving_ && entered_) TryResolve();
    std::vector<ProviderEvent> out;
    out.swap(events_);
    return out;
  }

 private:
  void Error(const std::string& text) {
    ProviderEvent e;
    e.kind = ProviderEvent::kError;
    e.text = text;
    events_.push_back(e);
  }
  void Publish() {
    api_->UpdateLobby(info_);
    if (!code_.empty()) api_->SetOwnInvite(code_);
  }
  void TryResolve() {
    const std::string code = api_->OwnerInvite();
    if (code.empty()) return;  // the owner's data has not arrived yet (kDataChanged)
    ProviderEvent e;
    e.kind = ProviderEvent::kResolved;
    e.provider_lobby = resolving_;
    e.code = code;
    events_.push_back(e);
    resolving_ = 0;
  }
  void Handle(const steam::SteamEvent& s) {
    switch (s.kind) {
      case steam::SteamEvent::kCreated:
        creating_ = false;
        own_lobby_ = true;
        Publish();
        break;
      case steam::SteamEvent::kCreateFailed:
        creating_ = false;
        Error(s.text.empty() ? "Steam could not create the lobby." : s.text);
        break;
      case steam::SteamEvent::kList: {
        ProviderEvent e;
        e.kind = ProviderEvent::kList;
        for (const net::LobbyInfo& info : s.lobbies) {
          LobbyEntry l;
          l.info = info;
          l.backend = net::Backend::kSteam;
          l.provider_lobby = info.id;
          l.joinable = !info.started && info.players < info.max_players;
          l.reason = info.started ? "The game has started." : l.joinable ? "" : "The lobby is full.";
          e.entries.push_back(l);
        }
        events_.push_back(e);
        break;
      }
      case steam::SteamEvent::kEntered:
        if (s.lobby == resolving_) entered_ = true;
        break;
      case steam::SteamEvent::kEnterFailed:
        if (resolving_) {
          resolving_ = 0;
          Error(s.text.empty() ? "The Steam lobby could not be joined." : s.text);
        }
        break;
      case steam::SteamEvent::kDataChanged:
      case steam::SteamEvent::kMembersChanged:
        break;  // TryResolve runs after the events
      case steam::SteamEvent::kInviteAccepted: {
        ProviderEvent e;
        e.kind = ProviderEvent::kInvite;
        e.provider_lobby = s.lobby;
        events_.push_back(e);
        break;
      }
    }
  }

  std::unique_ptr<SteamApi> api_;
  net::LobbyInfo info_;
  std::string code_;
  bool creating_ = false, own_lobby_ = false, entered_ = false;
  uint64_t resolving_ = 0;
  std::vector<ProviderEvent> events_;
};

}  // namespace

std::unique_ptr<SteamApi> MakeSteamApi(uint32_t app_id, const std::string& library) {
  return std::make_unique<RealSteam>(app_id, library);
}

std::unique_ptr<InviteProvider> MakeSteamProvider(std::unique_ptr<SteamApi> api) {
  return std::make_unique<SteamProvider>(std::move(api));
}

}  // namespace kuf::coop
