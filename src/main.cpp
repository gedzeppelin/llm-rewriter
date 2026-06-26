#include "llm_rewriter/App.hpp"

#include "llm_rewriter/AppIdentity.hpp"
#include "llm_rewriter/Cli.hpp"
#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/Clipboard.hpp"
#include "llm_rewriter/Output.hpp"
#include "llm_rewriter/Paths.hpp"
#include "llm_rewriter/RewriteService.hpp"

#include <iostream>
#include <sstream>
#include <string>

#if defined(__WXGTK__)
#include <glib.h>
#endif

wxIMPLEMENT_APP_NO_MAIN(llm_rewriter::App);

namespace {

std::string ReadStdin() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

std::string ReadInput(llm_rewriter::InputMode mode) {
  switch (mode) {
    case llm_rewriter::InputMode::Stdin:
      return ReadStdin();
    case llm_rewriter::InputMode::Primary:
      return llm_rewriter::ReadPrimarySelectionText();
    case llm_rewriter::InputMode::Clipboard:
      return llm_rewriter::ReadClipboardText();
  }
  return {};
}

int RunHeadless(const llm_rewriter::CliOptions& options,
                const llm_rewriter::UserPaths& paths,
                const llm_rewriter::AppConfig& config) {
  const auto result = llm_rewriter::RewriteAndRecord(
      config, paths, {.input = ReadInput(options.input)});
  if (!result.ok) {
    std::cerr << "rewrite failed: " << result.error << '\n';
    return 1;
  }

  if (options.output == llm_rewriter::OutputMode::Stdout) {
    std::cout << result.text;
    return 0;
  }

  std::string message;
  if (!llm_rewriter::WriteOutput(options.output, options, result.text,
                                     message)) {
    std::cerr << message << '\n';
    return 1;
  }
  if (!message.empty()) {
    std::cerr << message << '\n';
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const auto parsed = llm_rewriter::ParseCli(argc, argv);
  if (parsed.exit) {
    return parsed.exit_code;
  }
  if (parsed.options) {
    auto options = *parsed.options;
    auto paths = llm_rewriter::ResolveUserPaths();
    if (!options.config_path.empty()) {
      paths.config_file = llm_rewriter::ExpandUserPath(options.config_path);
    }
    llm_rewriter::EnsureDefaultConfigFile(paths.config_file);
    auto config = llm_rewriter::LoadConfig(paths.config_file);
    llm_rewriter::ApplyCliOverrides(config, options.model,
                                    options.reasoning_effort);
    llm_rewriter::ApplyConfigDefaults(options, config);
    if (llm_rewriter::IsHeadlessCli(options.output)) {
      return RunHeadless(options, paths, config);
    }
  }
#if defined(__WXGTK__)
  g_set_prgname(llm_rewriter::kApplicationId);
#endif
  return wxEntry(argc, argv);
}
