#include "llm_rewriter/TokenEstimate.hpp"

#include <algorithm>
#include <unordered_map>

namespace llm_rewriter {
namespace {

const std::unordered_map<std::string_view, int>& ReasoningMultipliers() {
  static const std::unordered_map<std::string_view, int> multipliers{
      {"none", 1},
      {"off", 1},
      {"minimal", 1},
      {"low", 2},
      {"medium", 4},
      {"auto", 4},
      {"enabled", 4},
      {"high", 8},
      {"xhigh", 12},
      {"max", 16},
      {"*", 4},
  };
  return multipliers;
}

int ReasoningMultiplier(std::string_view reasoning) {
  const auto& multipliers = ReasoningMultipliers();
  const auto exact = multipliers.find(reasoning);
  if (exact != multipliers.end()) {
    return exact->second;
  }
  return multipliers.at("*");
}

}  // namespace

int EstimateTokens(std::string_view text) {
  return std::max(1, static_cast<int>((text.size() + 3) / 4));
}

int ChooseMaxOutputTokens(const AppConfig& config, int input_tokens) {
  const auto estimated =
      static_cast<int>(input_tokens * config.output_token_multiplier) +
      config.output_token_padding;
  const auto reasoning_adjusted =
      static_cast<long long>(estimated) * ReasoningMultiplier(config.reasoning);
  const auto clamped =
      std::clamp(reasoning_adjusted,
                 static_cast<long long>(config.min_output_tokens),
                 static_cast<long long>(config.max_output_tokens_limit));

  return static_cast<int>(clamped);
}

}  // namespace llm_rewriter
