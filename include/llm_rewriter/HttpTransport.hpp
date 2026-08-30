#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llm_rewriter {

struct HttpRequest {
  std::string url;
  std::string body;
  std::vector<std::string> headers;
  std::string request_id;
  std::chrono::milliseconds timeout{30000};
  std::string method = "POST";
  std::string content_type = "application/json";
  bool follow_redirects = false;
  std::size_t max_response_bytes = 64 * 1024 * 1024;
  std::shared_ptr<class CancellationToken> cancellation;
  const class CancellationToken* cancellation_token = nullptr;
};

struct HttpResponse {
  long status = 0;
  std::string body;
  std::string error;
  std::map<std::string, std::string> headers;
  std::chrono::milliseconds duration{0};
};

class IHttpTransport {
 public:
  virtual ~IHttpTransport() = default;
  virtual HttpResponse PostJson(const HttpRequest& request) = 0;

  // Generic requests are used by credential management.  Existing adapters
  // that only implement JSON POSTs remain source-compatible; OAuth fakes and
  // platform transports can override this seam for form and JSON requests.
  virtual HttpResponse Send(const HttpRequest& request) {
    if (request.method == "POST" && request.content_type == "application/json") {
      return PostJson(request);
    }
    HttpResponse response;
    response.error = "HTTP transport does not support this request";
    return response;
  }
};

// Construct the platform HTTP adapter for callers such as explicit OAuth
// management.  The backend itself remains portable; platform targets provide
// the definition.
std::unique_ptr<IHttpTransport> CreatePlatformHttpTransport();

}  // namespace llm_rewriter
