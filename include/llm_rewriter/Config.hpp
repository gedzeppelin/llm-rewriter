#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace llm_rewriter {

enum class ApiFormat {
  OpenAiChat,
  OpenAiResponses,
  AnthropicMessages,
};

enum class NotificationMode {
  Cli,
  Always,
  Off,
};

enum class NotificationEvents {
  Errors,
  Completion,
  All,
};

enum class PasteShortcut {
  CtrlV,
  CtrlShiftV,
  ShiftInsert,
};

struct CustomProviderSettings {
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> query_parameters;
  nlohmann::json request_body = nlohmann::json::object();
};

struct AppConfig {
  std::string provider = "openrouter";
  ApiFormat api_format = ApiFormat::OpenAiChat;
  std::string base_url = "https://openrouter.ai/api/v1";
  std::string model;
  std::string input_mode = "clipboard";
  std::string output_mode = "preview";
  // An optional provider credential entered by the user.  Environment
  // variables remain the first source for providers that define one.
  std::string credential;
  std::optional<std::filesystem::path> codex_auth_file =
      std::filesystem::path{"~/.codex/auth.json"};
  std::string reasoning = "none";
  std::chrono::milliseconds timeout{30000};
  bool history_enabled = true;
  int min_output_tokens = 256;
  int max_output_tokens_limit = 65536;
  double output_token_multiplier = 1.5;
  int output_token_padding = 128;
  NotificationMode notification_mode = NotificationMode::Cli;
  NotificationEvents notification_events = NotificationEvents::All;
  PasteShortcut paste_shortcut = PasteShortcut::CtrlV;
  CustomProviderSettings custom_provider;
  std::string system_prompt;
};

std::string DefaultSystemPrompt();
std::optional<ApiFormat> ParseApiFormat(const std::string& value);
std::optional<NotificationMode> ParseNotificationMode(const std::string& value);
std::optional<NotificationEvents> ParseNotificationEvents(const std::string& value);
std::optional<PasteShortcut> ParsePasteShortcut(const std::string& value);
std::string ToString(ApiFormat format);
std::string ToString(NotificationMode mode);
std::string ToString(NotificationEvents events);
std::string ToString(PasteShortcut shortcut);
AppConfig LoadConfig(const std::filesystem::path& config_path);
bool EnsureDefaultConfigFile(const std::filesystem::path& config_path);
bool SetConfigValue(const std::filesystem::path& config_path,
                    const std::string& key,
                    const std::string& value);
void ApplyCliOverrides(AppConfig& config,
                       const std::optional<std::string>& model,
                       const std::optional<std::string>& reasoning);

}  // namespace llm_rewriter
