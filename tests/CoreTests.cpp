#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "llm_rewriter/Cli.hpp"
#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/Credentials.hpp"
#include "llm_rewriter/Doctor.hpp"
#include "llm_rewriter/Diagnostics.hpp"
#include "llm_rewriter/History.hpp"
#include "llm_rewriter/LlmClient.hpp"
#include "llm_rewriter/Notification.hpp"
#include "llm_rewriter/RewriteService.hpp"
#include "llm_rewriter/TokenEstimate.hpp"
#include "llm_rewriter/abi/backend.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace flr = llm_rewriter;

namespace {

std::filesystem::path WriteTempConfig(const std::string& name,
                                      const std::string& content) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream output(path);
  output << content;
  return path;
}

std::string Base64Url(std::string_view input) {
  constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char character : input) {
    buffer = (buffer << 8U) | character;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      output.push_back(alphabet[(buffer >> bits) & 0x3fU]);
    }
  }
  if (bits > 0) {
    output.push_back(alphabet[(buffer << (6 - bits)) & 0x3fU]);
  }
  return output;
}

std::string Jwt(std::uint64_t expiry, std::string_view account) {
  const auto header = Base64Url(R"({"alg":"none"})");
  const auto payload = Base64Url(
      nlohmann::json{{"exp", expiry}, {"chatgpt_account_id", account}}.dump());
  return header + "." + payload + ".signature";
}

struct TestStore final : flr::ICredentialStore {
  std::map<std::string, std::string> values;
  int reads = 0;
  std::optional<std::string> Get(std::string_view account) override {
    ++reads;
    if (const auto it = values.find(std::string{account}); it != values.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  bool Set(std::string_view account, std::string_view secret) override {
    values[std::string{account}] = secret;
    return true;
  }
  bool Remove(std::string_view account) override {
    values.erase(std::string{account});
    return true;
  }
  flr::CredentialError LastError() const override { return {}; }
};

struct TestEnvironment final : flr::ICredentialEnvironment {
  std::map<std::string, std::string> values;
  std::optional<std::string> Get(std::string_view name) const override {
    if (const auto it = values.find(std::string{name}); it != values.end()) {
      return it->second;
    }
    return std::nullopt;
  }
};

struct TestClock final : flr::ICredentialClock {
  std::uint64_t now = 1000;
  std::uint64_t NowEpochSeconds() const override { return now; }
  void SleepFor(std::chrono::milliseconds duration) const override {
    const_cast<TestClock*>(this)->now +=
        static_cast<std::uint64_t>(duration.count() / 1000);
  }
};

struct TestRandom final : flr::ICredentialRandom {
  int calls = 0;
  std::string UrlSafe(std::size_t byte_count) override {
    ++calls;
    return std::string(byte_count, static_cast<char>('a' + calls));
  }
};

struct TestHttp final : flr::IHttpTransport {
  std::vector<flr::HttpRequest> requests;
  std::vector<flr::HttpResponse> responses;
  flr::HttpResponse PostJson(const flr::HttpRequest& request) override {
    requests.push_back(request);
    if (responses.empty()) {
      return {.status = 200,
              .body = R"({"output_text":"rewritten"})",
              .error = {}};
    }
    auto response = responses.front();
    responses.erase(responses.begin());
    return response;
  }
  flr::HttpResponse Send(const flr::HttpRequest& request) override {
    requests.push_back(request);
    if (responses.empty()) {
      return {.status = 200,
              .body = R"({"output_text":"rewritten"})",
              .error = {}};
    }
    auto response = responses.front();
    responses.erase(responses.begin());
    return response;
  }
};

struct TestDiagnosticSink final : flr::IDiagnosticSink {
  std::vector<flr::DiagnosticEvent> events;
  void Record(const flr::DiagnosticEvent& event) override {
    events.push_back(event);
  }
};

int PostJsonForAbiTest(void*,
                       const llmr_http_request* request,
                       llmr_http_response* response) {
  if (request == nullptr || response == nullptr || request->url == nullptr ||
      std::string{request->url}.find("/chat/completions") == std::string::npos) {
    return 1;
  }
  response->status = 200;
  response->body = R"({"choices":[{"message":{"content":"rewritten"}}]})";
  response->error = "";
  return 0;
}

}  // namespace

TEST_CASE("JSON config parser handles sections and custom provider fields") {
  const auto path =
      WriteTempConfig("llm-rewriter-config-test.json",
                      nlohmann::json{
                          {"provider",
                           {{"name", "custom"},
                            {"api_format", "openai_responses"},
                            {"base_url", "https://example.test/v1"},
                            {"model", "provider/model"}}},
                          {"credentials", {{"credential", "configured-secret"}}},
                          {"workflow",
                           {{"input", "stdin"},
                            {"output", "stdout"},
                            {"paste_shortcut", "ctrl_shift_v"}}},
                          {"generation", {{"reasoning", "high"}}},
                          {"custom_provider",
                           {{"headers", {{"X-Test", "yes"}}},
                            {"query_parameters", {{"api-version", "2026-01-01"}}},
                            {"request_body", {{"metadata", {{"source", "test"}}}}}}},
                          {"system_prompt_file", "prompt-from-file.md"}}
                          .dump(2));

  const auto config = flr::LoadConfig(path);
  CHECK(config.credential == "configured-secret");
  CHECK(config.system_prompt == flr::DefaultSystemPrompt());
  CHECK(config.input_mode == "stdin");
  CHECK(config.output_mode == "stdout");
  CHECK(config.reasoning == "high");
  CHECK(config.paste_shortcut == flr::PasteShortcut::CtrlShiftV);
  CHECK(config.custom_provider.headers.at("X-Test") == "yes");
  CHECK(config.custom_provider.query_parameters.at("api-version") ==
        "2026-01-01");
  CHECK(config.custom_provider.request_body["metadata"]["source"] == "test");
}

TEST_CASE("credential precedence rejects invalid defined environment values") {
  auto store = std::make_shared<TestStore>();
  store->values["openrouter-api-key"] = "stored";
  auto environment = std::make_shared<TestEnvironment>();
  environment->values["OPENROUTER_API_KEY"] = "";
  flr::CredentialDependencies dependencies;
  dependencies.store = store;
  dependencies.environment = environment;
  flr::CredentialResolver resolver(std::nullopt, dependencies);

  const auto result = resolver.Resolve("openrouter");
  CHECK_FALSE(result.ok());
  CHECK(result.error.code == flr::CredentialErrorCode::InvalidCredential);
  CHECK(store->reads == 0);
}

TEST_CASE("canonical provider environment names are used by default") {
  auto environment = std::make_shared<TestEnvironment>();
  environment->values["OPENAI_API_KEY"] = "environment-secret";
  auto store = std::make_shared<TestStore>();
  flr::CredentialDependencies dependencies;
  dependencies.environment = environment;
  dependencies.store = store;
  flr::CredentialResolver resolver(std::nullopt, dependencies);
  const auto result = resolver.Resolve("openai");
  REQUIRE(result.ok());
  CHECK(result.source == flr::CredentialSource::Environment);
  CHECK(result.credential->access_token == "environment-secret");
  CHECK(store->reads == 0);
}

TEST_CASE("custom provider accepts optional configuration credentials") {
  auto environment = std::make_shared<TestEnvironment>();
  auto store = std::make_shared<TestStore>();
  flr::CredentialDependencies dependencies;
  dependencies.environment = environment;
  dependencies.store = store;
  flr::CredentialResolver resolver(std::nullopt, dependencies);

  const auto keyless = resolver.Resolve("custom");
  REQUIRE(keyless.ok());
  CHECK(keyless.source == flr::CredentialSource::Keyless);
  CHECK_FALSE(keyless.credential.has_value());

  const auto configured = resolver.Resolve("custom", "local-secret");
  REQUIRE(configured.ok());
  CHECK(configured.source == flr::CredentialSource::Configuration);
  REQUIRE(configured.credential.has_value());
  CHECK(configured.credential->access_token == "local-secret");
  CHECK(store->reads == 0);
}

TEST_CASE("removed provider names are not accepted") {
  CHECK_FALSE(flr::ParseCredentialProvider("ollama").has_value());
  CHECK_FALSE(flr::ParseCredentialProvider("openai_compatible").has_value());
}

TEST_CASE("external Codex auth paths must be absolute") {
  flr::CredentialResolver resolver(std::filesystem::path{"relative-auth.json"});
  CHECK(resolver.ConstructionError().code ==
        flr::CredentialErrorCode::ExternalPathRejected);
  CHECK(resolver.Resolve("codex").error.code ==
        flr::CredentialErrorCode::ExternalPathRejected);
}

#if !defined(_WIN32)
TEST_CASE("tilde Codex auth paths resolve against the home directory") {
  const auto directory =
      std::filesystem::temp_directory_path() / "llm-rewriter-home-test";
  std::filesystem::create_directories(directory / ".codex");
  const auto path = directory / ".codex" / "auth.json";
  {
    std::ofstream output(path);
    output << nlohmann::json{{"access_token", Jwt(2000, "account-home")}}
                      .dump();
  }

  const char *previous_home = std::getenv("HOME");
  const std::string previous_value = previous_home ? previous_home : "";
  setenv("HOME", directory.c_str(), 1);
  flr::CredentialDependencies dependencies;
  dependencies.clock = std::make_shared<TestClock>();
  flr::CredentialResolver resolver(std::filesystem::path{"~/.codex/auth.json"},
                                   dependencies);
  const auto result = resolver.Resolve("codex");
  REQUIRE(result.ok());
  CHECK(result.source == flr::CredentialSource::ExternalFile);
  if (previous_home) {
    setenv("HOME", previous_value.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::filesystem::remove(path);
  std::filesystem::remove_all(directory);
}
#endif

TEST_CASE("codex external auth file is read without ownership transfer") {
  const auto directory =
      std::filesystem::temp_directory_path() / "llm-rewriter-credentials-test";
  std::filesystem::create_directories(directory);
  const auto path = directory / "auth.json";
  const auto access = Jwt(2000, "account-123");
  {
    std::ofstream output(path);
    output << nlohmann::json{{"tokens",
                              {{"access_token", access},
                               {"refresh_token", "refresh"},
                               {"account_id", "account-123"}}},
                             {"unrelated", "preserved"}}
                    .dump();
  }
  auto clock = std::make_shared<TestClock>();
  flr::CredentialDependencies dependencies;
  dependencies.clock = clock;
  flr::CredentialResolver resolver(path, dependencies);
  const auto result = resolver.Resolve("codex");
  REQUIRE(result.ok());
  REQUIRE(result.credential.has_value());
  CHECK(result.source == flr::CredentialSource::ExternalFile);
  CHECK(result.credential->account_id == std::optional<std::string>{"account-123"});
  CHECK(result.credential->RedactedDescription().find(access) ==
        std::string::npos);

#if !defined(_WIN32)
  const auto link = directory / "auth-link.json";
  std::error_code error;
  std::filesystem::create_symlink(path, link, error);
  if (!error) {
    flr::CredentialResolver linked(link, dependencies);
    const auto linked_result = linked.Resolve("codex");
    CHECK(linked_result.error.code ==
          flr::CredentialErrorCode::ExternalFileRejected);
    std::filesystem::remove(link, error);
  }
#endif
  std::filesystem::remove(path);
  std::filesystem::remove(directory);
}

TEST_CASE("stale external codex credentials refresh and rotate only token fields") {
  const auto directory =
      std::filesystem::temp_directory_path() / "llm-rewriter-refresh-test";
  std::filesystem::create_directories(directory);
  const auto path = directory / "auth.json";
  {
    std::ofstream output(path);
    output << nlohmann::json{{"access_token", Jwt(1010, "account-123")},
                             {"refresh_token", "old-refresh"},
                             {"account_id", "account-123"},
                             {"unrelated", {{"keep", true}}}}
                    .dump();
  }
  auto clock = std::make_shared<TestClock>();
  auto http = std::make_shared<TestHttp>();
  http->responses.push_back(
      {.status = 200,
       .body = nlohmann::json{{"access_token", Jwt(3000, "account-123")},
                              {"refresh_token", "new-refresh"}}
                   .dump(),
       .error = {}});
  flr::CredentialDependencies dependencies;
  dependencies.clock = clock;
  dependencies.http = http.get();
  flr::CredentialResolver resolver(path, dependencies);
  const auto result = resolver.Resolve("codex");
  REQUIRE(result.ok());
  CHECK(result.credential->access_token == Jwt(3000, "account-123"));
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto document = nlohmann::json::parse(contents.str());
  CHECK(document["refresh_token"] == "new-refresh");
  CHECK(document["unrelated"]["keep"] == true);
  CHECK(http->requests.size() == 1);
  CHECK(http->requests.front().content_type ==
        "application/x-www-form-urlencoded");

  std::filesystem::remove(path);
  std::filesystem::remove(directory);
}

TEST_CASE("external Codex ownership blocks configuration and clearing") {
  const auto path =
      std::filesystem::temp_directory_path() / "llm-rewriter-owned-auth.json";
  flr::CredentialResolver resolver(path);
  CHECK(resolver.ConfigureApiKey("codex", "refresh") .code ==
        flr::CredentialErrorCode::ExternalCredentialUnowned);
  CHECK(resolver.Clear("codex").code ==
        flr::CredentialErrorCode::ExternalCredentialUnowned);
  flr::CredentialError error;
  (void)resolver.BeginBrowserAuthorization("http://127.0.0.1:1455/callback",
                                           error);
  CHECK(error.code == flr::CredentialErrorCode::ExternalCredentialUnowned);
}

TEST_CASE("codex requests inject account identity without implicit OAuth") {
  auto environment = std::make_shared<TestEnvironment>();
  environment->values["CODEX_ACCESS_TOKEN"] = Jwt(2000, "account-xyz");
  auto clock = std::make_shared<TestClock>();
  flr::CredentialDependencies dependencies;
  dependencies.environment = environment;
  dependencies.clock = clock;
  flr::CredentialResolver resolver(std::nullopt, dependencies);
  TestHttp http;
  flr::AppConfig config;
  config.provider = "codex";
  config.api_format = flr::ApiFormat::OpenAiResponses;
  config.model = "gpt-5.1-codex";
  const auto result = flr::RewriteWithLlm(
      config, {.input = "draft"}, http, resolver);
  REQUIRE(result.ok);
  REQUIRE(http.requests.size() == 1);
  const auto& request = http.requests.front();
  CHECK(request.url == "https://chatgpt.com/backend-api/codex/responses");
  CHECK(std::find(request.headers.begin(), request.headers.end(),
                  "ChatGPT-Account-ID: account-xyz") != request.headers.end());
  CHECK(std::find(request.headers.begin(), request.headers.end(),
                  "originator: codex_cli_rs") != request.headers.end());
}

TEST_CASE("browser authorization accepts only loopback callbacks") {
  flr::CredentialResolver resolver;
  flr::CredentialError error;
  const auto rejected =
      resolver.BeginBrowserAuthorization("https://127.0.0.1/callback", error);
  CHECK(error.code == flr::CredentialErrorCode::OAuthRejected);
  CHECK(rejected.authorization_url.empty());

  const auto accepted =
      resolver.BeginBrowserAuthorization("http://127.0.0.1:1455/callback",
                                         error);
  REQUIRE(error.ok());
  CHECK(accepted.authorization_url.find("code_challenge_method=S256") !=
        std::string::npos);
  CHECK(accepted.RedactedDescription() == "BrowserAuthorization(state=redacted)");
}

TEST_CASE("device authorization polls bounded statuses and persists refresh token") {
  auto store = std::make_shared<TestStore>();
  auto clock = std::make_shared<TestClock>();
  auto random = std::make_shared<TestRandom>();
  auto http = std::make_shared<TestHttp>();
  http->responses = {
      {.status = 200,
       .body = R"({"device_auth_id":"device-id","usercode":"USER-CODE","interval":1})",
       .error = {}},
      {.status = 403, .body = "", .error = {}},
      {.status = 200,
       .body = R"({"authorization_code":"auth-code","code_verifier":"verifier"})",
       .error = {}},
      {.status = 200,
       .body = nlohmann::json{{"access_token", Jwt(3000, "account-device")},
                              {"refresh_token", "refresh-device"}}
                   .dump(),
       .error = {}}};
  flr::CredentialDependencies dependencies;
  dependencies.store = store;
  dependencies.clock = clock;
  dependencies.random = random;
  dependencies.http = http.get();
  flr::CredentialResolver resolver(std::nullopt, dependencies);
  flr::CredentialError error;
  const auto authorization =
      resolver.RequestDeviceAuthorization(nullptr, error);
  REQUIRE(error.ok());
  CHECK(authorization.interval_seconds == 5);
  error = resolver.CompleteDeviceAuthorization(authorization, nullptr);
  CHECK(error.ok());
  CHECK(store->values["codex-oauth-refresh-token"] == "refresh-device");
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
      "--model", "provider/model", "--reasoning", "provider-custom"});
  REQUIRE(parsed.options.has_value());
  REQUIRE(parsed.options->model.has_value());
  REQUIRE(parsed.options->reasoning.has_value());

  flr::AppConfig config;
  config.model = "config/model";
  config.reasoning = "none";
  flr::ApplyCliOverrides(config, parsed.options->model,
                         parsed.options->reasoning);
  CHECK(config.model == "provider/model");
  CHECK(config.reasoning == "provider-custom");

  auto removed =
      flr::ParseCli(std::vector<std::string>{"--old-reasoning", "medium"});
  CHECK(removed.exit);
  CHECK_FALSE(removed.options.has_value());
}

TEST_CASE("CLI parses doctor subcommand after root options") {
  auto parsed = flr::ParseCli(std::vector<std::string>{
      "doctor", "--input", "clipboard", "--output", "paste", "--model",
      "provider/model", "--live"});
  REQUIRE(parsed.options.has_value());
  CHECK(parsed.options->command == flr::CliCommand::Doctor);
  CHECK(parsed.options->doctor_live);
  CHECK(parsed.options->input == flr::InputMode::Clipboard);
#if defined(__linux__)
  CHECK(parsed.options->output == flr::OutputMode::Paste);
#endif
  REQUIRE(parsed.options->model.has_value());
  CHECK(*parsed.options->model == "provider/model");
}

TEST_CASE("first run config generation writes JSON defaults") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "llm-rewriter-generated-config-test";
  const auto path = dir / "config.json";
  std::filesystem::remove(path);
  std::filesystem::remove(dir);

  CHECK(flr::EnsureDefaultConfigFile(path));
  CHECK(std::filesystem::exists(path));

  std::ifstream input(path);
  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  const auto json = nlohmann::json::parse(content);
  CHECK(json["provider"]["model"] == "openai/gpt-4.1-mini");
  CHECK(json["generation"]["reasoning"] == "none");
  CHECK(json["workflow"]["input"] == "clipboard");
  CHECK(json["credentials"]["credential"] == "");
  CHECK(json["credentials"]["codex_auth_file"] == "~/.codex/auth.json");
  CHECK(json["custom_provider"]["headers"].is_object());
  CHECK_FALSE(flr::EnsureDefaultConfigFile(path));

  std::filesystem::remove(path);
  std::filesystem::remove(dir);
}

TEST_CASE("config value writer updates JSON config file") {
  const auto dir =
      std::filesystem::temp_directory_path() / "llm-rewriter-set-config-test";
  const auto path = dir / "config.json";
  std::filesystem::remove(path);
  std::filesystem::remove(dir);

  REQUIRE(flr::EnsureDefaultConfigFile(path));
  CHECK(flr::SetConfigValue(path, "model", "provider/updated"));
  CHECK(flr::SetConfigValue(path, "reasoning", "low"));
  CHECK(flr::SetConfigValue(path, "input", "stdin"));
  CHECK(flr::SetConfigValue(path, "system_prompt", "line one\nline two"));
  CHECK(flr::SetConfigValue(path, "credential", "configured-secret"));
  CHECK(flr::SetConfigValue(path, "custom_provider.headers.X-Extra", "1"));
  CHECK(flr::SetConfigValue(path, "custom_provider.query_parameters.debug",
                            "true"));
  CHECK(flr::SetConfigValue(path, "custom_provider.request_body.metadata",
                            R"({"source":"writer-test"})"));
  CHECK_FALSE(flr::SetConfigValue(path, "input", "invalid"));
  CHECK_FALSE(flr::SetConfigValue(path, "api_key", "secret"));
  CHECK(flr::SetConfigValue(path, "codex_auth_file",
                            (dir / "codex-auth.json").string()));
  CHECK_FALSE(flr::SetConfigValue(path, "codex_auth_file", "relative.json"));

  const auto config = flr::LoadConfig(path);
  CHECK(config.model == "provider/updated");
  CHECK(config.reasoning == "low");
  CHECK(config.input_mode == "stdin");
  CHECK(config.system_prompt == "line one\nline two");
  CHECK(config.credential == "configured-secret");
  CHECK(config.custom_provider.headers.at("X-Extra") == "1");
  CHECK(config.custom_provider.query_parameters.at("debug") == "true");
  CHECK(config.custom_provider.request_body["metadata"]["source"] ==
        "writer-test");
  CHECK(config.codex_auth_file == dir / "codex-auth.json");

  std::filesystem::remove(path);
  std::filesystem::remove(dir);
}

TEST_CASE("config writer saves the complete application configuration") {
  const auto dir =
      std::filesystem::temp_directory_path() / "llm-rewriter-save-config-test";
  const auto path = dir / "config.json";
  std::filesystem::remove(path);
  std::filesystem::remove(dir);

  REQUIRE(flr::EnsureDefaultConfigFile(path));
  {
    std::ifstream input(path);
    auto document = nlohmann::json::parse(input);
    document["unknown_setting"] = nlohmann::json{{"preserved", true}};
    document["system_prompt_file"] = "stale-prompt.md";
    std::ofstream output(path);
    output << document.dump(2) << '\n';
  }
  flr::AppConfig config;
  config.provider = "custom";
  config.api_format = flr::ApiFormat::AnthropicMessages;
  config.base_url = "https://example.test/v1";
  config.model = "custom/model";
  config.input_mode = "primary";
  config.output_mode = "stdout";
  config.credential = "configured-secret";
  config.codex_auth_file = dir / "codex-auth.json";
  config.reasoning = "high";
  config.timeout = std::chrono::milliseconds{45000};
  config.history_enabled = false;
  config.min_output_tokens = 512;
  config.max_output_tokens_limit = 8192;
  config.output_token_multiplier = 2.0;
  config.output_token_padding = 256;
  config.notification_mode = flr::NotificationMode::Always;
  config.notification_events = flr::NotificationEvents::Completion;
  config.paste_shortcut = flr::PasteShortcut::ShiftInsert;
  config.custom_provider.headers["X-Test"] = "header";
  config.custom_provider.query_parameters["debug"] = "true";
  config.custom_provider.request_body =
      nlohmann::json{{"source", "test"}};
  config.system_prompt = "Rewrite in active voice.";

  CHECK(flr::SaveConfig(path, config));
  const auto saved = flr::LoadConfig(path);
  CHECK(saved.provider == "custom");
  CHECK(saved.api_format == flr::ApiFormat::AnthropicMessages);
  CHECK(saved.base_url == "https://example.test/v1");
  CHECK(saved.model == "custom/model");
  CHECK(saved.input_mode == "primary");
  CHECK(saved.output_mode == "stdout");
  CHECK(saved.credential == "configured-secret");
  CHECK(saved.codex_auth_file == dir / "codex-auth.json");
  CHECK(saved.reasoning == "high");
  CHECK(saved.timeout == std::chrono::milliseconds{45000});
  CHECK_FALSE(saved.history_enabled);
  CHECK(saved.min_output_tokens == 512);
  CHECK(saved.max_output_tokens_limit == 8192);
  CHECK(saved.output_token_multiplier == 2.0);
  CHECK(saved.output_token_padding == 256);
  CHECK(saved.notification_mode == flr::NotificationMode::Always);
  CHECK(saved.notification_events == flr::NotificationEvents::Completion);
  CHECK(saved.paste_shortcut == flr::PasteShortcut::ShiftInsert);
  CHECK(saved.custom_provider.headers.at("X-Test") == "header");
  CHECK(saved.custom_provider.query_parameters.at("debug") == "true");
  CHECK(saved.custom_provider.request_body["source"] == "test");
  CHECK(saved.system_prompt == "Rewrite in active voice.");

  {
    std::ifstream input(path);
    const auto document = nlohmann::json::parse(input);
    CHECK(document["unknown_setting"]["preserved"] == true);
    CHECK_FALSE(document.contains("system_prompt_file"));
  }
#if !defined(_WIN32)
  struct stat config_metadata {};
  REQUIRE(::stat(path.c_str(), &config_metadata) == 0);
  CHECK((config_metadata.st_mode & 0777) == 0600);
  struct stat directory_metadata {};
  REQUIRE(::stat(dir.c_str(), &directory_metadata) == 0);
  CHECK((directory_metadata.st_mode & 0777) == 0700);
#endif

  std::filesystem::remove(path);
  std::filesystem::remove(dir);
}

TEST_CASE("token estimation clamps with 64k default limit") {
  flr::AppConfig config;
  CHECK(config.max_output_tokens_limit == 65536);
  CHECK(flr::EstimateTokens("12345678") == 2);
  CHECK(flr::ChooseMaxOutputTokens(config, 10) == 256);
  config.reasoning = "high";
  CHECK(flr::ChooseMaxOutputTokens(config, 100) == 2224);
  config.reasoning = "provider-custom";
  CHECK(flr::ChooseMaxOutputTokens(config, 100) == 1112);
  CHECK(flr::ChooseMaxOutputTokens(config, 1000000) == 65536);
}

TEST_CASE("LLM payloads include explicit reasoning") {
  flr::AppConfig config;
  config.provider = "custom";
  config.base_url = "https://example.test/v1";
  config.model = "test-model";
  config.system_prompt = "system";
  config.api_format = flr::ApiFormat::OpenAiChat;
  config.custom_provider.request_body = {
      {"metadata", {{"source", "test-suite"}}},
      {"temperature", 0.1},
  };

  auto json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["model"] == "test-model");
  CHECK(json["messages"][0]["content"] == "system");
  CHECK(json["reasoning_effort"] == "none");
  CHECK(json["metadata"]["source"] == "test-suite");
  CHECK(json["temperature"] == 0.1);

  config.reasoning = "low";
  json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["reasoning_effort"] == "low");

  config.api_format = flr::ApiFormat::OpenAiResponses;
  json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["reasoning"]["effort"] == "low");

  config.api_format = flr::ApiFormat::AnthropicMessages;
  config.reasoning = "none";
  json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["system"] == "system");
  CHECK(json["messages"][0]["content"] == "hello");
  CHECK_FALSE(json.contains("thinking"));
  CHECK_FALSE(json.contains("output_config"));

  config.reasoning = "high";
  json = nlohmann::json::parse(
      flr::BuildLlmPayloadForTest(config, {.input = "hello"}, 512));
  CHECK(json["thinking"]["type"] == "adaptive");
  CHECK(json["output_config"]["effort"] == "high");
}

TEST_CASE("Codex uses streaming Responses payloads and parses SSE output") {
  flr::AppConfig config;
  config.provider = "codex";
  config.model = "gpt-test";
  config.system_prompt = "system";
  config.credential = "codex-token";
  config.reasoning = "low";

  TestHttp http;
  http.responses.push_back(
      {.status = 200,
       .body =
           "event: response.output_text.delta\n"
           "data: {\"type\":\"response.output_text.delta\",\"delta\":\"first\"}\n\n"
           "event: response.output_text.delta\n"
           "data: {\"type\":\"response.output_text.delta\",\"delta\":\" second\"}\n\n"
           "event: response.completed\n"
           "data: {\"type\":\"response.completed\"}\n\n",
       .error = {},
       .headers = {{"x-request-id", "provider-123"}}});
  TestDiagnosticSink diagnostics;

  const auto result = flr::RewriteWithLlm(
      config, {.input = "draft"}, http, &diagnostics);
  REQUIRE(result.ok);
  CHECK(result.text == "first second");
  REQUIRE(http.requests.size() == 1);
  const auto payload = nlohmann::json::parse(http.requests.front().body);
  CHECK(payload["stream"] == true);
  REQUIRE(payload["input"].is_array());
  REQUIRE(payload["input"].size() == 1);
  CHECK(payload["input"][0]["type"] == "message");
  CHECK(payload["input"][0]["role"] == "user");
  CHECK(payload["input"][0]["content"][0]["type"] == "input_text");
  CHECK(payload["input"][0]["content"][0]["text"] == "draft");
  CHECK_FALSE(payload.contains("temperature"));
  CHECK_FALSE(payload.contains("max_output_tokens"));
  CHECK(std::find(http.requests.front().headers.begin(),
                  http.requests.front().headers.end(),
                  "Accept: text/event-stream") !=
        http.requests.front().headers.end());
  CHECK_FALSE(result.request_id.empty());
  CHECK(result.provider_request_id == "provider-123");
  REQUIRE(diagnostics.events.size() == 1);
  CHECK(diagnostics.events.front().outcome == "success");
  CHECK(diagnostics.events.front().request_id == result.request_id);
}

TEST_CASE("provider error details are surfaced without retaining response bodies") {
  flr::AppConfig config;
  config.provider = "openai";
  config.model = "gpt-test";
  config.credential = "secret";
  TestHttp http;
  http.responses.push_back(
      {.status = 400,
       .body = R"({"error":{"message":"Unsupported parameter: temperature","type":"invalid_request_error","code":"unsupported_parameter"}})",
       .error = {}});

  const auto result = flr::RewriteWithLlm(
      config, {.input = "draft"}, http);
  CHECK_FALSE(result.ok);
  CHECK(result.error ==
        "HTTP 400: Unsupported parameter: temperature (unsupported_parameter)");
  CHECK(result.error.find("secret") == std::string::npos);
  CHECK(result.http_status == 400);
}

TEST_CASE("diagnostic JSONL contains metadata but no endpoint credentials") {
  const auto path = std::filesystem::temp_directory_path() /
                    "llm-rewriter-diagnostics-test" / "diagnostics.jsonl";
  std::filesystem::remove_all(path.parent_path());
  flr::JsonlDiagnosticSink sink(path);
  sink.Record({.request_id = "rw-test",
               .provider = "custom",
               .api_format = "openai_chat",
               .model = "local",
               .endpoint = "https://user:password@example.test/v1?token=secret",
               .provider_request_id = "provider-123",
               .outcome = "failure",
               .error = "HTTP 400",
               .error_code = "bad_request",
               .http_status = 400,
               .duration = std::chrono::milliseconds{12}});

  std::ifstream input(path);
  std::ostringstream content;
  content << input.rdbuf();
  CHECK(content.str().find("password") == std::string::npos);
  CHECK(content.str().find("token=secret") == std::string::npos);
  CHECK(content.str().find("https://example.test/v1") != std::string::npos);
  CHECK(content.str().find("rw-test") != std::string::npos);
  std::filesystem::remove_all(path.parent_path());
}

TEST_CASE("custom provider supports keyless local inference and optional auth") {
  flr::AppConfig config;
  config.provider = "custom";
  config.api_format = flr::ApiFormat::OpenAiChat;
  config.base_url = "http://127.0.0.1:11434/v1";
  config.model = "local-model";
  config.system_prompt = "system";
  config.custom_provider.query_parameters["keep_alive"] = "5m";
  config.custom_provider.headers["X-Local"] = "yes";

  auto environment = std::make_shared<TestEnvironment>();
  auto store = std::make_shared<TestStore>();
  flr::CredentialDependencies dependencies;
  dependencies.environment = environment;
  dependencies.store = store;
  flr::CredentialResolver resolver(std::nullopt, dependencies);
  TestHttp http;
  http.responses.push_back(
      {.status = 200,
       .body = R"({"choices":[{"message":{"content":"local result"}}]})",
       .error = {}});
  const auto keyless = flr::RewriteWithLlm(
      config, {.input = "draft"}, http, resolver);
  REQUIRE(keyless.ok);
  CHECK(keyless.text == "local result");
  REQUIRE(http.requests.size() == 1);
  CHECK(http.requests.front().url ==
        "http://127.0.0.1:11434/v1/chat/completions?keep_alive=5m");
  CHECK(std::find(http.requests.front().headers.begin(),
                  http.requests.front().headers.end(),
                  "X-Local: yes") != http.requests.front().headers.end());
  CHECK(std::find_if(http.requests.front().headers.begin(),
                     http.requests.front().headers.end(),
                     [](const std::string &header) {
                       return header.rfind("Authorization:", 0) == 0;
                     }) == http.requests.front().headers.end());

  config.credential = "local-secret";
  http.responses.push_back(
      {.status = 200,
       .body = R"({"choices":[{"message":{"content":"authenticated"}}]})",
       .error = {}});
  const auto authenticated = flr::RewriteWithLlm(
      config, {.input = "draft"}, http, resolver);
  REQUIRE(authenticated.ok);
  CHECK(std::find(http.requests.back().headers.begin(),
                  http.requests.back().headers.end(),
                  "Authorization: Bearer local-secret") !=
        http.requests.back().headers.end());
}

TEST_CASE("built-in providers use fixed endpoints and formats") {
  flr::AppConfig config;
  config.provider = "openai";
  config.base_url = "https://should-be-ignored.example";
  config.model = "gpt-test";
  auto environment = std::make_shared<TestEnvironment>();
  environment->values["OPENAI_API_KEY"] = "openai-secret";
  flr::CredentialDependencies dependencies;
  dependencies.environment = environment;
  flr::CredentialResolver resolver(std::nullopt, dependencies);
  TestHttp http;
  http.responses.push_back(
      {.status = 200,
       .body = R"({"choices":[{"message":{"content":"ok"}}]})",
       .error = {}});
  const auto result = flr::RewriteWithLlm(
      config, {.input = "draft"}, http, resolver);
  REQUIRE(result.ok);
  REQUIRE(http.requests.size() == 1);
  CHECK(http.requests.front().url == "https://api.openai.com/v1/chat/completions");
}

TEST_CASE("C ABI exposes opaque rewrite result lifecycle") {
  CHECK(llmr_abi_version() == LLM_REWRITER_ABI_VERSION);
  llmr_context* context = llmr_context_create(nullptr);
  REQUIRE(context != nullptr);
  CHECK(llmr_context_set_model(context, "provider/model") == 0);
  CHECK(llmr_context_set_reasoning(context, "none") == 0);

  llmr_rewrite_result* result = llmr_rewrite(context, "");
  REQUIRE(result != nullptr);
  CHECK(llmr_result_ok(result) == 0);
  CHECK(std::string{llmr_result_error(result)} == "input is empty");
  CHECK(llmr_result_estimated_input_tokens(result) == 1);

  llmr_result_destroy(result);

  CHECK(llmr_context_set_http_transport(context, PostJsonForAbiTest, nullptr) ==
        0);
  result = llmr_rewrite(context, "draft");
  REQUIRE(result != nullptr);
  CHECK(llmr_result_ok(result) == 1);
  CHECK(std::string{llmr_result_text(result)} == "rewritten");
  llmr_result_destroy(result);
  llmr_context_destroy(context);
}

TEST_CASE("doctor report validates configured platform workflow") {
  flr::AppConfig config;
  config.model = "provider/model";
  flr::CliOptions options;
  options.command = flr::CliCommand::Doctor;
  options.input = flr::InputMode::Clipboard;
#if defined(__linux__)
  options.output = flr::OutputMode::Paste;
#else
  options.output = flr::OutputMode::Clipboard;
#endif

  const auto base = std::filesystem::temp_directory_path() /
                    "llm-rewriter-doctor-test";
  flr::UserPaths paths{.config_dir = base,
                       .data_dir = base,
                       .config_file = base / "config.json",
                       .history_file = base / "history.jsonl"};

  flr::RuntimeStatus status;
  status.clipboard_read_available = true;
  status.clipboard_write_available = true;
  status.primary_selection_available = true;
  status.synthetic_output_available = true;
  status.notifications_available = true;
  status.details = {{"wayland", true}, {"wtype", true}};
  status.config_parent_writable = true;
  status.config_readable = true;
  status.history_parent_writable = true;

  auto report = flr::BuildDoctorReport(config, options, paths, status);
  CHECK(report.ok);
  CHECK(report.text.find("result: ready") != std::string::npos);

#if defined(__linux__)
  status.synthetic_output_available = false;
  status.details = {{"ydotool", true}, {"ydotoold reachable", false}};
  report = flr::BuildDoctorReport(config, options, paths, status);
  CHECK_FALSE(report.ok);
  CHECK(report.text.find("ydotool+ydotoold") != std::string::npos);
#endif

  status.config_secure_permissions = false;
  report = flr::BuildDoctorReport(config, options, paths, status);
  CHECK_FALSE(report.ok);
  CHECK(report.text.find("config.json is not private") != std::string::npos);
}

TEST_CASE("history records explicit reasoning value") {
  const auto dir =
      std::filesystem::temp_directory_path() / "llm-rewriter-history-test";
  const auto path = dir / "history.jsonl";
  std::filesystem::remove(path);
  std::filesystem::remove(dir);

  flr::AppConfig config;
  config.model = "test-model";
  config.reasoning = "high";
  flr::RewriteResult result;
  result.ok = true;
  result.text = "rewritten";
  result.estimated_input_tokens = 3;
  result.max_output_tokens = 128;

  flr::AppendHistory(path, config, "input", result);
  std::ifstream input(path);
  std::string line;
  std::getline(input, line);
  const auto json = nlohmann::json::parse(line);
  CHECK(json["reasoning"] == "high");
#if !defined(_WIN32)
  struct stat history_metadata {};
  REQUIRE(::stat(path.c_str(), &history_metadata) == 0);
  CHECK((history_metadata.st_mode & 0777) == 0600);
  struct stat history_directory_metadata {};
  REQUIRE(::stat(dir.c_str(), &history_directory_metadata) == 0);
  CHECK((history_directory_metadata.st_mode & 0777) == 0700);
#endif

  std::filesystem::remove(path);
  std::filesystem::remove(dir);
}

TEST_CASE("history loader tolerates malformed lines and searches newest first") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "llm-rewriter-history-loader-test";
  const auto path = dir / "history.jsonl";
  std::filesystem::remove(path);
  std::filesystem::remove(dir);
  std::filesystem::create_directories(dir);
  {
    std::ofstream output(path);
    output << nlohmann::json{{"timestamp_ms", 100},
                             {"provider", "openai"},
                             {"model", "older"},
                             {"ok", true},
                             {"request_id", "old"},
                             {"duration_ms", 12},
                             {"input", "A draft"},
                             {"output", "An answer"}}
                    .dump()
            << '\n';
    output << "not-json\n";
    output << nlohmann::json{{"timestamp_ms", 200},
                             {"provider", "anthropic"},
                             {"model", "newer"},
                             {"ok", false},
                             {"request_id", "new"},
                             {"http_status", 500},
                             {"error", "rate limited"},
                             {"input", "Second draft"},
                             {"output", ""}}
                    .dump()
            << '\n';
  }

  const auto entries = flr::LoadHistory(path);
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].timestamp_ms == 200);
  CHECK(entries[0].request_id == "new");
  CHECK(entries[0].http_status == 500);
  CHECK(entries[0].duration == std::chrono::milliseconds{0});
  CHECK_FALSE(entries[0].ok);
  CHECK(entries[1].duration == std::chrono::milliseconds{12});
  CHECK(entries[1].Identity() == "old:100");

  const auto matches = flr::SearchHistory(entries, "ANTHROPIC");
  REQUIRE(matches.size() == 1);
  CHECK(matches.front().request_id == "new");
  const auto ranked = flr::HistorySearchIndex(entries).Search("draft answer");
  REQUIRE(ranked.size() == 2);
  CHECK(ranked.front().request_id == "old");
  const auto page = flr::HistorySearchIndex(entries).SearchPage("draft", 0, 1);
  CHECK(page.total_matches == 2);
  REQUIRE(page.entries.size() == 1);
  CHECK(page.entries.front().request_id == "new");
  CHECK(flr::LoadHistory(dir / "missing.jsonl").empty());

  std::filesystem::remove(path);
  std::filesystem::remove(dir);
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
