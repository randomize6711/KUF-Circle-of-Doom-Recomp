// The Windows updater's downloads (M17G): HTTPS with the system's certificate checks. Redirects are followed by hand,
// each one checked: only HTTPS to GitHub's hosts (as tools/updater/kuf_update.py), or, for tests, only the configured
// loopback origin (http://127.0.0.1:<port>).
//
// Three ways to the server, tried in this order until one gets an HTTP answer (0.9.0-rc.3: a player's Windows 11 got
// "error 12152", the answer cannot be parsed, from WinHTTP with TLS 1.2 and 1.3 forced, while the same program under
// Wine got through): WinHTTP with Windows' own protocol settings, WinHTTP with TLS 1.2 only, and WinINet (Windows'
// other HTTP stack, the one Internet Options configure). Only a failure to get an answer moves on to the next way; an
// HTTP answer (403, 404, a hostile redirect) is final. The way that worked is kept for the following downloads.
// KUF_UPDATE_HTTP=winhttp|winhttp-tls12|wininet uses only that way (tests).
#if defined(_WIN32)

#include <windows.h>
#include <winhttp.h>

#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <memory>
#include <vector>

#include "core/platform.h"
#include "core/update/http_transport_win.h"
#include "core/update/json.h"
#include "core/update/updater.h"

namespace kuf::setup::update {
namespace {

constexpr const char* kGitHubApi = "https://api.github.com";
const char* const kAllowedHosts[] = {"api.github.com", "github.com", "objects.githubusercontent.com",
                                     "release-assets.githubusercontent.com", "codeload.github.com"};
constexpr uint64_t kMaxJson = 8 << 20;
constexpr const wchar_t* kAgent = L"kuf-cod-recomp-updater";

std::wstring Wide(const std::string& s) { return PathFromUtf8(s).wstring(); }
std::string Narrow(const std::wstring& s) { return PathUtf8(fs::path(s)); }

using Url = HttpTarget;

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

// ---- WinHTTP ----
class WinHttpResponse : public HttpResponse {
 public:
  WinHttpResponse(HINTERNET connect, HINTERNET request) : connect_(connect), request_(request) {}
  ~WinHttpResponse() override {
    WinHttpCloseHandle(request_);
    WinHttpCloseHandle(connect_);
  }
  bool Read(char* buf, DWORD size, DWORD* got) override { return WinHttpReadData(request_, buf, size, got); }

 private:
  HINTERNET connect_, request_;
};

class WinHttp : public HttpTransport {
 public:
  // protocols: 0 leaves Windows' own choice (TLS 1.3 where the system enables it).
  WinHttp(const char* name, DWORD protocols) : name_(name) {
    session_ = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                           0);
    if (!session_) {  // before Windows 8.1 (not supported, but no reason to fail): the default proxy settings
      session_ = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session_ && protocols) WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
  ~WinHttp() override {
    if (session_) WinHttpCloseHandle(session_);
  }
  const char* Name() const override { return name_; }
  bool ok() const override { return session_ != nullptr; }

  std::unique_ptr<HttpResponse> Send(const Url& u, const std::wstring& headers, DWORD* err) override {
    HINTERNET connect = WinHttpConnect(session_, u.host.c_str(), u.port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (!request) {
      *err = GetLastError();
      if (connect) WinHttpCloseHandle(connect);
      return nullptr;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!WinHttpSendRequest(request, headers.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
      *err = GetLastError();
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      return nullptr;
    }
    auto r = std::make_unique<WinHttpResponse>(connect, request);
    DWORD size = sizeof(r->status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &r->status, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t location[4096];
    DWORD len = sizeof(location);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location, &len,
                            WINHTTP_NO_HEADER_INDEX)) {
      r->location.assign(location, wcsnlen(location, len / sizeof(wchar_t)));
    }
    wchar_t length_text[32];
    len = sizeof(length_text);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &len,
                            WINHTTP_NO_HEADER_INDEX)) {
      r->length = std::wcstoull(length_text, nullptr, 10);
    }
    return r;
  }

 private:
  const char* name_;
  HINTERNET session_ = nullptr;
};

// WinHTTP's and WinINet's error numbers agree for these (winhttp.h, wininet.h).
std::string ErrorText(DWORD err) {
  std::string t = "error " + std::to_string(err);
  if (err == ERROR_WINHTTP_SECURE_FAILURE || (err >= 12037 && err <= 12045) || err == 12057) {
    t += ": the server's certificate was not accepted";
  } else if (err == ERROR_WINHTTP_INVALID_SERVER_RESPONSE) {
    t += ": the answer could not be read";
  } else if (err == ERROR_WINHTTP_TIMEOUT) {
    t += ": no answer in time";
  } else if (err == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
    t += ": the name was not found (no internet connection?)";
  } else if (err == ERROR_WINHTTP_CANNOT_CONNECT) {
    t += ": cannot connect";
  }
  return t;
}

class Http : public ReleaseSource {
 public:
  Http(std::string repo, std::string api) : repo_(std::move(repo)), api_(std::move(api)) {
    const char* only = std::getenv("KUF_UPDATE_HTTP");
    const std::string want = only ? only : "";
    const auto add = [&](const std::string& key, std::unique_ptr<HttpTransport> t) {
      if ((want.empty() || want == key) && t->ok()) ways_.push_back(std::move(t));
    };
    add("winhttp", std::make_unique<WinHttp>("WinHTTP", 0));
    add("winhttp-tls12", std::make_unique<WinHttp>("WinHTTP (TLS 1.2)", WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2));
    add("wininet", MakeWinInetTransport(kAgent));
    Url u;
    loopback_ = Crack(api_, &u) && !u.https && (u.host == L"127.0.0.1" || u.host == L"[::1]");
    if (loopback_) loopback_port_ = u.port;
  }
  bool ok() const { return !ways_.empty(); }

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

  // One download over the ways in turn, starting with the one that worked last.
  bool Get(const std::string& url, uint64_t limit, const char* accept, std::string* body, std::ofstream* file,
           const DownloadProgress* progress, const std::atomic<bool>* cancel, std::string* why) {
    std::string failures;
    for (size_t n = 0; n < ways_.size(); ++n) {
      const size_t i = (preferred_ + n) % ways_.size();
      bool answered = false;
      const bool ok = GetWith(*ways_[i], url, limit, accept, body, file, progress, cancel, &answered, why);
      if (answered) {
        preferred_ = i;
        return ok;
      }
      failures += (failures.empty() ? "" : "; ") + std::string(ways_[i]->Name()) + ": " + *why;
    }
    *why = failures.empty() ? "no way to download (KUF_UPDATE_HTTP?)" : "no answer from GitHub (" + failures + ")";
    return false;
  }

  // *answered: an HTTP answer came (then the result is final, whatever it is).
  bool GetWith(HttpTransport& t, std::string url, uint64_t limit, const char* accept, std::string* body,
               std::ofstream* file, const DownloadProgress* progress, const std::atomic<bool>* cancel, bool* answered,
               std::string* why) {
    const std::wstring headers = L"Accept: " + Wide(accept) + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    for (int hop = 0; hop < 6; ++hop) {
      Url u;
      if (!Crack(url, &u) || !Allowed(u)) {
        *answered = true;
        *why = "refused to download from " + url + " (only " + (loopback_ ? api_ : std::string("GitHub over HTTPS")) +
               ")";
        return false;
      }
      DWORD err = 0;
      const auto r = t.Send(u, headers, &err);
      if (!r) {
        *why = "the download from " + Narrow(u.host) + " failed (" + ErrorText(err) + ")";
        return false;  // not answered: the next way may get through
      }
      *answered = true;
      if (r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308) {
        if (r->location.empty()) {
          *why = "a redirect without a location";
          return false;
        }
        url = Narrow(r->location);
        continue;
      }
      if (r->status != 200) {
        *why = "the server answered " + std::to_string(r->status) + " for " + url +
               (r->status == 403 ? " (GitHub's rate limit? try again later)" : "");
        return false;
      }
      const uint64_t total = r->length;
      if (total > limit) {
        *why = url + ": larger than " + std::to_string(limit) + " bytes";
        return false;
      }
      uint64_t done = 0;
      std::vector<char> buf(1 << 16);
      for (;;) {
        if (cancel && *cancel) {
          *why = "cancelled";
          return false;
        }
        DWORD got = 0;
        if (!r->Read(buf.data(), DWORD(buf.size()), &got)) {
          *why = "the download was interrupted (" + ErrorText(GetLastError()) + ")";
          return false;
        }
        if (got == 0) break;
        done += got;
        if (done > limit) {
          *why = url + ": larger than " + std::to_string(limit) + " bytes";
          return false;
        }
        if (body) body->append(buf.data(), got);
        if (file) file->write(buf.data(), got);
        if (progress && *progress) (*progress)(done, total ? total : limit);
      }
      if (total && done != total) {
        *why = "the download was incomplete";
        return false;
      }
      return !file || bool(*file) || (*why = "cannot write the download", false);
    }
    *answered = true;
    *why = "too many redirects";
    return false;
  }

  std::string repo_, api_;
  std::vector<std::unique_ptr<HttpTransport>> ways_;
  size_t preferred_ = 0;
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
    *why = "neither WinHTTP nor WinINet could be started";
    return nullptr;
  }
  return h;
}

}  // namespace kuf::setup::update

#endif
