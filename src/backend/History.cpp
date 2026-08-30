#include "llm_rewriter/History.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace llm_rewriter {

void AppendHistory(const std::filesystem::path& history_path,
                   const AppConfig& config,
                   const std::string& input,
                   const RewriteResult& result) {
  if (!config.history_enabled) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(history_path.parent_path(), error);
  if (error) {
    return;
  }
  std::ofstream output(history_path, std::ios::app);
  if (!output) {
    return;
  }

  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

  nlohmann::json line{{"timestamp_ms", millis},
                      {"provider", config.provider},
                      {"api_format", ToString(config.api_format)},
                      {"model", config.model},
                      {"reasoning", config.reasoning},
                      {"ok", result.ok},
                      {"request_id", result.request_id},
                      {"provider_request_id", result.provider_request_id},
                      {"http_status", result.http_status},
                      {"duration_ms", result.duration.count()},
                      {"input", input},
                      {"output", result.text},
                      {"error", result.error},
                      {"estimated_input_tokens",
                       result.estimated_input_tokens},
                      {"max_output_tokens", result.max_output_tokens}};
  output << line.dump() << '\n';
}

}  // namespace llm_rewriter
