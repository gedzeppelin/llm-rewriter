#pragma once

#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/Credentials.hpp"
#include "llm_rewriter/HttpTransport.hpp"

#include <chrono>
#include <string>

namespace llm_rewriter {

class IDiagnosticSink;

struct RewriteRequest {
  std::string input;
};

struct RewriteResult {
  bool ok = false;
  std::string text;
  std::string error;
  std::string request_id;
  std::string provider_request_id;
  std::string error_code;
  long http_status = 0;
  std::chrono::milliseconds duration{0};
  int estimated_input_tokens = 0;
  int max_output_tokens = 0;
};

RewriteResult RewriteWithLlm(const AppConfig& config,
                             const RewriteRequest& request,
                             IHttpTransport& transport,
                             IDiagnosticSink* diagnostics = nullptr);

RewriteResult RewriteWithLlm(const AppConfig& config,
                             const RewriteRequest& request,
                             IHttpTransport& transport,
                             CredentialResolver& credentials,
                             const CancellationToken* cancellation = nullptr,
                             IDiagnosticSink* diagnostics = nullptr);

std::string BuildLlmPayloadForTest(const AppConfig& config,
                                   const RewriteRequest& request,
                                   int max_output_tokens);

}  // namespace llm_rewriter
