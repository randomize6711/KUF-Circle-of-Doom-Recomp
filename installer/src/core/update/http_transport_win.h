// The Windows updater's ways to the server (http_win.cpp): one request, its answer's status and headers, then the body
// in pieces. WinHTTP's and WinINet's headers cannot be included together, so each lives in its own file.
#pragma once
#if defined(_WIN32)

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace kuf::setup::update {

struct HttpTarget {
  bool https = false;
  std::wstring host, path;  // path with its query
  unsigned short port = 0;
};

class HttpResponse {
 public:
  virtual ~HttpResponse() = default;
  DWORD status = 0;
  std::wstring location;  // for a redirect; empty when there is none
  uint64_t length = 0;    // Content-Length, 0 when unknown
  virtual bool Read(char* buf, DWORD size, DWORD* got) = 0;
};

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual const char* Name() const = 0;
  virtual bool ok() const = 0;
  // nullptr with *err set when no HTTP answer came; redirects are never followed.
  virtual std::unique_ptr<HttpResponse> Send(const HttpTarget& t, const std::wstring& headers, DWORD* err) = 0;
};

std::unique_ptr<HttpTransport> MakeWinInetTransport(const wchar_t* agent);  // http_wininet.cpp

}  // namespace kuf::setup::update

#endif
