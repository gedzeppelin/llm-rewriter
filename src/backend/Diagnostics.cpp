#include "llm_rewriter/Diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace llm_rewriter {
namespace {

std::string DiagnosticApiFormat(const AppConfig& config) {
  std::string provider = config.provider;
  std::ranges::transform(provider, provider.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  if (provider == "codex") {
    return "openai_responses";
  }
  return ToString(config.api_format);
}

std::string SafeEndpoint(std::string endpoint) {
  if (const auto query = endpoint.find('?'); query != std::string::npos) {
    endpoint.erase(query);
  }
  const auto scheme_end = endpoint.find("://");
  if (scheme_end != std::string::npos) {
    const auto authority_start = scheme_end + 3;
    const auto authority_end = endpoint.find('/', authority_start);
    const auto at = endpoint.find('@', authority_start);
    if (at != std::string::npos &&
        (authority_end == std::string::npos || at < authority_end)) {
      endpoint.erase(authority_start, at - authority_start + 1);
    }
  }
  return endpoint;
}

}  // namespace

JsonlDiagnosticSink::JsonlDiagnosticSink(std::filesystem::path path)
    : path_(std::move(path)) {}

void JsonlDiagnosticSink::Record(const DiagnosticEvent& event) {
  if (path_.empty()) {
    return;
  }
  std::error_code error;
  if (path_.has_parent_path()) {
    std::filesystem::create_directories(path_.parent_path(), error);
  }
  if (error) {
    return;
  }
  std::ofstream output(path_, std::ios::app);
  if (!output) {
    return;
  }
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto timestamp_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  const nlohmann::json line{
      {"timestamp_ms", timestamp_ms},
      {"request_id", event.request_id},
      {"provider", event.provider},
      {"api_format", event.api_format},
      {"model", event.model},
      {"endpoint", SafeEndpoint(event.endpoint)},
      {"provider_request_id", event.provider_request_id},
      {"outcome", event.outcome},
      {"http_status", event.http_status},
      {"duration_ms", event.duration.count()},
      {"estimated_input_tokens", event.estimated_input_tokens},
      {"max_output_tokens", event.max_output_tokens},
      {"error_code", event.error_code},
      {"error", event.error}};
  output << line.dump() << '\n';
}

void RecordDiagnostic(IDiagnosticSink* sink,
                      const AppConfig& config,
                      const std::string& endpoint,
                      const RewriteResult& result) {
  if (sink == nullptr) {
    return;
  }
  DiagnosticEvent event;
  event.request_id = result.request_id;
  event.provider = config.provider;
  event.api_format = DiagnosticApiFormat(config);
  event.model = config.model;
  event.endpoint = endpoint;
  event.provider_request_id = result.provider_request_id;
  event.outcome = result.ok ? "success" : "failure";
  event.error = result.error;
  event.error_code = result.error_code;
  event.http_status = result.http_status;
  event.duration = result.duration;
  event.estimated_input_tokens = result.estimated_input_tokens;
  event.max_output_tokens = result.max_output_tokens;
  sink->Record(event);
}

}  // namespace llm_rewriter
