#include "llm_rewriter/Cli.hpp"
#include "llm_rewriter/Clipboard.hpp"
#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/Credentials.hpp"
#include "llm_rewriter/Doctor.hpp"
#include "llm_rewriter/Output.hpp"
#include "llm_rewriter/Paths.hpp"
#include "llm_rewriter/RewriteService.hpp"

#include <iostream>
#include <sstream>
#include <string>

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

std::string ReadSecretFromStdin() {
  std::string value;
  std::getline(std::cin, value, '\0');
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

const char* CredentialSourceName(llm_rewriter::CredentialSource source) {
  switch (source) {
    case llm_rewriter::CredentialSource::Environment:
      return "environment";
    case llm_rewriter::CredentialSource::Configuration:
      return "configuration";
    case llm_rewriter::CredentialSource::ExternalFile:
      return "external-file";
    case llm_rewriter::CredentialSource::NativeStore:
      return "native-store";
    case llm_rewriter::CredentialSource::Unconfigured:
      return "unconfigured";
    case llm_rewriter::CredentialSource::Keyless:
      return "keyless";
  }
  return "unknown";
}

int RunProviderCommand(const llm_rewriter::CliOptions& options,
                       const llm_rewriter::AppConfig& config) {
  auto transport = llm_rewriter::CreatePlatformHttpTransport();
  llm_rewriter::CredentialDependencies dependencies;
  dependencies.store = llm_rewriter::CreatePlatformCredentialStore();
  dependencies.http = transport.get();
  llm_rewriter::CredentialResolver resolver(config.codex_auth_file,
                                            dependencies);
  if (options.provider_command ==
      llm_rewriter::ProviderCredentialCommand::Status) {
    const auto status = resolver.Status(
        options.credential_provider,
        options.credential_provider == config.provider ? config.credential
                                                        : std::string{});
    if (!status.ok()) {
      std::cerr << "credential status failed: " << status.error.Message()
                << '\n';
      return 1;
    }
    std::cout << options.credential_provider << ": "
              << CredentialSourceName(status.source) << '\n';
    return 0;
  }
  if (options.provider_command ==
      llm_rewriter::ProviderCredentialCommand::Clear) {
    const auto error = resolver.Clear(options.credential_provider);
    if (error) {
      std::cerr << "credential clear failed: " << error.Message() << '\n';
      return 1;
    }
    return 0;
  }

  if (options.credential_provider == "codex") {
    if (!options.device_code) {
      std::cerr
          << "Codex browser authorization requires a frontend-owned loopback "
             "callback; use --device-code in the CLI.\n";
      return 1;
    }
    llm_rewriter::CredentialError error;
    const auto authorization =
        resolver.RequestDeviceAuthorization(nullptr, error);
    if (error) {
      std::cerr << "device authorization failed: " << error.Message() << '\n';
      return 1;
    }
    std::cout << "Open " << authorization.verification_url << " and enter "
              << authorization.user_code << "\n";
    error = resolver.CompleteDeviceAuthorization(authorization, nullptr);
    if (error) {
      std::cerr << "device authorization failed: " << error.Message() << '\n';
      return 1;
    }
    return 0;
  }

  const auto secret = ReadSecretFromStdin();
  const auto error =
      resolver.ConfigureApiKey(options.credential_provider, secret);
  if (error) {
    std::cerr << "credential configure failed: " << error.Message() << '\n';
    return 1;
  }
  return 0;
}

int RunHeadless(const llm_rewriter::CliOptions& options,
                const llm_rewriter::UserPaths& paths,
                const llm_rewriter::AppConfig& config) {
  if (options.output == llm_rewriter::OutputMode::Preview) {
    std::cerr << "preview output requires llm-rewriter-gtk4\n";
    return 1;
  }

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
  if (!parsed.options) {
    return 1;
  }

  auto options = *parsed.options;
  auto paths = llm_rewriter::ResolveUserPaths();
  if (!options.config_path.empty()) {
    paths.config_file = llm_rewriter::ExpandUserPath(options.config_path);
  }
  llm_rewriter::EnsureDefaultConfigFile(paths.config_file);
  auto config = llm_rewriter::LoadConfig(paths.config_file);
  llm_rewriter::ApplyCliOverrides(config, options.model, options.reasoning);
  llm_rewriter::ApplyConfigDefaults(options, config);

  if (options.command == llm_rewriter::CliCommand::Doctor) {
    const auto report =
        llm_rewriter::RunDoctor(config, options, paths, options.doctor_live);
    std::cout << report.text;
    return report.ok ? 0 : 1;
  }
  if (options.command == llm_rewriter::CliCommand::Providers) {
    return RunProviderCommand(options, config);
  }

  return RunHeadless(options, paths, config);
}
