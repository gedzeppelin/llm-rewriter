#pragma once

#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/LlmClient.hpp"

#include <filesystem>
#include <string>

namespace llm_rewriter {

void AppendHistory(const std::filesystem::path& history_path,
                   const AppConfig& config,
                   const std::string& input,
                   const RewriteResult& result);

}  // namespace llm_rewriter
