#include "llm_rewriter/Config.hpp"

#include "llm_rewriter/Paths.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace llm_rewriter {
namespace {

std::string Trim(std::string_view value) {
  auto first = value.begin();
  auto last = value.end();
  while (first != last && std::isspace(static_cast<unsigned char>(*first))) {
    ++first;
  }
  while (first != last && std::isspace(static_cast<unsigned char>(*(last - 1)))) {
    --last;
  }
  return std::string(first, last);
}

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool BoolValue(const std::string& value, bool fallback) {
  const auto lower = Lower(Trim(value));
  if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
    return true;
  }
  if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
    return false;
  }
  return fallback;
}

int IntValue(const std::string& value, int fallback) {
  try {
    return std::stoi(Trim(value));
  } catch (...) {
    return fallback;
  }
}

double DoubleValue(const std::string& value, double fallback) {
  try {
    return std::stod(Trim(value));
  } catch (...) {
    return fallback;
  }
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

struct ParsedIni {
  std::unordered_map<std::string, std::string> values;
  std::string prompt;
  std::string api_key;
  std::string api_key_env;
};

ParsedIni ParseIni(const std::filesystem::path& path) {
  ParsedIni parsed;
  std::ifstream input(path);
  if (!input) {
    return parsed;
  }

  std::string line;
  while (std::getline(input, line)) {
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    const auto equals = trimmed.find('=');
    if (equals == std::string::npos) {
      continue;
    }

    const auto key = Trim(std::string_view(trimmed).substr(0, equals));
    auto value = Trim(std::string_view(trimmed).substr(equals + 1));
    if (key.empty()) {
      continue;
    }

    if (value.rfind("<<", 0) == 0) {
      const auto marker = Trim(std::string_view(value).substr(2));
      std::ostringstream block;
      bool first = true;
      while (std::getline(input, line)) {
        if (Trim(line) == marker) {
          break;
        }
        if (!first) {
          block << '\n';
        }
        block << line;
        first = false;
      }
      value = block.str();
    }

    if (!parsed.values.contains(key)) {
      parsed.values.emplace(key, value);
    }

    if (parsed.prompt.empty() && key == "system_prompt" && !Trim(value).empty()) {
      parsed.prompt = value;
    }
    if (parsed.prompt.empty() && key == "system_prompt_file" &&
        !Trim(value).empty()) {
      parsed.prompt = ReadTextFile(ExpandUserPath(Trim(value)));
    }
    if (parsed.api_key.empty() && parsed.api_key_env.empty() &&
        key == "api_key" && !Trim(value).empty()) {
      parsed.api_key = value;
    }
    if (parsed.api_key.empty() && parsed.api_key_env.empty() &&
        key == "api_key_env" && !Trim(value).empty()) {
      parsed.api_key_env = value;
    }
  }
  return parsed;
}

std::string Value(const std::unordered_map<std::string, std::string>& values,
                  const std::string& key,
                  const std::string& fallback = {}) {
  const auto it = values.find(key);
  return it == values.end() ? fallback : it->second;
}

}  // namespace

std::string DefaultSystemPrompt() {
  return "Rewrite the user's current message as a clear, concise prompt for an "
         "AI coding agent.\n\n"
         "Preserve the user's exact intent and scope. Improve spelling, "
         "grammar, wording, and structure so the request is easier to act on. "
         "Keep the result direct and specific, but do not add requirements, "
         "assumptions, implementation ideas, architecture, tests, constraints, "
         "or extra detail that the user did not ask for.\n\n"
         "Preserve meaningful code, commands, file paths, identifiers, flags, "
         "config keys, numbers, quoted text, and markup exactly.\n\n"
         "If prior user messages are provided as context, use them only to "
         "resolve references in the current message. Do not summarize or "
         "include prior messages unless the current message explicitly asks "
         "for that.\n\n"
         "Return only the rewritten current prompt.";
}

std::optional<ApiFormat> ParseApiFormat(const std::string& value) {
  const auto normalized = Lower(Trim(value));
  if (normalized == "openai_chat") {
    return ApiFormat::OpenAiChat;
  }
  if (normalized == "openai_responses") {
    return ApiFormat::OpenAiResponses;
  }
  if (normalized == "anthropic_messages") {
    return ApiFormat::AnthropicMessages;
  }
  return std::nullopt;
}

std::optional<NotificationMode> ParseNotificationMode(const std::string& value) {
  const auto normalized = Lower(Trim(value));
  if (normalized == "cli") {
    return NotificationMode::Cli;
  }
  if (normalized == "always") {
    return NotificationMode::Always;
  }
  if (normalized == "off") {
    return NotificationMode::Off;
  }
  return std::nullopt;
}

std::optional<NotificationEvents> ParseNotificationEvents(
    const std::string& value) {
  const auto normalized = Lower(Trim(value));
  if (normalized == "errors") {
    return NotificationEvents::Errors;
  }
  if (normalized == "completion") {
    return NotificationEvents::Completion;
  }
  if (normalized == "lifecycle" || normalized == "all") {
    return NotificationEvents::All;
  }
  return std::nullopt;
}

std::optional<PasteShortcut> ParsePasteShortcut(const std::string& value) {
  const auto normalized = Lower(Trim(value));
  if (normalized == "ctrl_v") {
    return PasteShortcut::CtrlV;
  }
  if (normalized == "ctrl_shift_v") {
    return PasteShortcut::CtrlShiftV;
  }
  if (normalized == "shift_insert") {
    return PasteShortcut::ShiftInsert;
  }
  return std::nullopt;
}

std::string ToString(ApiFormat format) {
  switch (format) {
    case ApiFormat::OpenAiChat:
      return "openai_chat";
    case ApiFormat::OpenAiResponses:
      return "openai_responses";
    case ApiFormat::AnthropicMessages:
      return "anthropic_messages";
  }
  return "openai_chat";
}

std::string ToString(NotificationMode mode) {
  switch (mode) {
    case NotificationMode::Cli:
      return "cli";
    case NotificationMode::Always:
      return "always";
    case NotificationMode::Off:
      return "off";
  }
  return "cli";
}

std::string ToString(NotificationEvents events) {
  switch (events) {
    case NotificationEvents::Errors:
      return "errors";
    case NotificationEvents::Completion:
      return "completion";
    case NotificationEvents::All:
      return "lifecycle";
  }
  return "lifecycle";
}

std::string ToString(PasteShortcut shortcut) {
  switch (shortcut) {
    case PasteShortcut::CtrlV:
      return "ctrl_v";
    case PasteShortcut::CtrlShiftV:
      return "ctrl_shift_v";
    case PasteShortcut::ShiftInsert:
      return "shift_insert";
  }
  return "ctrl_v";
}

AppConfig LoadConfig(const std::filesystem::path& config_path) {
  AppConfig config;
  const auto parsed = ParseIni(config_path);
  const auto& values = parsed.values;

  config.provider = Value(values, "provider", config.provider);
  config.base_url = Value(values, "base_url", config.base_url);
  config.model = Value(values, "model", config.model);
  config.input_mode = Lower(Value(values, "input", config.input_mode));
  config.output_mode = Lower(Value(values, "output", config.output_mode));
  config.api_key = parsed.api_key;
  if (!parsed.api_key_env.empty()) {
    config.api_key_env = parsed.api_key_env;
  }
  config.bearer_token_env =
      Value(values, "bearer_token_env", config.bearer_token_env);
  config.reasoning_effort =
      Lower(Value(values, "reasoning_effort", config.reasoning_effort));
  config.history_enabled =
      BoolValue(Value(values, "history_enabled"), config.history_enabled);
  config.min_output_tokens =
      IntValue(Value(values, "min_output_tokens"), config.min_output_tokens);
  config.max_output_tokens_limit = IntValue(
      Value(values, "max_output_tokens_limit"), config.max_output_tokens_limit);
  config.output_token_multiplier = DoubleValue(
      Value(values, "output_token_multiplier"), config.output_token_multiplier);
  config.output_token_padding = IntValue(Value(values, "output_token_padding"),
                                         config.output_token_padding);
  config.timeout =
      std::chrono::milliseconds(IntValue(Value(values, "timeout_ms"), 30000));

  if (const auto parsed = ParseApiFormat(Value(values, "api_format"))) {
    config.api_format = *parsed;
  }
  if (const auto parsed =
          ParseNotificationMode(Value(values, "notification_mode"))) {
    config.notification_mode = *parsed;
  }
  if (const auto parsed =
          ParseNotificationEvents(Value(values, "notification_events"))) {
    config.notification_events = *parsed;
  }
  if (const auto parsed = ParsePasteShortcut(Value(values, "paste_shortcut"))) {
    config.paste_shortcut = *parsed;
  }

  config.system_prompt = parsed.prompt;

  if (Trim(config.system_prompt).empty()) {
    config.system_prompt = DefaultSystemPrompt();
  }

  return config;
}

bool EnsureDefaultConfigFile(const std::filesystem::path& config_path) {
  if (config_path.empty() || std::filesystem::exists(config_path)) {
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(config_path.parent_path(), error);
  if (error) {
    return false;
  }

  std::ofstream output(config_path);
  if (!output) {
    return false;
  }

  output
      << "# llm-rewriter configuration\n"
      << "# Generated on first run. Uncomment options as needed.\n\n"
      << "model = openai/gpt-4.1-mini\n\n"
      << "# provider = openrouter\n"
      << "# api_format = openai_chat\n"
      << "# base_url = https://openrouter.ai/api/v1\n"
      << "# api_key_env = OPENROUTER_API_KEY\n"
      << "# api_key =\n"
      << "# bearer_token_env =\n\n"
      << "# input = clipboard\n"
      << "# output = preview\n\n"
      << "# reasoning_effort = off\n"
      << "# timeout_ms = 30000\n\n"
      << "# notification_mode = cli\n"
      << "# notification_events = lifecycle\n\n"
      << "# history_enabled = true\n"
      << "# min_output_tokens = 256\n"
      << "# max_output_tokens_limit = 65536\n"
      << "# output_token_multiplier = 1.5\n"
      << "# output_token_padding = 128\n\n"
      << "# paste_shortcut = ctrl_v\n\n"
      << "# system_prompt = <<EOF\n"
      << "# Rewrite the user's current message as a clear, concise prompt.\n"
      << "# EOF\n"
      << "# system_prompt_file = ~/path/to/prompt.md\n";
  return true;
}

void ApplyCliOverrides(AppConfig& config,
                       const std::optional<std::string>& model,
                       const std::optional<std::string>& reasoning_effort) {
  if (model && !Trim(*model).empty()) {
    config.model = Trim(*model);
  }
  if (reasoning_effort && !Trim(*reasoning_effort).empty()) {
    config.reasoning_effort = Lower(Trim(*reasoning_effort));
  }
}

}  // namespace llm_rewriter
