#pragma once

#include "llm_rewriter/Cli.hpp"

#include <string>

namespace llm_rewriter {

bool WriteOutput(OutputMode mode,
                 const CliOptions& options,
                 const std::string& text,
                 std::string& message);

}  // namespace llm_rewriter
