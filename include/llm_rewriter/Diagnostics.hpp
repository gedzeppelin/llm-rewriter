#pragma once

#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/LlmClient.hpp"

#include <filesystem>
#include <string>

namespace llm_rewriter {

// Diagnostics deliberately contain request metadata only.  They must never
// contain resolved credentials, request bodies, prompts, or model output.
struct DiagnosticEvent {
  std::string request_id;
  std::string provider;
  std::string api_format;
  std::string model;
  std::string endpoint;
  std::string provider_request_id;
  std::string outcome;
  std::string error;
  std::string error_code;
  long http_status = 0;
  std::chrono::milliseconds duration{0};
  int estimated_input_tokens = 0;
  int max_output_tokens = 0;
};

class IDiagnosticSink {
 public:
  virtual ~IDiagnosticSink() = default;
  virtual void Record(const DiagnosticEvent& event) = 0;
};

class JsonlDiagnosticSink final : public IDiagnosticSink {
 public:
  explicit JsonlDiagnosticSink(std::filesystem::path path);
  void Record(const DiagnosticEvent& event) override;

 private:
  std::filesystem::path path_;
};

void RecordDiagnostic(IDiagnosticSink* sink,
                      const AppConfig& config,
                      const std::string& endpoint,
                      const RewriteResult& result);

}  // namespace llm_rewriter
