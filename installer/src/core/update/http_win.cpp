// The Windows updater's downloads (M17G): WinHTTP, HTTPS with the system's certificate checks. Redirects are followed
// by hand, each one checked: only HTTPS to GitHub's hosts (as tools/updater/kuf_update.py), or, for tests, only the
// configured loopback origin (http://127.0.0.1:<port>).
#if defined(_WIN32)

#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <vector>

#include "core/platform.h"
#include "core/update/json.h"
#include "core/update/updater.h"

namespace kuf::setup::update {
namespace {

constexpr const char* kGitHubApi = "https://api.github.com";
const char* const kAllowedHosts[] = {"api.github.com", "github.com", "objects.githubusercontent.com",
                                     "release-assets.githubusercontent.com", "codeload.github.com"};
constexpr uint64_t kMaxJson = 8 << 20;

std::wstring Wide(const std::string& s) { return PathFromUtf8(s).wstring(); }
std::string Narrow(const std::wstring& s) { return PathUtf8(fs::path(s)); }

struct Url {
  bool https = false;
  std::wstring host, path;
  INTERNET_PORT port = 0;
};

bool Crack(const std::string& url, Url* out) {
  const std::wstring w = Wide(url);
  URL_COMPONENTS c{};
  c.dwStructSize = sizeof(c);
  wchar_t host[256], path[4096];
  c.lpszHostName = host;
  c.dwHostNameLength = 256;
  c.lpszUrlPath = path;
  c.dwUrlPathLength = 4096;
  wchar_t extra[4096];
  c.lpszExtraInfo = extra;
  c.dwExtraInfoLength = 4096;
  if (!WinHttpCrackUrl(w.c_str(), DWORD(w.size()), 0, &c)) return false;
  out->https = c.nScheme == INTERNET_SCHEME_HTTPS;
  out->host.assign(host, c.dwHostNameLength);
  out->path.assign(path, c.dwUrlPathLength);
  out->path.append(extra, c.dwExtraInfoLength);
  out->port = c.nPort;
  return true;
}

class Http : public ReleaseSource {
 public:
  Http(std::string repo, std::string api) : repo_(std::move(repo)), api_(std::move(api)) {
    session_ = WinHttpOpen(L"kuf-cod-recomp-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
      DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
      WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    Url u;
    loopback_ = Crack(api_, &u) && !u.https && (u.host == L"127.0.0.1" || u.host == L"[::1]");
    if (loopback_) loopback_port_ = u.port;
  }
  ~Http() override {
    if (session_) WinHttpCloseHandle(session_);
  }
  bool ok() const { return session_ != nullptr; }

  bool Releases(std::vector<Release>* out, std::string* why) override {
    std::string body;
    if (!Get(api_ + "/repos/" + repo_ + "/releases?per_page=30", kMaxJson, "application/vnd.github+json", &body,
             nullptr, nullptr, nullptr, why)) {
      return false;
    }
    const auto json = ParseJson(body);
    if (!json || json->type != Json::kArray) {
      *why = "GitHub's answer is not a release list";
      return false;
    }
    for (const Json& r : json->items) {
      Release rel;
      rel.tag = r.String("tag_name");
      rel.html_url = r.String("html_url");
      rel.published = r.String("published_at");
      rel.draft = r.Bool("draft");
      rel.prerelease = r.Bool("prerelease");
      if (const Json* assets = r.Get("assets"); assets && assets->type == Json::kArray) {
        for (const Json& a : assets->items) {
          rel.assets.push_back({a.String("name"), a.String("browser_download_url"), a.Integer("size").value_or(0)});
        }
      }
      out->push_back(std::move(rel));
    }
    return true;
  }
  bool Fetch(const Asset& a, uint64_t limit, std::string* data, std::string* why) override {
    return Get(a.url, limit, "application/octet-stream", data, nullptr, nullptr, nullptr, why);
  }
  bool Download(const Asset& a, const fs::path& dest, uint64_t limit, const DownloadProgress& progress,
                const std::atomic<bool>& cancel, std::string* why) override {
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
      *why = "cannot write " + PathUtf8(dest);
      return false;
    }
    return Get(a.url, limit, "application/octet-stream", nullptr, &out, &progress, &cancel, why);
  }

 private:
  // An allowed place to connect to: GitHub over HTTPS, or the loopback test origin.
  bool Allowed(const Url& u) const {
    if (loopback_) return !u.https && u.host == L"127.0.0.1" && u.port == loopback_port_;
    if (!u.https) return false;
    for (const char* h : kAllowedHosts) {
      if (u.host == Wide(h)) return true;
    }
    return false;
  }

  bool Get(std::string url, uint64_t limit, const char* accept, std::string* body, std::ofstream* file,
           const DownloadProgress* progress, const std::atomic<bool>* cancel, std::string* why) {
    for (int hop = 0; hop < 6; ++hop) {
      Url u;
      if (!Crack(url, &u) || !Allowed(u)) {
        *why = "refused to download from " + url + " (only " + (loopback_ ? api_ : std::string("GitHub over HTTPS")) +
               ")";
        return false;
      }
      HINTERNET connect = WinHttpConnect(session_, u.host.c_str(), u.port, 0);
      HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                       WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)
                                  : nullptr;
      const auto close = [&] {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
      };
      if (!request) {
        close();
        *why = "cannot connect to " + Narrow(u.host);
        return false;
      }
      DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
      WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
      const std::wstring headers = L"Accept: " + Wide(accept) + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
      if (!WinHttpSendRequest(request, headers.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
          !WinHttpReceiveResponse(request, nullptr)) {
        const DWORD err = GetLastError();
        close();
        *why = "the download from " + Narrow(u.host) + " failed (error " + std::to_string(err) +
               (err == ERROR_WINHTTP_SECURE_FAILURE ? ": the server's certificate was not accepted" : "") + ")";
        return false;
      }
      DWORD status = 0, size = sizeof(status);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &size, WINHTTP_NO_HEADER_INDEX);
      if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        wchar_t location[4096];
        DWORD len = sizeof(location);
        const bool has = WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location,
                                             &len, WINHTTP_NO_HEADER_INDEX);
        close();
        if (!has) {
          *why = "a redirect without a location";
          return false;
        }
        url = Narrow(std::wstring(location, len / sizeof(wchar_t)));
        continue;
      }
      if (status != 200) {
        close();
        *why = "the server answered " + std::to_string(status) + " for " + url +
               (status == 403 ? " (GitHub's rate limit? try again later)" : "");
        return false;
      }
      wchar_t length_text[32];
      DWORD llen = sizeof(length_text);
      uint64_t total = 0;
      if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &llen,
                              WINHTTP_NO_HEADER_INDEX)) {
        total = std::wcstoull(length_text, nullptr, 10);
        if (total > limit) {
          close();
          *why = url + ": larger than " + std::to_string(limit) + " bytes";
          return false;
        }
      }
      uint64_t done = 0;
      std::vector<char> buf(1 << 16);
      for (;;) {
        if (cancel && *cancel) {
          close();
          *why = "cancelled";
          return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request, buf.data(), DWORD(buf.size()), &got)) {
          close();
          *why = "the download was interrupted";
          return false;
        }
        if (got == 0) break;
        done += got;
        if (done > limit) {
          close();
          *why = url + ": larger than " + std::to_string(limit) + " bytes";
          return false;
        }
        if (body) body->append(buf.data(), got);
        if (file) file->write(buf.data(), got);
        if (progress && *progress) (*progress)(done, total ? total : limit);
      }
      close();
      if (total && done != total) {
        *why = "the download was incomplete";
        return false;
      }
      return !file || bool(*file) || (*why = "cannot write the download", false);
    }
    *why = "too many redirects";
    return false;
  }

  std::string repo_, api_;
  HINTERNET session_ = nullptr;
  bool loopback_ = false;
  INTERNET_PORT loopback_port_ = 0;
};

}  // namespace

std::unique_ptr<ReleaseSource> HttpSource(const std::string& repo, const std::string& api, std::string* why) {
  const std::string origin = api.empty() ? kGitHubApi : api;
  Url u;
  if (origin != kGitHubApi && (!Crack(origin, &u) || u.https || u.host != L"127.0.0.1")) {
    *why = "api must be " + std::string(kGitHubApi) + " or http://127.0.0.1:<port> (a local test server)";
    return nullptr;
  }
  auto h = std::make_unique<Http>(repo, origin);
  if (!h->ok()) {
    *why = "WinHTTP could not be started";
    return nullptr;
  }
  return h;
}

}  // namespace kuf::setup::update

#endif
