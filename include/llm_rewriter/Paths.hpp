#pragma once

#include <filesystem>

namespace llm_rewriter {

struct UserPaths {
  std::filesystem::path config_dir;
  std::filesystem::path data_dir;
  std::filesystem::path config_file;
  std::filesystem::path history_file;
};

UserPaths ResolveUserPaths();
std::filesystem::path ExpandUserPath(const std::string& path);

}  // namespace llm_rewriter
