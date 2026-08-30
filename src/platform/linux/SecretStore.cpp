#include "llm_rewriter/Credentials.hpp"

#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>

namespace llm_rewriter {
namespace {

bool SafeAccount(std::string_view account) {
  return !account.empty() &&
         std::all_of(account.begin(), account.end(), [](unsigned char value) {
           return std::isalnum(value) || value == '-' || value == '_';
         });
}

class SecretToolStore final : public ICredentialStore {
 public:
  std::optional<std::string> Get(std::string_view account) override {
    if (!SafeAccount(account)) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return std::nullopt;
    }
    const std::string command =
        "secret-tool lookup service llm-rewriter account " +
        std::string{account} + " 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> process(popen(command.c_str(), "r"),
                                                     pclose);
    if (!process) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return std::nullopt;
    }
    std::string value;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()),
                      process.get()) != nullptr) {
      value += buffer.data();
      if (value.size() > 64 * 1024) {
        last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
        return std::nullopt;
      }
    }
    const int status = pclose(process.release());
    if (status == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return std::nullopt;
    }
    // secret-tool uses a non-zero status for a missing item.  Treat that as
    // an ordinary miss; unavailable command/process failures are 126/127.
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
      value.pop_back();
    }
    last_error_ = {};
    return value.empty() ? std::nullopt : std::optional{std::move(value)};
  }

  bool Set(std::string_view account, std::string_view secret) override {
    if (!SafeAccount(account) || secret.empty() || secret.size() > 64 * 1024) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return false;
    }
    const std::string command =
        "secret-tool store --label=llm-rewriter service llm-rewriter account " +
        std::string{account} + " 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> process(popen(command.c_str(), "w"),
                                                     pclose);
    if (!process ||
        std::fwrite(secret.data(), 1, secret.size(), process.get()) !=
            secret.size()) {
      if (process) pclose(process.release());
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return false;
    }
    const int status = pclose(process.release());
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return false;
    }
    last_error_ = {};
    return true;
  }

  bool Remove(std::string_view account) override {
    if (!SafeAccount(account)) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return false;
    }
    const std::string command =
        "secret-tool clear service llm-rewriter account " +
        std::string{account} + " 2>/dev/null";
    const int status = std::system(command.c_str());
    if (status == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return false;
    }
    // A missing entry is equivalent to a successful clear.
    last_error_ = {};
    return true;
  }

  CredentialError LastError() const override { return last_error_; }

 private:
  CredentialError last_error_;
};

}  // namespace

std::shared_ptr<ICredentialStore> CreatePlatformCredentialStore() {
  return std::make_shared<SecretToolStore>();
}

}  // namespace llm_rewriter
