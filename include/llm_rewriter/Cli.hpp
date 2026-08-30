#pragma once

#include "llm_rewriter/Config.hpp"

#include <optional>
#include <string>
#include <vector>

namespace llm_rewriter {

enum class InputMode {
  Clipboard,
  Primary,
  Stdin,
};

enum class OutputMode {
  Preview,
  Clipboard,
  Stdout,
#if defined(__linux__)
  Type,
  Paste,
#endif
};

enum class CliCommand {
  Rewrite,
  Doctor,
  Providers,
};

enum class ProviderCredentialCommand {
  Status,
  Configure,
  Clear,
};

struct CliOptions {
  CliCommand command = CliCommand::Rewrite;
  InputMode input = InputMode::Clipboard;
  OutputMode output = OutputMode::Preview;
  bool input_explicit = false;
  bool output_explicit = false;
  std::optional<std::string> model;
  std::optional<std::string> reasoning;
  std::string config_path;
#if defined(__linux__)
  bool ctrl_c_before_output = false;
  PasteShortcut paste_shortcut = PasteShortcut::CtrlV;
#endif
  bool doctor_live = false;
  ProviderCredentialCommand provider_command =
      ProviderCredentialCommand::Status;
  std::string credential_provider;
  bool device_code = false;
};

struct CliParseResult {
  bool exit = false;
  int exit_code = 0;
  std::optional<CliOptions> options;
};

CliParseResult ParseCli(int argc, char** argv);
CliParseResult ParseCli(const std::vector<std::string>& args);

std::string ToString(InputMode mode);
std::string ToString(OutputMode mode);
std::string ValidOutputModesText();
bool IsHeadlessCli(OutputMode mode);
void ApplyConfigDefaults(CliOptions& options, const AppConfig& config);

}  // namespace llm_rewriter
