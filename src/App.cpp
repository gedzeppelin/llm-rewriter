#include "llm_rewriter/App.hpp"

#include "llm_rewriter/AppIdentity.hpp"
#include "llm_rewriter/Clipboard.hpp"
#include "llm_rewriter/Cli.hpp"
#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/MainFrame.hpp"
#include "llm_rewriter/Output.hpp"
#include "llm_rewriter/Paths.hpp"
#include "llm_rewriter/RewriteService.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace llm_rewriter {
namespace {

std::string ReadStdin() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

std::string ReadInput(InputMode mode) {
  switch (mode) {
    case InputMode::Stdin:
      return ReadStdin();
    case InputMode::Primary:
      return ReadPrimarySelectionText();
    case InputMode::Clipboard:
      return ReadClipboardText();
  }
  return {};
}

std::vector<std::string> ArgsFromWx(int argc, wxChar** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    args.push_back(wxString(argv[i]).ToStdString());
  }
  return args;
}

}  // namespace

bool App::OnInit() {
  SetAppName(kApplicationId);
  SetClassName(kApplicationId);

  const auto parsed = ParseCli(ArgsFromWx(argc, argv));
  if (parsed.exit || !parsed.options) {
    return false;
  }
  const auto& options = *parsed.options;
  auto paths = ResolveUserPaths();
  if (!options.config_path.empty()) {
    paths.config_file = ExpandUserPath(options.config_path);
  }
  EnsureDefaultConfigFile(paths.config_file);
  auto config = LoadConfig(paths.config_file);
  ApplyCliOverrides(config, options.model, options.reasoning_effort);
  auto effective_options = options;
  ApplyConfigDefaults(effective_options, config);
  auto* frame = new MainFrame(std::move(config), std::move(paths),
                              ReadInput(effective_options.input));
  frame->Show(true);
  return true;
}

}  // namespace llm_rewriter
