#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace llm_rewriter::internal {

inline bool IsSymlink(const std::filesystem::path& path) {
  std::error_code error;
  const bool symlink = std::filesystem::is_symlink(path, error);
  return !error && symlink;
}

inline bool IsRegularFileOrMissing(const std::filesystem::path& path) {
  if (IsSymlink(path)) return false;
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return !error;
  return !error && std::filesystem::is_regular_file(path, error) && !error;
}

inline bool PrepareSecureParent(const std::filesystem::path& path) {
  if (!path.has_parent_path()) return true;
  const auto parent = path.parent_path();
  if (parent == "." || parent == parent.root_path()) return true;

  std::error_code error;
  if (IsSymlink(parent)) return false;
  const bool existed = std::filesystem::exists(parent, error);
  if (error) return false;
  if (existed && !std::filesystem::is_directory(parent, error)) {
    return false;
  }
  if (error) return false;
  std::filesystem::create_directories(parent, error);
  if (error) return false;
#if defined(_WIN32)
  return true;
#else
  // Only harden a directory created for this write (or the application's
  // conventional leaf) so an arbitrary path such as /tmp/config.json does
  // not chmod a shared system directory.
  if (!existed || parent.filename() == "llm-rewriter") {
    return ::chmod(parent.c_str(), 0700) == 0;
  }
  return true;
#endif
}

inline bool SetPrivateFilePermissions(const std::filesystem::path& path) {
#if defined(_WIN32)
  (void)path;
  return true;
#else
  return ::chmod(path.c_str(), 0600) == 0;
#endif
}

inline std::filesystem::path SecureTempPath(
    const std::filesystem::path& destination, unsigned attempt) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
  const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  const auto process = static_cast<std::uint64_t>(::getpid());
#endif
  return destination.parent_path() /
         (destination.filename().string() + ".tmp." +
          std::to_string(process) + "." + std::to_string(now) + "." +
          std::to_string(attempt));
}

inline bool AtomicReplace(const std::filesystem::path& temporary,
                          const std::filesystem::path& destination) {
#if defined(_WIN32)
  return MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  std::filesystem::rename(temporary, destination, error);
  return !error;
#endif
}

inline bool SecureFilePermissions(const std::filesystem::path& path) {
  if (IsSymlink(path)) return false;
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return true;
  if (error || !std::filesystem::is_regular_file(path, error) || error) {
    return false;
  }
#if defined(_WIN32)
  return true;
#else
  struct stat metadata {};
  if (::stat(path.c_str(), &metadata) != 0) return false;
  return (metadata.st_mode & 0777) == 0600;
#endif
}

}  // namespace llm_rewriter::internal
