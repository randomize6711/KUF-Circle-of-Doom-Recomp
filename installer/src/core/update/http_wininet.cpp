// The Windows updater's third way to the server (http_win.cpp): WinINet, Windows' other HTTP stack (the one Internet
// Options configure), with the system's proxy settings and certificate checks. No cache, no cookies, no redirects.
#if defined(_WIN32)

#include <windows.h>
#include <wininet.h>

#include <cwchar>

#include "core/update/http_transport_win.h"

namespace kuf::setup::update {
namespace {

class Response : public HttpResponse {
 public:
  Response(HINTERNET connect, HINTERNET request) : connect_(connect), request_(request) {}
  ~Response() override {
    InternetCloseHandle(request_);
    InternetCloseHandle(connect_);
  }
  bool Read(char* buf, DWORD size, DWORD* got) override { return InternetReadFile(request_, buf, size, got); }

 private:
  HINTERNET connect_, request_;
};

class WinInet : public HttpTransport {
 public:
  explicit WinInet(const wchar_t* agent) {
    session_ = InternetOpenW(agent, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
  }
  ~WinInet() override {
    if (session_) InternetCloseHandle(session_);
  }
  const char* Name() const override { return "WinINet"; }
  bool ok() const override { return session_ != nullptr; }

  std::unique_ptr<HttpResponse> Send(const HttpTarget& t, const std::wstring& headers, DWORD* err) override {
    HINTERNET connect =
        InternetConnectW(session_, t.host.c_str(), t.port, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    const DWORD flags = INTERNET_FLAG_NO_AUTO_REDIRECT | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
                        INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | (t.https ? INTERNET_FLAG_SECURE : 0);
    HINTERNET request =
        connect ? HttpOpenRequestW(connect, L"GET", t.path.c_str(), nullptr, nullptr, nullptr, flags, 0) : nullptr;
    if (!request) {
      *err = GetLastError();
      if (connect) InternetCloseHandle(connect);
      return nullptr;
    }
    if (!HttpSendRequestW(request, headers.c_str(), DWORD(-1L), nullptr, 0)) {
      *err = GetLastError();
      InternetCloseHandle(request);
      InternetCloseHandle(connect);
      return nullptr;
    }
    auto r = std::make_unique<Response>(connect, request);
    DWORD size = sizeof(r->status), index = 0;
    HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &r->status, &size, &index);
    wchar_t location[4096];
    DWORD len = sizeof(location);
    index = 0;
    if (HttpQueryInfoW(request, HTTP_QUERY_LOCATION, location, &len, &index)) {
      r->location.assign(location, wcsnlen(location, len / sizeof(wchar_t)));
    }
    wchar_t length_text[32];
    len = sizeof(length_text);
    index = 0;
    if (HttpQueryInfoW(request, HTTP_QUERY_CONTENT_LENGTH, length_text, &len, &index)) {
      r->length = std::wcstoull(length_text, nullptr, 10);
    }
    return r;
  }

 private:
  HINTERNET session_ = nullptr;
};

}  // namespace

std::unique_ptr<HttpTransport> MakeWinInetTransport(const wchar_t* agent) { return std::make_unique<WinInet>(agent); }

}  // namespace kuf::setup::update

#endif
