#include "llm_rewriter/Cli.hpp"

#include "llm_rewriter/Config.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace llm_rewriter {
namespace {

std::string InputChoices() {
  return "clipboard,primary,stdin";
}

std::string OutputChoices() {
#if defined(__linux__)
  return "preview,clipboard,stdout,type,paste";
#else
  return "preview,clipboard,stdout";
#endif
}

std::string OutputChoicesForShell() {
#if defined(__linux__)
  return "preview clipboard stdout type paste";
#else
  return "preview clipboard stdout";
#endif
}

std::string BashCompletion() {
  return R"(# bash completion for llm-rewriter
_llm_rewriter_complete() {
  local cur prev
  COMPREPLY=()
  cur="${COMP_WORDS[COMP_CWORD]}"
  prev="${COMP_WORDS[COMP_CWORD-1]}"

  case "$prev" in
    --input)
      COMPREPLY=($(compgen -W "clipboard primary stdin" -- "$cur"))
      return 0
      ;;
    --output)
      COMPREPLY=($(compgen -W ")" +
         OutputChoicesForShell() +
         R"(" -- "$cur"))
      return 0
      ;;
    --generate-completion)
      COMPREPLY=($(compgen -W "bash zsh fish" -- "$cur"))
      return 0
      ;;
    --config|--model|--reasoning-effort)
      return 0
      ;;
  esac

  COMPREPLY=($(compgen -W "--input --output --model --reasoning-effort --config --help --generate-completion )" +
#if defined(__linux__)
         std::string("--ctrl-c-before-output ") +
#else
         std::string{} +
#endif
         R"(" -- "$cur"))
}
complete -F _llm_rewriter_complete llm-rewriter
)";
}

std::string ZshCompletion() {
  std::ostringstream out;
  out << "#compdef llm-rewriter\n"
      << "_llm_rewriter() {\n"
      << "  _arguments \\\n"
      << "    '--input[Input source]:input:(clipboard primary stdin)' \\\n"
      << "    '--output[Output mode]:output:(" << OutputChoicesForShell()
      << ")' \\\n"
      << "    '--model[LLM model id]:model:' \\\n"
      << "    '--reasoning-effort[Reasoning effort]:reasoning:(off low medium high)' \\\n"
      << "    '--config[Config file path]:config:_files' \\\n"
      << "    '--generate-completion[Generate shell completion]:shell:(bash zsh "
         "fish)' \\\n";
#if defined(__linux__)
  out << "    '--ctrl-c-before-output[Send Ctrl+C before Linux type/paste "
         "output]' \\\n";
#endif
  out << "    '--help[Show help]'\n"
      << "}\n"
      << "_llm_rewriter \"$@\"\n";
  return out.str();
}

std::string FishCompletion() {
  std::ostringstream out;
  out << "complete -c llm-rewriter -l input -d 'Input source' -xa "
         "'clipboard primary stdin'\n";
  out << "complete -c llm-rewriter -l output -d 'Output mode' -xa '"
      << OutputChoicesForShell() << "'\n";
  out << "complete -c llm-rewriter -l model -d 'LLM model id' -r\n";
  out << "complete -c llm-rewriter -l reasoning-effort -d 'Reasoning effort' "
         "-xa 'off low medium high'\n";
  out << "complete -c llm-rewriter -l config -d 'Config file path' -r\n";
  out << "complete -c llm-rewriter -l generate-completion -d 'Generate "
         "shell completion' -xa 'bash zsh fish'\n";
#if defined(__linux__)
  out << "complete -c llm-rewriter -l ctrl-c-before-output -d 'Send Ctrl+C "
         "before Linux type/paste output'\n";
#endif
  out << "complete -c llm-rewriter -s h -l help -d 'Show help'\n";
  return out.str();
}

std::string CompletionScript(const std::string& shell) {
  if (shell == "bash") {
    return BashCompletion();
  }
  if (shell == "zsh") {
    return ZshCompletion();
  }
  if (shell == "fish") {
    return FishCompletion();
  }
  return {};
}

InputMode ParseInputMode(const std::string& value) {
  if (value == "clipboard") {
    return InputMode::Clipboard;
  }
  if (value == "primary") {
    return InputMode::Primary;
  }
  if (value == "stdin") {
    return InputMode::Stdin;
  }
  throw CLI::ValidationError("--input", "expected one of: " + InputChoices());
}

OutputMode ParseOutputMode(const std::string& value) {
  if (value == "preview") {
    return OutputMode::Preview;
  }
  if (value == "clipboard") {
    return OutputMode::Clipboard;
  }
  if (value == "stdout") {
    return OutputMode::Stdout;
  }
#if defined(__linux__)
  if (value == "type") {
    return OutputMode::Type;
  }
  if (value == "paste") {
    return OutputMode::Paste;
  }
#endif
  throw CLI::ValidationError("--output", "expected one of: " + OutputChoices());
}

void ConfigureCliApp(CLI::App& app,
                     CliOptions& options,
                     std::string& input,
                     std::string& output,
                     std::string& completion_shell,
                     CLI::Option*& input_option,
                     CLI::Option*& output_option) {
  app.allow_extras(false);

  input = ToString(options.input);
  output = ToString(options.output);

  input_option = app.add_option("--input", input, "Input source")
                     ->check(CLI::IsMember({"clipboard", "primary", "stdin"}));
  output_option = app.add_option("--output", output, "Output mode")
                      ->check(CLI::IsMember({
          "preview",
          "clipboard",
          "stdout",
#if defined(__linux__)
          "type",
          "paste",
#endif
      }));
  app.add_option("--config", options.config_path, "Config file path");
  app.add_option("--model", options.model, "LLM model id");
  app.add_option("--reasoning-effort", options.reasoning_effort,
                 "Reasoning effort to request from the provider")
      ->check(CLI::IsMember({"off", "low", "medium", "high"}));
  app.add_option("--generate-completion", completion_shell,
                 "Generate shell completion for bash, zsh, or fish")
      ->check(CLI::IsMember({"bash", "zsh", "fish"}));
#if defined(__linux__)
  app.add_flag("--ctrl-c-before-output", options.ctrl_c_before_output,
               "Send Ctrl+C before Linux type/paste output. This can interrupt "
               "the focused application.");
#endif
}

CliParseResult ParseVector(std::vector<std::string> args) {
  CliOptions options;
  std::string input;
  std::string output;
  std::string completion_shell;
  CLI::Option* input_option = nullptr;
  CLI::Option* output_option = nullptr;
  CLI::App app{"Native LLM rewrite utility"};
  ConfigureCliApp(app, options, input, output, completion_shell, input_option,
                  output_option);

  try {
    app.parse(args);
    if (!completion_shell.empty()) {
      std::cout << CompletionScript(completion_shell);
      return {.exit = true, .exit_code = 0, .options = std::nullopt};
    }
    options.input = ParseInputMode(input);
    options.output = ParseOutputMode(output);
    options.input_explicit = input_option != nullptr && input_option->count() > 0;
    options.output_explicit =
        output_option != nullptr && output_option->count() > 0;
#if defined(__linux__)
    if (options.ctrl_c_before_output && options.output != OutputMode::Type &&
        options.output != OutputMode::Paste) {
      throw CLI::ValidationError(
          "--ctrl-c-before-output",
          "only applies to --output type or --output paste");
    }
#endif
  } catch (const CLI::ParseError& error) {
    return {.exit = true,
            .exit_code = app.exit(error),
            .options = std::nullopt};
  }

  return {.exit = false, .exit_code = 0, .options = options};
}

}  // namespace

CliParseResult ParseCli(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = argc - 1; i >= 1; --i) {
    args.emplace_back(argv[i]);
  }
  return ParseVector(std::move(args));
}

CliParseResult ParseCli(const std::vector<std::string>& args) {
  auto reversed = args;
  std::reverse(reversed.begin(), reversed.end());
  return ParseVector(std::move(reversed));
}

std::string ToString(InputMode mode) {
  switch (mode) {
    case InputMode::Clipboard:
      return "clipboard";
    case InputMode::Primary:
      return "primary";
    case InputMode::Stdin:
      return "stdin";
  }
  return "clipboard";
}

std::string ToString(OutputMode mode) {
  switch (mode) {
    case OutputMode::Preview:
      return "preview";
    case OutputMode::Clipboard:
      return "clipboard";
    case OutputMode::Stdout:
      return "stdout";
#if defined(__linux__)
    case OutputMode::Type:
      return "type";
    case OutputMode::Paste:
      return "paste";
#endif
  }
  return "preview";
}

std::string ValidOutputModesText() {
  return OutputChoices();
}

bool IsHeadlessCli(OutputMode mode) {
  switch (mode) {
    case OutputMode::Preview:
      return false;
    case OutputMode::Clipboard:
    case OutputMode::Stdout:
      return true;
#if defined(__linux__)
    case OutputMode::Type:
    case OutputMode::Paste:
      return true;
#endif
  }
  return false;
}

void ApplyConfigDefaults(CliOptions& options, const AppConfig& config) {
#if defined(__linux__)
  options.paste_shortcut = config.paste_shortcut;
#endif
  if (!options.input_explicit) {
    try {
      options.input = ParseInputMode(config.input_mode);
    } catch (const CLI::ValidationError&) {
    }
  }
  if (!options.output_explicit) {
    try {
      options.output = ParseOutputMode(config.output_mode);
    } catch (const CLI::ValidationError&) {
    }
  }
}

}  // namespace llm_rewriter
