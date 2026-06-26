#pragma once

#include <chrono>
#include <filesystem>
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

struct AppConfig {
  std::string provider = "openrouter";
  ApiFormat api_format = ApiFormat::OpenAiChat;
  std::string base_url = "https://openrouter.ai/api/v1";
  std::string model;
  std::string input_mode = "clipboard";
  std::string output_mode = "preview";
  std::string api_key;
  std::string api_key_env = "OPENROUTER_API_KEY";
  std::string bearer_token_env;
  std::string reasoning_effort = "off";
  std::chrono::milliseconds timeout{30000};
  bool history_enabled = true;
  int min_output_tokens = 256;
  int max_output_tokens_limit = 65536;
  double output_token_multiplier = 1.5;
  int output_token_padding = 128;
  NotificationMode notification_mode = NotificationMode::Cli;
  NotificationEvents notification_events = NotificationEvents::All;
  PasteShortcut paste_shortcut = PasteShortcut::CtrlV;
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
void ApplyCliOverrides(AppConfig& config,
                       const std::optional<std::string>& model,
                       const std::optional<std::string>& reasoning_effort);

}  // namespace llm_rewriter
