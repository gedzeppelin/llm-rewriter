#pragma once

#include "llm_rewriter/Config.hpp"

#include <string>

namespace llm_rewriter {

struct RewriteRequest {
  std::string input;
};

struct RewriteResult {
  bool ok = false;
  std::string text;
  std::string error;
  int estimated_input_tokens = 0;
  int max_output_tokens = 0;
};

RewriteResult RewriteWithLlm(const AppConfig& config,
                             const RewriteRequest& request);

std::string BuildLlmPayloadForTest(const AppConfig& config,
                                   const RewriteRequest& request,
                                   int max_output_tokens);

}  // namespace llm_rewriter
