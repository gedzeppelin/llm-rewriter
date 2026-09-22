#pragma once

#include "llm_rewriter/Cli.hpp"
#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/Paths.hpp"

#include <string>
#include <vector>

namespace llm_rewriter {

struct RuntimeStatus {
  struct Detail {
    std::string name;
    bool available = false;
  };

  bool clipboard_read_available = false;
  bool clipboard_write_available = false;
  bool primary_selection_available = false;
  bool synthetic_output_available = false;
  bool notifications_available = false;
  bool config_parent_writable = false;
  bool config_readable = false;
  bool config_secure_permissions = true;
  bool history_parent_writable = false;
  bool history_secure_permissions = true;
  bool diagnostics_secure_permissions = true;
  bool credential_available = false;
  std::vector<Detail> details;
};

struct DoctorReport {
  bool ok = false;
  std::string text;
};

RuntimeStatus ProbeRuntime(const AppConfig& config,
                           const UserPaths& paths,
                           bool live);
DoctorReport BuildDoctorReport(const AppConfig& config,
                               const CliOptions& options,
                               const UserPaths& paths,
                               const RuntimeStatus& status);
DoctorReport RunDoctor(const AppConfig& config,
                       const CliOptions& options,
                       const UserPaths& paths,
                       bool live);

}  // namespace llm_rewriter
