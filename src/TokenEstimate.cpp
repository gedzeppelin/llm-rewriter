#include "llm_rewriter/TokenEstimate.hpp"

#include <algorithm>

namespace llm_rewriter {

int EstimateTokens(std::string_view text) {
  return std::max(1, static_cast<int>((text.size() + 3) / 4));
}

int ChooseMaxOutputTokens(const AppConfig& config, int input_tokens) {
  const auto estimated =
      static_cast<int>(input_tokens * config.output_token_multiplier) +
      config.output_token_padding;
  return std::clamp(estimated, config.min_output_tokens,
                    config.max_output_tokens_limit);
}

}  // namespace llm_rewriter
