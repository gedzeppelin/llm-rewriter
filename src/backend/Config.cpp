#include "llm_rewriter/Config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace llm_rewriter {
namespace {

using Json = nlohmann::json;

std::string Trim(std::string_view value) {
  auto first = value.begin();
  auto last = value.end();
  while (first != last && std::isspace(static_cast<unsigned char>(*first))) {
    ++first;
  }
  while (first != last &&
         std::isspace(static_cast<unsigned char>(*(last - 1)))) {
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

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

std::filesystem::path ExpandConfigPath(const std::string& path) {
  const char* home = std::getenv("HOME");
#if defined(_WIN32)
  if (home == nullptr || *home == '\0') home = std::getenv("USERPROFILE");
#endif
  if (path == "~" && home != nullptr && *home != '\0') return home;
  if (path.rfind("~/", 0) == 0) {
    if (home != nullptr && *home != '\0') {
      return std::filesystem::path{home} / path.substr(2);
    }
  }
  return path;
}

const Json& ObjectValue(const Json& parent, const char* key) {
  static const Json empty = Json::object();
  if (!parent.is_object()) {
    return empty;
  }
  const auto it = parent.find(key);
  if (it == parent.end() || !it->is_object()) {
    return empty;
  }
  return *it;
}

Json& EnsureObject(Json& parent, const char* key) {
  if (!parent.is_object()) {
    parent = Json::object();
  }
  auto& value = parent[key];
  if (!value.is_object()) {
    value = Json::object();
  }
  return value;
}

std::string StringValue(const Json& parent,
                        const char* key,
                        const std::string& fallback = {}) {
  if (!parent.is_object()) {
    return fallback;
  }
  const auto it = parent.find(key);
  if (it == parent.end()) {
    return fallback;
  }
  if (it->is_string()) {
    return it->get<std::string>();
  }
  return fallback;
}

bool BoolValue(const Json& parent, const char* key, bool fallback) {
  if (!parent.is_object()) {
    return fallback;
  }
  const auto it = parent.find(key);
  if (it == parent.end()) {
    return fallback;
  }
  if (it->is_boolean()) {
    return it->get<bool>();
  }
  return fallback;
}

int IntValue(const Json& parent, const char* key, int fallback) {
  if (!parent.is_object()) {
    return fallback;
  }
  const auto it = parent.find(key);
  if (it == parent.end()) {
    return fallback;
  }
  if (it->is_number_integer()) {
    return it->get<int>();
  }
  return fallback;
}

double DoubleValue(const Json& parent, const char* key, double fallback) {
  if (!parent.is_object()) {
    return fallback;
  }
  const auto it = parent.find(key);
  if (it == parent.end()) {
    return fallback;
  }
  if (it->is_number()) {
    return it->get<double>();
  }
  return fallback;
}

std::string ScalarToString(const Json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  if (value.is_number()) {
    return value.dump();
  }
  return {};
}

std::map<std::string, std::string> StringMapValue(const Json& object) {
  std::map<std::string, std::string> values;
  if (!object.is_object()) {
    return values;
  }
  for (const auto& item : object.items()) {
    auto value = ScalarToString(item.value());
    if (!item.key().empty() && !value.empty()) {
      values.emplace(item.key(), std::move(value));
    }
  }
  return values;
}

bool ParseBool(std::string_view value, bool& output) {
  const auto normalized = Lower(Trim(value));
  if (normalized == "true" || normalized == "1" || normalized == "yes" ||
      normalized == "on") {
    output = true;
    return true;
  }
  if (normalized == "false" || normalized == "0" || normalized == "no" ||
      normalized == "off") {
    output = false;
    return true;
  }
  return false;
}

bool ParseInt(std::string_view value, int& output) {
  try {
    std::size_t parsed = 0;
    const auto text = Trim(value);
    const int number = std::stoi(text, &parsed);
    if (parsed != text.size()) {
      return false;
    }
    output = number;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDouble(std::string_view value, double& output) {
  try {
    std::size_t parsed = 0;
    const auto text = Trim(value);
    const double number = std::stod(text, &parsed);
    if (parsed != text.size()) {
      return false;
    }
    output = number;
    return true;
  } catch (...) {
    return false;
  }
}

bool IsInputMode(const std::string& value) {
  return value == "clipboard" || value == "primary" || value == "stdin";
}

bool IsOutputMode(const std::string& value) {
  return value == "preview" || value == "clipboard" || value == "stdout" ||
         value == "type" || value == "paste";
}

Json DefaultConfigDocument() {
  AppConfig config;
  return Json{
      {"provider",
       Json{{"name", config.provider},
            {"api_format", ToString(config.api_format)},
            {"base_url", config.base_url},
            {"model", "openai/gpt-4.1-mini"}}},
      {"credentials",
       Json{{"credential", config.credential},
            {"codex_auth_file", "~/.codex/auth.json"}}},
      {"workflow",
       Json{{"input", config.input_mode},
            {"output", config.output_mode},
            {"paste_shortcut", ToString(config.paste_shortcut)}}},
      {"generation",
       Json{{"reasoning", config.reasoning},
            {"history_enabled", config.history_enabled},
            {"timeout_ms", static_cast<int>(config.timeout.count())},
            {"min_output_tokens", config.min_output_tokens},
            {"max_output_tokens_limit", config.max_output_tokens_limit},
            {"output_token_multiplier", config.output_token_multiplier},
            {"output_token_padding", config.output_token_padding}}},
      {"notifications",
       Json{{"mode", ToString(config.notification_mode)},
            {"events", ToString(config.notification_events)}}},
      {"custom_provider",
       Json{{"headers", Json::object()},
            {"query_parameters", Json::object()},
            {"request_body", Json::object()}}},
      {"system_prompt", ""},
      {"system_prompt_file", ""}};
}

void MergeMissing(Json& target, const Json& defaults) {
  if (!target.is_object()) {
    target = Json::object();
  }
  for (const auto& item : defaults.items()) {
    auto& current = target[item.key()];
    if (current.is_null()) {
      current = item.value();
      continue;
    }
    if (current.is_object() && item.value().is_object()) {
      MergeMissing(current, item.value());
    }
  }
}

Json ReadConfigJson(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return Json::object();
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto content = Trim(buffer.str());
  if (content.empty()) {
    return Json::object();
  }
  auto parsed = Json::parse(content);
  if (!parsed.is_object()) {
    throw std::runtime_error("configuration JSON root must be an object");
  }
  return parsed;
}

bool WriteConfigJson(const std::filesystem::path& path, const Json& document) {
  if (path.empty()) {
    return false;
  }
  if (path.has_parent_path()) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return false;
    }
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    return false;
  }
  output << document.dump(2) << '\n';
  return true;
}

AppConfig LoadConfigFromJson(const Json& document) {
  AppConfig config;
  const auto& provider = ObjectValue(document, "provider");
  const auto& credentials = ObjectValue(document, "credentials");
  const auto& workflow = ObjectValue(document, "workflow");
  const auto& generation = ObjectValue(document, "generation");
  const auto& notifications = ObjectValue(document, "notifications");
  const auto& custom_provider = ObjectValue(document, "custom_provider");

  config.provider = Lower(Trim(StringValue(provider, "name", config.provider)));
  if (config.provider.empty()) {
    config.provider = "openrouter";
  }
  config.base_url = StringValue(provider, "base_url", config.base_url);
  config.model = StringValue(provider, "model", config.model);
  config.input_mode =
      Lower(StringValue(workflow, "input", config.input_mode));
  config.output_mode =
      Lower(StringValue(workflow, "output", config.output_mode));
  config.credential = StringValue(credentials, "credential", config.credential);
  if (const auto it = credentials.find("codex_auth_file");
      it != credentials.end() && it->is_string()) {
    const auto codex_auth_file = Trim(it->get<std::string>());
    if (codex_auth_file.empty()) {
      config.codex_auth_file = std::nullopt;
    } else {
      config.codex_auth_file = std::filesystem::path{codex_auth_file};
    }
  }
  config.reasoning =
      Lower(Trim(StringValue(generation, "reasoning", config.reasoning)));
  if (config.reasoning.empty()) {
    config.reasoning = "none";
  }
  config.history_enabled =
      BoolValue(generation, "history_enabled", config.history_enabled);
  config.min_output_tokens =
      IntValue(generation, "min_output_tokens", config.min_output_tokens);
  config.max_output_tokens_limit =
      IntValue(generation, "max_output_tokens_limit",
               config.max_output_tokens_limit);
  config.output_token_multiplier =
      DoubleValue(generation, "output_token_multiplier",
                  config.output_token_multiplier);
  config.output_token_padding =
      IntValue(generation, "output_token_padding", config.output_token_padding);
  config.timeout = std::chrono::milliseconds(
      IntValue(generation, "timeout_ms", static_cast<int>(config.timeout.count())));

  if (const auto parsed =
          ParseApiFormat(StringValue(provider, "api_format"))) {
    config.api_format = *parsed;
  }
  if (const auto parsed =
          ParseNotificationMode(StringValue(notifications, "mode"))) {
    config.notification_mode = *parsed;
  }
  if (const auto parsed =
          ParseNotificationEvents(StringValue(notifications, "events"))) {
    config.notification_events = *parsed;
  }
  if (const auto parsed =
          ParsePasteShortcut(StringValue(workflow, "paste_shortcut"))) {
    config.paste_shortcut = *parsed;
  }

  config.custom_provider.headers =
      StringMapValue(ObjectValue(custom_provider, "headers"));
  config.custom_provider.query_parameters =
      StringMapValue(ObjectValue(custom_provider, "query_parameters"));
  const auto& request_body = ObjectValue(custom_provider, "request_body");
  config.custom_provider.request_body =
      request_body.is_object() ? request_body : Json::object();

  config.system_prompt = StringValue(document, "system_prompt");
  if (Trim(config.system_prompt).empty()) {
    const auto prompt_file = Trim(StringValue(document, "system_prompt_file"));
    if (!prompt_file.empty()) {
      config.system_prompt = ReadTextFile(ExpandConfigPath(prompt_file));
    }
  }
  if (Trim(config.system_prompt).empty()) {
    config.system_prompt = DefaultSystemPrompt();
  }

  return config;
}

bool SetJsonObjectFromStringMap(Json& target,
                                const char* key,
                                const std::string& value) {
  const auto parsed = Json::parse(value, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return false;
  }
  Json object = Json::object();
  for (const auto& item : parsed.items()) {
    const auto scalar = ScalarToString(item.value());
    if (item.key().empty() || scalar.empty()) {
      return false;
    }
    object[item.key()] = scalar;
  }
  target[key] = std::move(object);
  return true;
}

bool SetCustomProviderValue(Json& document,
                            const std::string& key,
                            const std::string& value) {
  constexpr std::string_view headers_prefix = "custom_provider.headers.";
  constexpr std::string_view query_prefix =
      "custom_provider.query_parameters.";
  constexpr std::string_view body_prefix = "custom_provider.request_body.";

  auto& custom = EnsureObject(document, "custom_provider");
  auto& headers = EnsureObject(custom, "headers");
  auto& query = EnsureObject(custom, "query_parameters");
  auto& body = EnsureObject(custom, "request_body");

  if (key == "custom_provider.headers") {
    return SetJsonObjectFromStringMap(custom, "headers", value);
  }
  if (key == "custom_provider.query_parameters") {
    return SetJsonObjectFromStringMap(custom, "query_parameters", value);
  }
  if (key == "custom_provider.request_body") {
    const auto parsed = Json::parse(value, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return false;
    }
    custom["request_body"] = parsed;
    return true;
  }
  if (key.rfind(headers_prefix, 0) == 0) {
    const auto name = key.substr(headers_prefix.size());
    if (name.empty()) {
      return false;
    }
    headers[name] = value;
    return true;
  }
  if (key.rfind(query_prefix, 0) == 0) {
    const auto name = key.substr(query_prefix.size());
    if (name.empty()) {
      return false;
    }
    query[name] = value;
    return true;
  }
  if (key.rfind(body_prefix, 0) == 0) {
    const auto name = key.substr(body_prefix.size());
    if (name.empty()) {
      return false;
    }
    const auto parsed = Json::parse(value, nullptr, false);
    body[name] = parsed.is_discarded() ? Json(value) : parsed;
    return true;
  }
  return false;
}

bool ApplyConfigValue(Json& document,
                      const std::string& key,
                      const std::string& value) {
  auto& provider = EnsureObject(document, "provider");
  auto& credentials = EnsureObject(document, "credentials");
  auto& workflow = EnsureObject(document, "workflow");
  auto& generation = EnsureObject(document, "generation");
  auto& notifications = EnsureObject(document, "notifications");

  const auto string_value = Trim(value);
  if (key == "provider") {
    provider["name"] = string_value;
  } else if (key == "api_format") {
    const auto parsed = ParseApiFormat(string_value);
    if (!parsed) {
      return false;
    }
    provider["api_format"] = ToString(*parsed);
  } else if (key == "base_url") {
    provider["base_url"] = string_value;
  } else if (key == "model") {
    provider["model"] = string_value;
  } else if (key == "input") {
    const auto normalized = Lower(string_value);
    if (!IsInputMode(normalized)) {
      return false;
    }
    workflow["input"] = normalized;
  } else if (key == "output") {
    const auto normalized = Lower(string_value);
    if (!IsOutputMode(normalized)) {
      return false;
    }
    workflow["output"] = normalized;
  } else if (key == "paste_shortcut") {
    const auto parsed = ParsePasteShortcut(string_value);
    if (!parsed) {
      return false;
    }
    workflow["paste_shortcut"] = ToString(*parsed);
  } else if (key == "credential") {
    credentials["credential"] = value;
  } else if (key == "codex_auth_file") {
    const auto path = ExpandConfigPath(string_value);
    if (string_value.empty()) {
      credentials["codex_auth_file"] = "";
      return true;
    }
    if (!path.is_absolute()) {
      return false;
    }
    credentials["codex_auth_file"] = string_value;
  } else if (key == "reasoning") {
    generation["reasoning"] = string_value.empty() ? "none" : Lower(string_value);
  } else if (key == "history_enabled") {
    bool parsed = false;
    if (!ParseBool(value, parsed)) {
      return false;
    }
    generation["history_enabled"] = parsed;
  } else if (key == "timeout_ms") {
    int parsed = 0;
    if (!ParseInt(value, parsed) || parsed <= 0) {
      return false;
    }
    generation["timeout_ms"] = parsed;
  } else if (key == "min_output_tokens") {
    int parsed = 0;
    if (!ParseInt(value, parsed) || parsed < 0) {
      return false;
    }
    generation["min_output_tokens"] = parsed;
  } else if (key == "max_output_tokens_limit") {
    int parsed = 0;
    if (!ParseInt(value, parsed) || parsed <= 0) {
      return false;
    }
    generation["max_output_tokens_limit"] = parsed;
  } else if (key == "output_token_multiplier") {
    double parsed = 0.0;
    if (!ParseDouble(value, parsed) || parsed <= 0.0) {
      return false;
    }
    generation["output_token_multiplier"] = parsed;
  } else if (key == "output_token_padding") {
    int parsed = 0;
    if (!ParseInt(value, parsed) || parsed < 0) {
      return false;
    }
    generation["output_token_padding"] = parsed;
  } else if (key == "notification_mode") {
    const auto parsed = ParseNotificationMode(string_value);
    if (!parsed) {
      return false;
    }
    notifications["mode"] = ToString(*parsed);
  } else if (key == "notification_events") {
    const auto parsed = ParseNotificationEvents(string_value);
    if (!parsed) {
      return false;
    }
    notifications["events"] = ToString(*parsed);
  } else if (key == "system_prompt") {
    document["system_prompt"] = value;
  } else if (key == "system_prompt_file") {
    document["system_prompt_file"] = string_value;
  } else if (!SetCustomProviderValue(document, key, value)) {
    return false;
  }
  return true;
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
  return LoadConfigFromJson(ReadConfigJson(config_path));
}

bool EnsureDefaultConfigFile(const std::filesystem::path& config_path) {
  if (config_path.empty() || std::filesystem::exists(config_path)) {
    return false;
  }
  return WriteConfigJson(config_path, DefaultConfigDocument());
}

bool SetConfigValue(const std::filesystem::path& config_path,
                    const std::string& key,
                    const std::string& value) {
  if (config_path.empty() || Trim(key).empty()) {
    return false;
  }
  try {
    auto document = ReadConfigJson(config_path);
    MergeMissing(document, DefaultConfigDocument());
    if (document.contains("credentials") &&
        document["credentials"].is_object()) {
      document["credentials"].erase("api_key");
      document["credentials"].erase("api_key_env");
      document["credentials"].erase("bearer_token_env");
    }
    if (!ApplyConfigValue(document, Trim(key), value)) {
      return false;
    }
    (void)LoadConfigFromJson(document);
    return WriteConfigJson(config_path, document);
  } catch (...) {
    return false;
  }
}

void ApplyCliOverrides(AppConfig& config,
                       const std::optional<std::string>& model,
                       const std::optional<std::string>& reasoning) {
  if (model && !Trim(*model).empty()) {
    config.model = Trim(*model);
  }
  if (reasoning && !Trim(*reasoning).empty()) {
    config.reasoning = Lower(Trim(*reasoning));
  }
}

}  // namespace llm_rewriter
