#pragma once

#include "llm_rewriter/HttpTransport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace llm_rewriter {

// A small cancellation seam shared by OAuth and provider requests.  The
// caller owns the token and may cancel it from another thread.
class CancellationToken {
 public:
  void Cancel() noexcept;
  bool IsCancelled() const noexcept;

 private:
  std::atomic_bool cancelled_{false};
};

enum class CredentialProvider {
  OpenAi,
  Anthropic,
  Gemini,
  OpenRouter,
  Codex,
  Custom,
};

std::optional<CredentialProvider> ParseCredentialProvider(std::string_view value);
std::string ToString(CredentialProvider provider);

struct ResolvedCredential {
  std::string access_token;
  std::optional<std::string> account_id;

  // Deliberately does not include either secret in the returned text.
  std::string RedactedDescription() const;
};

enum class CredentialSource {
  Environment,
  Configuration,
  ExternalFile,
  NativeStore,
  Unconfigured,
  Keyless,
};

enum class CredentialErrorCode {
  None,
  Cancelled,
  MissingCredential,
  InvalidCredential,
  UnsupportedProvider,
  NativeStoreUnavailable,
  ExternalPathRejected,
  ExternalFileUnavailable,
  ExternalFileRejected,
  ExternalCredentialUnowned,
  OAuthTransportFailed,
  OAuthRejected,
  OAuthTimedOut,
  InvalidOAuthResponse,
};

struct CredentialError {
  CredentialErrorCode code = CredentialErrorCode::None;

  bool ok() const noexcept { return code == CredentialErrorCode::None; }
  explicit operator bool() const noexcept { return !ok(); }
  std::string Message() const;
};

struct CredentialResolution {
  std::optional<ResolvedCredential> credential;
  CredentialSource source = CredentialSource::Unconfigured;
  CredentialError error;

  bool ok() const noexcept { return error.ok(); }
};

struct CredentialStatus {
  CredentialSource source = CredentialSource::Unconfigured;
  CredentialError error;

  bool ok() const noexcept { return error.ok(); }
};

struct BrowserAuthorization {
  std::string authorization_url;
  std::string redirect_uri;
  std::string expected_state;

  // The verifier is intentionally not returned by value to callers.  It is
  // retained by the resolver and consumed by CompleteBrowserAuthorization.
  std::string RedactedDescription() const;
};

struct DeviceAuthorization {
  std::string verification_url;
  std::string user_code;
  std::uint64_t interval_seconds = 5;

  std::string RedactedDescription() const;
};

class ICredentialStore {
 public:
  virtual ~ICredentialStore() = default;
  virtual std::optional<std::string> Get(std::string_view account) = 0;
  virtual bool Set(std::string_view account, std::string_view secret) = 0;
  virtual bool Remove(std::string_view account) = 0;
  virtual CredentialError LastError() const = 0;
};

class ICredentialEnvironment {
 public:
  virtual ~ICredentialEnvironment() = default;

  // An engaged optional means the variable is defined, including when it is
  // defined to an empty string.  That distinction prevents invalid values from
  // falling through to a lower-precedence source.
  virtual std::optional<std::string> Get(std::string_view name) const = 0;
};

class ICredentialClock {
 public:
  virtual ~ICredentialClock() = default;
  virtual std::uint64_t NowEpochSeconds() const = 0;
  virtual void SleepFor(std::chrono::milliseconds duration) const = 0;
};

class ICredentialRandom {
 public:
  virtual ~ICredentialRandom() = default;
  virtual std::string UrlSafe(std::size_t byte_count) = 0;
};

class ICredentialFile {
 public:
  virtual ~ICredentialFile() = default;
  virtual std::optional<std::string> Read(const std::filesystem::path& path,
                                          CredentialError& error) const = 0;
  virtual bool Rewrite(const std::filesystem::path& path,
                       std::string_view contents,
                       CredentialError& error) const = 0;
};

struct CredentialDependencies {
  std::shared_ptr<ICredentialStore> store;
  std::shared_ptr<ICredentialEnvironment> environment;
  std::shared_ptr<ICredentialClock> clock;
  std::shared_ptr<ICredentialRandom> random;
  std::shared_ptr<ICredentialFile> file;
  IHttpTransport* http = nullptr;
};

// Platform targets provide an adapter backed by Secret Service on Linux or
// Windows Credential Manager.  It never falls back to config-file storage.
std::shared_ptr<ICredentialStore> CreatePlatformCredentialStore();

class CredentialResolver {
 public:
  // The resolver performs no I/O in its constructor.  A relative external
  // Codex auth path is remembered as a bounded construction error and rejected
  // by operations that would use it.
  explicit CredentialResolver(
      std::optional<std::filesystem::path> external_codex_auth = std::nullopt,
      CredentialDependencies dependencies = {});

  CredentialError ConstructionError() const;

  CredentialResolution Resolve(
      std::string_view provider,
      std::string_view configured_credential = {},
      const CancellationToken* cancellation = nullptr);

  CredentialStatus Status(std::string_view provider,
                          std::string_view configured_credential = {}) const;

  CredentialError ConfigureApiKey(std::string_view provider,
                                  std::string_view api_key);
  CredentialError Clear(std::string_view provider);

  BrowserAuthorization BeginBrowserAuthorization(std::string_view redirect_uri,
                                                  CredentialError& error);
  CredentialError CompleteBrowserAuthorization(
      const BrowserAuthorization& authorization,
      std::string_view returned_state,
      std::string_view authorization_code,
      const CancellationToken* cancellation = nullptr);

  DeviceAuthorization RequestDeviceAuthorization(
      const CancellationToken* cancellation,
      CredentialError& error);
  CredentialError CompleteDeviceAuthorization(
      const DeviceAuthorization& authorization,
      const CancellationToken* cancellation = nullptr);

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace llm_rewriter
