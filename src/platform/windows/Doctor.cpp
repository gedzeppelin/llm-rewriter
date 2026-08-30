#include "llm_rewriter/Doctor.hpp"

#include "llm_rewriter/Credentials.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace llm_rewriter {
namespace {

bool ParentWritable(const std::filesystem::path& path) {
  std::error_code error;
  const auto parent = path.parent_path();
  std::filesystem::create_directories(parent, error);
  if (error) {
    return false;
  }
  const auto probe = parent / ".llm-rewriter-doctor.tmp";
  std::ofstream output(probe);
  const bool ok = static_cast<bool>(output);
  output.close();
  std::filesystem::remove(probe, error);
  return ok;
}

}  // namespace

RuntimeStatus ProbeRuntime(const AppConfig& config,
                           const UserPaths& paths,
                           bool) {
  RuntimeStatus status;
  status.clipboard_read_available = true;
  status.clipboard_write_available = true;
  status.notifications_available = false;
  status.details = {{"native clipboard", true}, {"WinHTTP", true}};
  status.config_parent_writable = ParentWritable(paths.config_file);
  status.config_readable = std::filesystem::exists(paths.config_file)
                               ? std::filesystem::is_regular_file(paths.config_file)
                               : status.config_parent_writable;
  status.history_parent_writable = ParentWritable(paths.history_file);
  CredentialDependencies dependencies;
  dependencies.store = CreatePlatformCredentialStore();
  CredentialResolver credentials(config.codex_auth_file, dependencies);
  const auto credential_status =
      credentials.Status(config.provider, config.credential);
  status.credential_available =
      credential_status.ok() &&
      credential_status.source != CredentialSource::Unconfigured;
  return status;
}

DoctorReport BuildDoctorReport(const AppConfig& config,
                               const CliOptions& options,
                               const UserPaths& paths,
                               const RuntimeStatus& status) {
  const bool paths_ok = status.config_readable &&
                        status.history_parent_writable;
  const bool ok = !config.model.empty() && paths_ok;
  std::ostringstream out;
  out << "llm-rewriter doctor\nplatform: windows\n"
      << "config: " << paths.config_file << '\n'
      << "history: " << paths.history_file << '\n'
      << "diagnostics: " << paths.diagnostics_file << '\n'
      << "model configured: " << (!config.model.empty() ? "yes" : "no") << '\n'
      << "provider credential available: "
      << (status.credential_available ? "yes" : "no") << '\n'
      << "input: " << ToString(options.input) << '\n'
      << "output: " << ToString(options.output) << '\n'
      << "result: " << (ok ? "ready" : "not ready") << '\n';
  return {.ok = ok, .text = out.str()};
}

DoctorReport RunDoctor(const AppConfig& config,
                       const CliOptions& options,
                       const UserPaths& paths,
                       bool live) {
  return BuildDoctorReport(config, options, paths,
                           ProbeRuntime(config, paths, live));
}

}  // namespace llm_rewriter
