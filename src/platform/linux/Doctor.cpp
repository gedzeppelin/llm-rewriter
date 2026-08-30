#include "llm_rewriter/Doctor.hpp"

#include "llm_rewriter/Credentials.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace llm_rewriter {
namespace {

bool EnvSet(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0';
}

bool CommandExists(const char* command) {
  std::string check = "command -v ";
  check += command;
  check += " >/dev/null 2>&1";
  return std::system(check.c_str()) == 0;
}

bool RunCommand(const char* command) {
  return std::system(command) == 0;
}

bool ParentWritable(const std::filesystem::path& path) {
  std::error_code error;
  const auto parent = path.parent_path();
  if (parent.empty()) {
    return false;
  }
  if (!std::filesystem::exists(parent, error)) {
    std::filesystem::create_directories(parent, error);
  }
  if (error) {
    return false;
  }
  const auto probe = parent / ".llm-rewriter-doctor.tmp";
  {
    std::ofstream output(probe);
    if (!output) {
      return false;
    }
  }
  std::filesystem::remove(probe, error);
  return true;
}

std::string YesNo(bool value) {
  return value ? "yes" : "no";
}

void Line(std::ostringstream& out,
          const std::string& label,
          bool value,
          const std::string& suffix = {}) {
  out << "  " << label << ": " << YesNo(value);
  if (!suffix.empty()) {
    out << " (" << suffix << ")";
  }
  out << '\n';
}

bool DetailAvailable(const RuntimeStatus& status, const std::string& name) {
  for (const auto& detail : status.details) {
    if (detail.name == name) {
      return detail.available;
    }
  }
  return false;
}

}  // namespace

RuntimeStatus ProbeRuntime(const AppConfig& config,
                           const UserPaths& paths,
                           bool live) {
  RuntimeStatus status;
  const bool wayland = EnvSet("WAYLAND_DISPLAY");
  const bool x11 = EnvSet("DISPLAY");
  const bool wl_copy = CommandExists("wl-copy");
  const bool wl_paste = CommandExists("wl-paste");
  const bool xclip = CommandExists("xclip");
  const bool xsel = CommandExists("xsel");
  const bool wtype = CommandExists("wtype");
  const bool xdotool = CommandExists("xdotool");
  const bool ydotool = CommandExists("ydotool");
  bool ydotoold = CommandExists("pgrep") &&
                  RunCommand("pgrep -x ydotoold >/dev/null 2>&1");
  if (live && ydotool) {
    ydotoold = RunCommand("ydotool key 0:0 >/dev/null 2>&1");
  }
  status.clipboard_read_available = (wayland && wl_paste) || xclip || xsel;
  status.clipboard_write_available = (wayland && wl_copy) || xclip || xsel;
  status.primary_selection_available = xclip || xsel;
  status.synthetic_output_available = (wayland && wtype) || (x11 && xdotool) ||
                                      (ydotool && ydotoold);
  status.notifications_available = CommandExists("notify-send");
  status.details = {{"wayland", wayland},
                    {"x11", x11},
                    {"wl-copy", wl_copy},
                    {"wl-paste", wl_paste},
                    {"xclip", xclip},
                    {"xsel", xsel},
                    {"wtype", wtype},
                    {"xdotool", xdotool},
                    {"ydotool", ydotool},
                    {"ydotoold reachable", ydotoold},
                    {"notify-send", status.notifications_available}};
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
  std::ostringstream out;
  out << "llm-rewriter doctor\n";
  out << "runtime:\n";
  for (const auto& detail : status.details) {
    Line(out, detail.name, detail.available);
  }

  out << "configuration:\n";
  out << "  config: " << paths.config_file << '\n';
  out << "  history: " << paths.history_file << '\n';
  out << "  diagnostics: " << paths.diagnostics_file << '\n';
  Line(out, "config readable or creatable", status.config_readable);
  Line(out, "config directory writable", status.config_parent_writable);
  Line(out, "history directory writable", status.history_parent_writable);
  Line(out, "model configured", !config.model.empty());
  Line(out, "provider credential available", status.credential_available);
  out << "  input: " << ToString(options.input) << '\n';
  out << "  output: " << ToString(options.output) << '\n';

  bool ok = true;
  if (config.model.empty()) {
    ok = false;
  }
  if (!status.config_readable || !status.history_parent_writable) {
    ok = false;
  }
  if ((options.input == InputMode::Clipboard &&
       !status.clipboard_read_available) ||
      (options.input == InputMode::Primary &&
       !status.primary_selection_available)) {
    ok = false;
    out << "issue: clipboard input is configured but no clipboard reader is "
           "available.\n";
  }
  if (options.output == OutputMode::Clipboard &&
      !status.clipboard_write_available) {
    ok = false;
    out << "issue: clipboard output is configured but no clipboard writer is "
           "available.\n";
  }
  if ((options.output == OutputMode::Type || options.output == OutputMode::Paste) &&
      !status.synthetic_output_available) {
    ok = false;
    out << "issue: synthetic output is configured but wtype, xdotool, or "
           "ydotool+ydotoold is not ready.\n";
  }
  if (options.output == OutputMode::Paste &&
      !status.clipboard_write_available) {
    ok = false;
    out << "issue: paste output needs clipboard write support first.\n";
  }
  if (!status.notifications_available) {
    out << "warning: notifications are unavailable without notify-send.\n";
  }
  if (!status.credential_available) {
    out << "warning: no provider credential is available from configured "
           "sources.\n";
  }
  if (!DetailAvailable(status, "ydotoold reachable") &&
      DetailAvailable(status, "ydotool")) {
    out << "note: ydotool is installed but ydotoold does not appear reachable.\n";
  }

  out << "result: " << (ok ? "ready" : "not ready") << '\n';
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
