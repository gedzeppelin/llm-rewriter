#include "llm_rewriter/Paths.hpp"

#include <cstdlib>
#include <string>

namespace llm_rewriter {
namespace {

std::filesystem::path HomeDir() {
  if (const char* home = std::getenv("HOME")) {
    return home;
  }
  return {};
}

std::filesystem::path EnvPath(const char* name,
                              const std::filesystem::path& fallback) {
  if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
    return value;
  }
  return fallback;
}

}  // namespace

UserPaths ResolveUserPaths() {
  const auto home = HomeDir();

#if defined(__APPLE__)
  const auto base = home / "Library" / "Application Support" / "llm-rewriter";
  return {.config_dir = base,
          .data_dir = base,
          .config_file = base / "config.ini",
          .history_file = base / "history.jsonl"};
#elif defined(_WIN32)
  const char* appdata = std::getenv("APPDATA");
  const auto base = std::filesystem::path(appdata != nullptr ? appdata : ".") /
                    "llm-rewriter";
  return {.config_dir = base,
          .data_dir = base,
          .config_file = base / "config.ini",
          .history_file = base / "history.jsonl"};
#else
  const auto config_dir =
      EnvPath("XDG_CONFIG_HOME", home / ".config") / "llm-rewriter";
  const auto data_dir =
      EnvPath("XDG_DATA_HOME", home / ".local" / "share") / "llm-rewriter";
  return {.config_dir = config_dir,
          .data_dir = data_dir,
          .config_file = config_dir / "config.ini",
          .history_file = data_dir / "history.jsonl"};
#endif
}

std::filesystem::path ExpandUserPath(const std::string& path) {
  if (path == "~") {
    return HomeDir();
  }
  if (path.rfind("~/", 0) == 0) {
    return HomeDir() / path.substr(2);
  }
  return path;
}

}  // namespace llm_rewriter
