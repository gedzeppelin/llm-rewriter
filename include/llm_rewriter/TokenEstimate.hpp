#pragma once

#include "llm_rewriter/Config.hpp"

#include <string_view>

namespace llm_rewriter {

int EstimateTokens(std::string_view text);
int ChooseMaxOutputTokens(const AppConfig& config, int input_tokens);

}  // namespace llm_rewriter
