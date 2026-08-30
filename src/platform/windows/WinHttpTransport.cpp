#include "WinHttpTransport.hpp"

#include "llm_rewriter/Credentials.hpp"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <limits>
#include <memory>
#include <string>

namespace llm_rewriter {
namespace {

struct WinHttpHandleDeleter {
  void operator()(void* handle) const {
    if (handle != nullptr) {
      WinHttpCloseHandle(handle);
    }
  }
};

using WinHttpHandle = std::unique_ptr<void, WinHttpHandleDeleter>;

std::wstring Wide(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      output.data(), size);
  return output;
}

std::string WindowsError(const char* operation) {
  return std::string{operation} + " failed with Windows error " +
         std::to_string(GetLastError());
}

}  // namespace

HttpResponse WinHttpTransport::Send(const HttpRequest& request) {
  HttpResponse response;
  const auto started = std::chrono::steady_clock::now();
  const auto url = Wide(request.url);
  URL_COMPONENTS parts{.dwStructSize = sizeof(URL_COMPONENTS)};
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    response.error = WindowsError("WinHttpCrackUrl");
    return response;
  }

  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  WinHttpHandle session(
      WinHttpOpen(L"llm-rewriter/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    response.error = WindowsError("WinHttpOpen");
    return response;
  }
  const int timeout = static_cast<int>(request.timeout.count());
  WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);

  WinHttpHandle connection(
      WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
  if (!connection) {
    response.error = WindowsError("WinHttpConnect");
    return response;
  }

  const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS
                          ? WINHTTP_FLAG_SECURE
                          : 0;
  if (parts.nScheme != INTERNET_SCHEME_HTTPS &&
      host != L"127.0.0.1" && host != L"localhost" && host != L"[::1]" &&
      host != L"::1") {
    response.error = "remote HTTP endpoints must use TLS";
    return response;
  }
  WinHttpHandle http_request(WinHttpOpenRequest(
      connection.get(), Wide(request.method).c_str(), path.c_str(), nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
  if (!http_request) {
    response.error = WindowsError("WinHttpOpenRequest");
    return response;
  }

  std::wstring headers = L"Content-Type: " + Wide(request.content_type) + L"\r\n";
  for (const auto& header : request.headers) {
    headers += Wide(header);
    headers += L"\r\n";
  }
  if (request.body.size() > std::numeric_limits<DWORD>::max()) {
    response.error = "HTTP request body is too large";
    return response;
  }
  const auto body_size = static_cast<DWORD>(request.body.size());
  DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (request.follow_redirects) {
    redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  }
  WinHttpSetOption(http_request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                   &redirect_policy, sizeof(redirect_policy));
  if ((request.cancellation &&
       request.cancellation->IsCancelled()) ||
      (request.cancellation_token &&
       request.cancellation_token->IsCancelled())) {
    response.error = "HTTP request cancelled";
    return response;
  }
  if (!WinHttpSendRequest(http_request.get(), headers.c_str(),
                          static_cast<DWORD>(headers.size()),
                          const_cast<char*>(request.body.data()), body_size,
                          body_size, 0) ||
      !WinHttpReceiveResponse(http_request.get(), nullptr)) {
    response.error = WindowsError("WinHTTP request");
    return response;
  }
  DWORD status_size = sizeof(response.status);
  WinHttpQueryHeaders(http_request.get(),
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &response.status,
                      &status_size, WINHTTP_NO_HEADER_INDEX);

  for (const auto* name : {L"x-request-id", L"request-id"}) {
    DWORD size = 0;
    WinHttpQueryHeaders(http_request.get(), WINHTTP_QUERY_CUSTOM, name,
                        nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
      continue;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(http_request.get(), WINHTTP_QUERY_CUSTOM, name,
                             value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
      continue;
    }
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
      value.pop_back();
    }
    if (!value.empty()) {
      response.headers["x-request-id"] =
          std::string(value.begin(), value.end());
      break;
    }
  }

  for (;;) {
    if ((request.cancellation &&
         request.cancellation->IsCancelled()) ||
        (request.cancellation_token &&
         request.cancellation_token->IsCancelled())) {
      response.error = "HTTP request cancelled";
      return response;
    }
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(http_request.get(), &available)) {
      response.error = WindowsError("WinHttpQueryDataAvailable");
      return response;
    }
    if (available == 0) {
      break;
    }
    const auto offset = response.body.size();
    if (available > request.max_response_bytes ||
        offset > request.max_response_bytes - available) {
      response.error = "HTTP response body exceeds safety limit";
      return response;
    }
    response.body.resize(offset + available);
    DWORD read = 0;
    if (!WinHttpReadData(http_request.get(), response.body.data() + offset,
                         available, &read)) {
      response.error = WindowsError("WinHttpReadData");
      return response;
    }
    response.body.resize(offset + read);
  }
  response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  return response;
}

HttpResponse WinHttpTransport::PostJson(const HttpRequest& request) {
  auto copy = request;
  copy.method = "POST";
  copy.content_type = "application/json";
  return Send(copy);
}

std::unique_ptr<IHttpTransport> CreatePlatformHttpTransport() {
  return std::make_unique<WinHttpTransport>();
}

}  // namespace llm_rewriter
