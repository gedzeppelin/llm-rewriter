#include "CurlHttpTransport.hpp"

#include "llm_rewriter/Credentials.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <string>

namespace llm_rewriter {
namespace {

struct WriteContext {
  std::string* body = nullptr;
  std::size_t max_bytes = 0;
  bool too_large = false;
};

size_t WriteHeaders(char* data, size_t size, size_t count, void* user_data) {
  auto* response = static_cast<HttpResponse*>(user_data);
  const std::string line(data, size * count);
  const auto separator = line.find(':');
  if (separator == std::string::npos) {
    return size * count;
  }
  auto name = line.substr(0, separator);
  auto value = line.substr(separator + 1);
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t')) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         (value.back() == '\r' || value.back() == '\n' ||
          value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  response->headers[name] = value;
  return size * count;
}

size_t WriteBody(char* data, size_t size, size_t count, void* user_data) {
  auto* context = static_cast<WriteContext*>(user_data);
  const auto bytes = size * count;
  if (bytes > context->max_bytes - std::min(context->max_bytes,
                                             context->body->size())) {
    context->too_large = true;
    return 0;
  }
  context->body->append(data, bytes);
  return bytes;
}

int CheckCancelled(void* user_data) {
  auto* request = static_cast<const HttpRequest*>(user_data);
  return (request->cancellation &&
              request->cancellation->IsCancelled()) ||
             (request->cancellation_token &&
              request->cancellation_token->IsCancelled())
             ? 1
             : 0;
}

bool IsRemoteInsecureUrl(const std::string& url) {
  if (url.size() < 7) {
    return false;
  }
  auto scheme = url.substr(0, 7);
  std::ranges::transform(scheme, scheme.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  if (scheme != "http://") return false;
  const auto authority_start = 7U;
  const auto authority_end = url.find('/', authority_start);
  const auto authority = url.substr(
      authority_start, authority_end == std::string::npos
                           ? std::string::npos
                           : authority_end - authority_start);
  std::string host;
  if (!authority.empty() && authority.front() == '[') {
    const auto close = authority.find(']');
    host = close == std::string::npos ? authority
                                      : authority.substr(0, close + 1);
  } else {
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
  }
  return host != "127.0.0.1" && host != "localhost" && host != "[::1]";
}

}  // namespace

HttpResponse CurlHttpTransport::Send(const HttpRequest& request) {
  HttpResponse response;
  const auto started = std::chrono::steady_clock::now();
  if (IsRemoteInsecureUrl(request.url)) {
    response.error = "remote HTTP endpoints must use TLS";
    return response;
  }
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                           curl_easy_cleanup);
  if (!curl) {
    response.error = "failed to initialize libcurl";
    return response;
  }

  curl_slist* raw_headers = nullptr;
  const auto content_type = "Content-Type: " + request.content_type;
  raw_headers = curl_slist_append(raw_headers, content_type.c_str());
  for (const auto& header : request.headers) {
    raw_headers = curl_slist_append(raw_headers, header.c_str());
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      raw_headers, curl_slist_free_all);

  curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  if (request.method == "POST") {
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  } else {
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str());
  }
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(request.body.size()));
  WriteContext write_context{.body = &response.body,
                              .max_bytes = request.max_response_bytes};
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteBody);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &write_context);
  curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, WriteHeaders);
  curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                   static_cast<long>(request.timeout.count()));
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION,
                   request.follow_redirects ? 1L : 0L);
  curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, CheckCancelled);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA,
                   const_cast<HttpRequest*>(&request));
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

  const auto code = curl_easy_perform(curl.get());
  response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
  if (code != CURLE_OK) {
    response.error = write_context.too_large
                         ? "HTTP response body exceeds safety limit"
                         : (code == CURLE_ABORTED_BY_CALLBACK &&
                                    ((request.cancellation &&
                                      request.cancellation->IsCancelled()) ||
                                     (request.cancellation_token &&
                                      request.cancellation_token->IsCancelled()))
                                ? "HTTP request cancelled"
                                : curl_easy_strerror(code));
  }
  return response;
}

HttpResponse CurlHttpTransport::PostJson(const HttpRequest& request) {
  auto copy = request;
  copy.method = "POST";
  copy.content_type = "application/json";
  return Send(copy);
}

std::unique_ptr<IHttpTransport> CreatePlatformHttpTransport() {
  return std::make_unique<CurlHttpTransport>();
}

}  // namespace llm_rewriter
