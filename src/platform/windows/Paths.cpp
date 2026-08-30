#include "llm_rewriter/Paths.hpp"

#include <cstdlib>

namespace llm_rewriter {

UserPaths ResolveUserPaths() {
  const char* appdata = std::getenv("APPDATA");
  const auto base = std::filesystem::path(appdata != nullptr ? appdata : ".") /
                    "llm-rewriter";
  return {.config_dir = base,
          .data_dir = base,
          .config_file = base / "config.json",
          .history_file = base / "history.jsonl",
          .diagnostics_file = base / "diagnostics.jsonl"};
}

std::filesystem::path ExpandUserPath(const std::string& path) {
  return path;
}

}  // namespace llm_rewriter
