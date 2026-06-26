#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "llm_rewriter/Cli.hpp"
#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/LlmClient.hpp"
#include "llm_rewriter/Notification.hpp"
#include "llm_rewriter/RewriteService.hpp"
#include "llm_rewriter/TokenEstimate.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace flr = llm_rewriter;

namespace {

std::filesystem::path WriteTempConfig(const std::string& name,
                                      const std::string& content) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream output(path);
  output << content;
  return path;
}

}  // namespace

TEST_CASE("INI parser handles heredoc and first-declared prompt and api key") {
  const auto prompt_file =
      WriteTempConfig("llm-rewriter-prompt-test.md", "prompt from file");
  const auto path = WriteTempConfig(
      "llm-rewriter-config-test.ini",
      "api_key_env = SHOULD_WIN\n"
      "api_key = should_not_win\n"
      "system_prompt_file = " +
          prompt_file.string() +
          "\n"
          "system_prompt = <<EOF\n"
          "inline prompt should not win\n"
          "EOF\n"
          "input = stdin\n"
          "output = stdout\n"
          "paste_shortcut = ctrl_shift_v\n");

  const auto config = flr::LoadConfig(path);
  CHECK(config.api_key.empty());
  CHECK(config.api_key_env == "SHOULD_WIN");
  CHECK(config.system_prompt == "prompt from file");
  CHECK(config.input_mode == "stdin");
  CHECK(config.output_mode == "stdout");
  CHECK(config.paste_shortcut == flr::PasteShortcut::CtrlShiftV);
}

TEST_CASE("CLI parsing rejects removed output and applies config defaults") {
  const auto invalid =
      flr::ParseCli(std::vector<std::string>{"--output", "replace"});
  CHECK(invalid.exit);
  CHECK_FALSE(invalid.options.has_value());

  auto parsed = flr::ParseCli(std::vector<std::string>{});
  REQUIRE(parsed.options.has_value());

  flr::AppConfig config;
  config.input_mode = "stdin";
  config.output_mode = "stdout";
  config.paste_shortcut = flr::PasteShortcut::ShiftInsert;
  flr::ApplyConfigDefaults(*parsed.options, config);
  CHECK(parsed.options->input == flr::InputMode::Stdin);
  CHECK(parsed.options->output == flr::OutputMode::Stdout);
#if defined(__linux__)
  CHECK(parsed.options->paste_shortcut == flr::PasteShortcut::ShiftInsert);
#endif
}

TEST_CASE("CLI model and reasoning flags override config") {
  auto parsed = flr::ParseCli(std::vector<std::string>{
      "--model", "provider/model", "--reasoning-effort", "medium"});
  REQUIRE(parsed.options.has_value());
  REQUIRE(parsed.options->model.has_value());
  REQUIRE(parsed.options->reasoning_effort.has_value());

  flr::AppConfig config;
  config.model = "config/model";
  config.reasoning_effort = "off";
  flr::ApplyCliOverrides(config, parsed.options->model,
                         parsed.options->reasoning_effort);
  CHECK(config.model == "provider/model");
  CHECK(config.reasoning_effort == "medium");

  auto invalid = flr::ParseCli(
      std::vector<std::string>{"--reasoning-effort", "maximum"});
  CHECK(invalid.exit);
  CHECK_FALSE(invalid.options.has_value());
}

TEST_CASE("first run config generation writes only model as active option") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "llm-rewriter-generated-config-test";
  const auto path = dir / "config.ini";
  std::filesystem::remove(path);
  std::filesystem::remove(dir);

  CHECK(flr::EnsureDefaultConfigFile(path));
  CHECK(std::filesystem::exists(path));

  std::ifstream input(path);
  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  CHECK(content.find("model = ") != std::string::npos);
  CHECK(content.find("# reasoning_effort = off") != std::string::npos);
  CHECK(content.find("# input = clipboard") != std::string::npos);
  CHECK_FALSE(flr::EnsureDefaultConfigFile(path));

  std::filesystem::remove(path);
  std::filesystem::remove(dir);
}

TEST_CASE("token estimation clamps with 64k default limit") {
  flr::AppConfig config;
  CHECK(config.max_output_tokens_limit == 65536);
  CHECK(flr::EstimateTokens("12345678") == 2);
  CHECK(flr::ChooseMaxOutputTokens(config, 1000000) == 65536);
}

TEST_CASE("LLM payloads omit reasoning when off and include it when enabled") {
  flr::AppConfig config;
  config.model = "test-model";
  config.system_prompt = "system";
  config.api_format = flr::ApiFormat::OpenAiChat;

  auto json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["model"] == "test-model");
  CHECK(json["messages"][0]["content"] == "system");
  CHECK_FALSE(json.contains("reasoning_effort"));

  config.reasoning_effort = "low";
  json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["reasoning_effort"] == "low");

  config.api_format = flr::ApiFormat::OpenAiResponses;
  json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["reasoning"]["effort"] == "low");

  config.api_format = flr::ApiFormat::AnthropicMessages;
  json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["system"] == "system");
  CHECK(json["messages"][0]["content"] == "hello");
}

TEST_CASE("notification policy separates CLI and UI defaults") {
  flr::AppConfig config;
  CHECK(flr::ShouldNotifyForTest(config, flr::RewriteContext::Cli,
                                 flr::NotificationKind::Started));
  CHECK_FALSE(flr::ShouldNotifyForTest(config, flr::RewriteContext::Ui,
                                       flr::NotificationKind::Started));

  config.notification_mode = flr::NotificationMode::Always;
  config.notification_events = flr::NotificationEvents::Errors;
  CHECK_FALSE(flr::ShouldNotifyForTest(config, flr::RewriteContext::Ui,
                                       flr::NotificationKind::Succeeded));
  CHECK(flr::ShouldNotifyForTest(config, flr::RewriteContext::Ui,
                                 flr::NotificationKind::Failed));
}
