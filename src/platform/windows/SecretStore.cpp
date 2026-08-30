#include "llm_rewriter/Credentials.hpp"

#include <windows.h>
#include <wincred.h>

#include <algorithm>
#include <string>

namespace llm_rewriter {
namespace {

std::wstring Wide(std::string_view value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      output.data(), size);
  return output;
}

std::wstring Target(std::string_view account) {
  return L"llm-rewriter/" + Wide(account);
}

class WindowsCredentialStore final : public ICredentialStore {
 public:
  std::optional<std::string> Get(std::string_view account) override {
    PCREDENTIALW raw = nullptr;
    const auto target = Target(account);
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
      if (GetLastError() == ERROR_NOT_FOUND) {
        last_error_ = {};
      } else {
        last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      }
      return std::nullopt;
    }
    std::string value;
    if (raw->CredentialBlob != nullptr && raw->CredentialBlobSize != 0) {
      value.assign(static_cast<const char*>(raw->CredentialBlob),
                   static_cast<std::size_t>(raw->CredentialBlobSize));
    }
    CredFree(raw);
    if (value.empty() || value.size() > 64 * 1024) {
      last_error_ = {.code = CredentialErrorCode::InvalidCredential};
      return std::nullopt;
    }
    last_error_ = {};
    return value;
  }

  bool Set(std::string_view account, std::string_view secret) override {
    if (account.empty() || secret.empty() || secret.size() > 64 * 1024) {
      last_error_ = {.code = CredentialErrorCode::InvalidCredential};
      return false;
    }
    auto target = Target(account);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob =
        reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
    credential.UserName = nullptr;
    if (!CredWriteW(&credential, 0)) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return false;
    }
    last_error_ = {};
    return true;
  }

  bool Remove(std::string_view account) override {
    const auto target = Target(account);
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) &&
        GetLastError() != ERROR_NOT_FOUND) {
      last_error_ = {.code = CredentialErrorCode::NativeStoreUnavailable};
      return false;
    }
    last_error_ = {};
    return true;
  }

  CredentialError LastError() const override { return last_error_; }

 private:
  CredentialError last_error_;
};

}  // namespace

std::shared_ptr<ICredentialStore> CreatePlatformCredentialStore() {
  return std::make_shared<WindowsCredentialStore>();
}

}  // namespace llm_rewriter
