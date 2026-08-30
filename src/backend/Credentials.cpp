#include "llm_rewriter/Credentials.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace llm_rewriter {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaxCredentialBytes = 64 * 1024;
constexpr std::size_t kMaxOAuthResponseBytes = 64 * 1024;
constexpr std::uint64_t kFreshnessMarginSeconds = 60;
constexpr std::uint64_t kDeviceTimeoutSeconds = 15 * 60;
constexpr std::uint64_t kDefaultDevicePollSeconds = 5;
constexpr std::uint64_t kMaxDevicePollSeconds = 60;
constexpr char kOAuthIssuer[] = "https://auth.openai.com";
constexpr char kOAuthClientId[] = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr char kOAuthScopes[] = "openid profile email offline_access";
constexpr char kCodexStoreAccount[] = "codex-oauth-refresh-token";

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::filesystem::path HomeDirectory() {
#if defined(_WIN32)
  if (const char* home = std::getenv("USERPROFILE"); home && *home) {
    return home;
  }
  if (const char* drive = std::getenv("HOMEDRIVE"); drive && *drive) {
    if (const char* path = std::getenv("HOMEPATH"); path && *path) {
      return std::filesystem::path{drive} / path;
    }
  }
#else
  if (const char* home = std::getenv("HOME"); home && *home) {
    return home;
  }
#endif
  return {};
}

std::filesystem::path ExpandUserPath(const std::filesystem::path& path) {
  const auto text = path.string();
  if (text == "~") {
    return HomeDirectory();
  }
  if (text.rfind("~/", 0) == 0) {
    const auto home = HomeDirectory();
    if (!home.empty()) {
      return home / text.substr(2);
    }
  }
  return path;
}

bool ValidSecret(std::string_view value) {
  return !value.empty() && value.size() <= kMaxCredentialBytes &&
         std::ranges::none_of(value, [](unsigned char character) {
           return character <= 0x1f || character == 0x7f;
         });
}

bool ValidIdentifier(std::string_view value) {
  return !value.empty() && value.size() <= 512 &&
         std::ranges::none_of(value, [](unsigned char character) {
           return character <= 0x1f || character == 0x7f;
         });
}

std::vector<std::uint8_t> DecodeBase64Url(std::string_view input) {
  std::vector<std::uint8_t> output;
  output.reserve(input.size() * 3 / 4);
  std::uint32_t buffer = 0;
  int bits = 0;
  std::size_t data_size = input.size();
  std::size_t padding = 0;
  if (const auto equals = input.find('='); equals != std::string_view::npos) {
    data_size = equals;
    padding = input.size() - equals;
    if (padding > 2 || data_size == 0 || input.substr(equals).find_first_not_of('=') !=
                                  std::string_view::npos ||
        input.size() % 4 != 0 ||
        (padding == 1 && data_size % 4 != 3) ||
        (padding == 2 && data_size % 4 != 2)) {
      return {};
    }
  } else if (data_size % 4 == 1) {
    return {};
  }
  for (const unsigned char character : input.substr(0, data_size)) {
    int value = -1;
    if (character >= 'A' && character <= 'Z') value = character - 'A';
    else if (character >= 'a' && character <= 'z') value = character - 'a' + 26;
    else if (character >= '0' && character <= '9') value = character - '0' + 52;
    else if (character == '-') value = 62;
    else if (character == '_') value = 63;
    else return {};
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xffU));
    }
  }
  if (bits != 0 && (buffer & ((1U << bits) - 1U)) != 0) return {};
  return output;
}

std::optional<std::uint64_t> JsonEpoch(const Json& value) {
  if (value.is_number_unsigned()) {
    const auto result = value.get<std::uint64_t>();
    return result == 0 ? std::nullopt : std::optional{result};
  }
  if (value.is_number_integer()) {
    const auto result = value.get<std::int64_t>();
    return result > 0 ? std::optional{static_cast<std::uint64_t>(result)}
                      : std::nullopt;
  }
  if (value.is_number_float()) {
    const auto result = value.get<double>();
    if (!std::isfinite(result) || result <= 0 ||
        result >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(result);
  }
  return std::nullopt;
}

struct JwtMetadata {
  std::uint64_t expires_at = 0;
  std::optional<std::string> account_id;
};

std::optional<JwtMetadata> ReadJwtMetadata(std::string_view token) {
  std::array<std::string_view, 3> segments{};
  std::size_t start = 0;
  for (std::size_t index = 0; index < segments.size(); ++index) {
    const auto end = token.find('.', start);
    if (end == std::string_view::npos) {
      if (index != segments.size() - 1) return std::nullopt;
      segments[index] = token.substr(start);
      start = token.size();
      break;
    }
    if (index == segments.size() - 1) return std::nullopt;
    segments[index] = token.substr(start, end - start);
    start = end + 1;
  }
  if (start != token.size() || segments[0].empty() || segments[1].empty() ||
      segments[2].empty()) {
    return std::nullopt;
  }
  const auto bytes = DecodeBase64Url(segments[1]);
  if (bytes.empty()) return std::nullopt;
  Json claims;
  try {
    claims = Json::parse(bytes.begin(), bytes.end());
  } catch (...) {
    return std::nullopt;
  }
  if (!claims.is_object()) return std::nullopt;
  const auto expires = claims.find("exp");
  if (expires == claims.end()) return std::nullopt;
  const auto expires_at = JsonEpoch(*expires);
  if (!expires_at) return std::nullopt;

  std::optional<std::string> account_id;
  auto take_account = [&account_id](const Json& value) {
    if (!account_id && value.is_string() &&
        ValidIdentifier(value.get_ref<const std::string&>())) {
      account_id = value.get<std::string>();
    }
  };
  if (const auto it = claims.find("chatgpt_account_id"); it != claims.end()) {
    take_account(*it);
  }
  if (const auto it = claims.find("https://api.openai.com/auth");
      it != claims.end() && it->is_object()) {
    if (const auto nested = it->find("chatgpt_account_id");
        nested != it->end()) {
      take_account(*nested);
    }
  }
  return JwtMetadata{.expires_at = *expires_at, .account_id = std::move(account_id)};
}

std::string UrlEncode(std::string_view value) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string output;
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '.' ||
        character == '_' || character == '~') {
      output.push_back(static_cast<char>(character));
    } else {
      output.push_back('%');
      output.push_back(digits[character >> 4]);
      output.push_back(digits[character & 0x0f]);
    }
  }
  return output;
}

bool ConstantTimeEqual(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  unsigned char difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= static_cast<unsigned char>(left[index] ^ right[index]);
  }
  return difference == 0;
}

std::string ToBase64Url(std::string_view input) {
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
  if (bits > 0) output.push_back(alphabet[(buffer << (6 - bits)) & 0x3fU]);
  return output;
}

std::string Sha256(std::string_view input) {
  constexpr std::array<std::uint32_t, 64> constants = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
  auto rotate_right = [](std::uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32U - amount));
  };
  std::vector<std::uint8_t> message(input.begin(), input.end());
  const auto bit_length = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(0x80U);
  while ((message.size() % 64U) != 56U) message.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
  }
  std::array<std::uint32_t, 8> hash = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t block = 0; block < message.size(); block += 64) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
      const auto offset = block + index * 4;
      schedule[index] = (static_cast<std::uint32_t>(message[offset]) << 24U) |
                        (static_cast<std::uint32_t>(message[offset + 1]) << 16U) |
                        (static_cast<std::uint32_t>(message[offset + 2]) << 8U) |
                        static_cast<std::uint32_t>(message[offset + 3]);
    }
    for (std::size_t index = 16; index < schedule.size(); ++index) {
      const auto s0 = rotate_right(schedule[index - 15], 7) ^
                      rotate_right(schedule[index - 15], 18) ^
                      (schedule[index - 15] >> 3U);
      const auto s1 = rotate_right(schedule[index - 2], 17) ^
                      rotate_right(schedule[index - 2], 19) ^
                      (schedule[index - 2] >> 10U);
      schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }
    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < schedule.size(); ++index) {
      const auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                        rotate_right(e, 25);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temp1 = h + sum1 + choose + constants[index] + schedule[index];
      const auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                        rotate_right(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }
  std::string digest;
  digest.reserve(32);
  for (const auto value : hash) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      digest.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
  }
  return digest;
}

class ProcessEnvironment final : public ICredentialEnvironment {
 public:
  std::optional<std::string> Get(std::string_view name) const override {
    const std::string key{name};
    if (const char* value = std::getenv(key.c_str())) return std::string{value};
    return std::nullopt;
  }
};

class UnavailableStore final : public ICredentialStore {
 public:
  std::optional<std::string> Get(std::string_view) override { return std::nullopt; }
  bool Set(std::string_view, std::string_view) override { return false; }
  bool Remove(std::string_view) override { return false; }
  CredentialError LastError() const override {
    return {.code = CredentialErrorCode::NativeStoreUnavailable};
  }
};

class SystemClock final : public ICredentialClock {
 public:
  std::uint64_t NowEpochSeconds() const override {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now).count());
  }
  void SleepFor(std::chrono::milliseconds duration) const override {
    std::this_thread::sleep_for(duration);
  }
};

class SystemRandom final : public ICredentialRandom {
 public:
  std::string UrlSafe(std::size_t byte_count) override {
    std::random_device random;
    std::string bytes(byte_count, '\0');
    for (auto& byte : bytes) byte = static_cast<char>(random() & 0xffU);
    return ToBase64Url(bytes);
  }
};

class SafeCredentialFile final : public ICredentialFile {
 public:
  std::optional<std::string> Read(const std::filesystem::path& path,
                                  CredentialError& error) const override {
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      error = {.code = CredentialErrorCode::ExternalFileUnavailable};
      return std::nullopt;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      error = {.code = CredentialErrorCode::ExternalFileRejected};
      return std::nullopt;
    }
    const auto handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      error = {.code = CredentialErrorCode::ExternalFileUnavailable};
      return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      CloseHandle(handle);
      error = {.code = CredentialErrorCode::ExternalFileRejected};
      return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaxCredentialBytes) {
      CloseHandle(handle);
      error = {.code = CredentialErrorCode::InvalidCredential};
      return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(handle, contents.data(),
                             static_cast<DWORD>(contents.size()), &read, nullptr);
    CloseHandle(handle);
    if (!ok || read != contents.size()) {
      error = {.code = CredentialErrorCode::ExternalFileUnavailable};
      return std::nullopt;
    }
    return contents;
#else
    const int descriptor =
        open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
      error = {.code = errno == ELOOP ? CredentialErrorCode::ExternalFileRejected
                                      : CredentialErrorCode::ExternalFileUnavailable};
      return std::nullopt;
    }
    struct stat metadata {};
    if (fstat(descriptor, &metadata) != 0) {
      close(descriptor);
      error = {.code = CredentialErrorCode::ExternalFileUnavailable};
      return std::nullopt;
    }
    if (!S_ISREG(metadata.st_mode)) {
      close(descriptor);
      error = {.code = CredentialErrorCode::ExternalFileRejected};
      return std::nullopt;
    }
    if (metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > kMaxCredentialBytes) {
      close(descriptor);
      error = {.code = CredentialErrorCode::InvalidCredential};
      return std::nullopt;
    }
    std::string contents;
    contents.reserve(static_cast<std::size_t>(metadata.st_size));
    std::array<char, 4096> buffer{};
    for (;;) {
      const auto count = read(descriptor, buffer.data(), buffer.size());
      if (count == 0) break;
      if (count < 0) {
        close(descriptor);
        error = {.code = CredentialErrorCode::ExternalFileUnavailable};
        return std::nullopt;
      }
      contents.append(buffer.data(), static_cast<std::size_t>(count));
      if (contents.size() > kMaxCredentialBytes) {
        close(descriptor);
        error = {.code = CredentialErrorCode::InvalidCredential};
        return std::nullopt;
      }
    }
    close(descriptor);
    return contents;
#endif
  }

  bool Rewrite(const std::filesystem::path& path, std::string_view contents,
               CredentialError& error) const override {
    if (contents.size() > kMaxCredentialBytes) {
      error = {.code = CredentialErrorCode::InvalidCredential};
      return false;
    }
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      error = {.code = CredentialErrorCode::ExternalFileUnavailable};
      return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      error = {.code = CredentialErrorCode::ExternalFileRejected};
      return false;
    }
    const auto handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      error = {.code = CredentialErrorCode::ExternalFileUnavailable};
      return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      CloseHandle(handle);
      error = {.code = CredentialErrorCode::ExternalFileRejected};
      return false;
    }
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(handle)) {
      CloseHandle(handle);
      error = {.code = CredentialErrorCode::ExternalFileUnavailable};
      return false;
    }
    DWORD written = 0;
    const bool ok =
        WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()),
                  &written, nullptr) &&
        written == contents.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) error = {.code = CredentialErrorCode::ExternalFileUnavailable};
    return ok;
#else
    const int descriptor =
        open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
      error = {.code = errno == ELOOP ? CredentialErrorCode::ExternalFileRejected
                                      : CredentialErrorCode::ExternalFileUnavailable};
      return false;
    }
    struct stat metadata {};
    if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
      close(descriptor);
      error = {.code = CredentialErrorCode::ExternalFileRejected};
      return false;
    }
    bool ok = ftruncate(descriptor, 0) == 0;
    std::size_t offset = 0;
    while (ok && offset < contents.size()) {
      const auto count = write(descriptor, contents.data() + offset,
                               contents.size() - offset);
      if (count <= 0) ok = false;
      else offset += static_cast<std::size_t>(count);
    }
    ok = ok && fsync(descriptor) == 0;
    close(descriptor);
    if (!ok) error = {.code = CredentialErrorCode::ExternalFileUnavailable};
    return ok;
#endif
  }
};

struct OAuthTokens {
  std::string access_token;
  std::optional<std::string> refresh_token;
  std::optional<std::string> account_id;
  std::uint64_t expires_at = 0;
};

bool IsFresh(const OAuthTokens& token, const ICredentialClock& clock) {
  const auto now = clock.NowEpochSeconds();
  const auto threshold =
      now > std::numeric_limits<std::uint64_t>::max() - kFreshnessMarginSeconds
          ? std::numeric_limits<std::uint64_t>::max()
          : now + kFreshnessMarginSeconds;
  return token.expires_at > threshold;
}

std::string EnvironmentName(CredentialProvider provider) {
  switch (provider) {
    case CredentialProvider::OpenAi: return "OPENAI_API_KEY";
    case CredentialProvider::Anthropic: return "ANTHROPIC_API_KEY";
    case CredentialProvider::Gemini: return "GEMINI_API_KEY";
    case CredentialProvider::OpenRouter: return "OPENROUTER_API_KEY";
    case CredentialProvider::Codex: return "CODEX_ACCESS_TOKEN";
    case CredentialProvider::Custom: return {};
  }
  return {};
}

std::vector<std::string> EnvironmentNames(CredentialProvider provider) {
  std::vector<std::string> names;
  const auto primary = EnvironmentName(provider);
  if (!primary.empty()) names.push_back(primary);
  return names;
}

std::optional<std::string> StoreAccount(CredentialProvider provider) {
  switch (provider) {
    case CredentialProvider::OpenAi: return "openai-api-key";
    case CredentialProvider::Anthropic: return "anthropic-api-key";
    case CredentialProvider::Gemini: return "gemini-api-key";
    case CredentialProvider::OpenRouter: return "openrouter-api-key";
    case CredentialProvider::Codex: return kCodexStoreAccount;
    case CredentialProvider::Custom:
      return std::nullopt;
  }
  return std::nullopt;
}

std::string FormBody(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string body;
  for (const auto& [name, value] : fields) {
    if (!body.empty()) body.push_back('&');
    body += UrlEncode(name);
    body.push_back('=');
    body += UrlEncode(value);
  }
  return body;
}

std::optional<std::string> JsonString(const Json& object, const char* key) {
  if (!object.is_object()) return std::nullopt;
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

std::optional<OAuthTokens> ParseTokens(std::string_view body,
                                       const ICredentialClock& clock,
                                       CredentialError& error) {
  if (body.size() > kMaxOAuthResponseBytes) {
    error = {.code = CredentialErrorCode::InvalidOAuthResponse};
    return std::nullopt;
  }
  Json value;
  try {
    value = Json::parse(body);
  } catch (...) {
    error = {.code = CredentialErrorCode::InvalidOAuthResponse};
    return std::nullopt;
  }
  const auto access = JsonString(value, "access_token");
  if (!access || !ValidSecret(*access)) {
    error = {.code = CredentialErrorCode::InvalidOAuthResponse};
    return std::nullopt;
  }
  std::optional<std::string> refresh;
  if (const auto candidate = JsonString(value, "refresh_token")) {
    if (!ValidSecret(*candidate)) {
      error = {.code = CredentialErrorCode::InvalidOAuthResponse};
      return std::nullopt;
    }
    refresh = candidate;
  }
  const auto claims = ReadJwtMetadata(*access);
  if (!claims) {
    error = {.code = CredentialErrorCode::InvalidOAuthResponse};
    return std::nullopt;
  }
  const auto id_token = JsonString(value, "id_token");
  const auto id_claims = id_token ? ReadJwtMetadata(*id_token) : std::nullopt;
  std::optional<std::string> account =
      claims && claims->account_id
          ? claims->account_id
          : (id_claims ? id_claims->account_id : std::nullopt);
  std::uint64_t expires = claims->expires_at;
  if (expires == 0) {
    if (const auto it = value.find("expires_in"); it != value.end()) {
      if (const auto duration = JsonEpoch(*it)) {
        const auto now = clock.NowEpochSeconds();
        expires = now > std::numeric_limits<std::uint64_t>::max() - *duration
                      ? std::numeric_limits<std::uint64_t>::max()
                      : now + *duration;
      }
    }
  }
  if (expires == 0) {
    error = {.code = CredentialErrorCode::InvalidOAuthResponse};
    return std::nullopt;
  }
  return OAuthTokens{.access_token = *access,
                     .refresh_token = std::move(refresh),
                     .account_id = std::move(account),
                     .expires_at = expires};
}

bool ValidateRedirect(std::string_view uri) {
  if (uri.find('?') != std::string_view::npos ||
      uri.find('#') != std::string_view::npos) {
    return false;
  }
  const auto scheme_end = uri.find("://");
  if (scheme_end == std::string_view::npos ||
      Lower(std::string{uri.substr(0, scheme_end)}) != "http") {
    return false;
  }
  const auto authority_start = scheme_end + 3;
  const auto authority_end = uri.find('/', authority_start);
  const auto authority = uri.substr(
      authority_start, authority_end == std::string_view::npos
                           ? std::string_view::npos
                           : authority_end - authority_start);
  if (authority.empty() || authority.find('@') != std::string_view::npos) {
    return false;
  }
  std::string host{authority};
  std::string_view port;
  bool has_port = false;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos) return false;
    host = std::string{authority.substr(0, close + 1)};
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') return false;
      has_port = true;
      port = authority.substr(close + 2);
    }
  } else if (const auto colon = authority.rfind(':');
             colon != std::string_view::npos) {
    host = std::string{authority.substr(0, colon)};
    has_port = true;
    port = authority.substr(colon + 1);
  }
  if (has_port) {
    if (port.empty()) return false;
    if (!std::ranges::all_of(port, [](unsigned char value) {
          return std::isdigit(value);
        })) {
      return false;
    }
    try {
      const auto number = std::stoul(std::string{port});
      if (number == 0 || number > 65535) return false;
    } catch (...) {
      return false;
    }
  }
  return host == "127.0.0.1" || host == "[::1]";
}

}  // namespace

void CancellationToken::Cancel() noexcept { cancelled_.store(true); }

bool CancellationToken::IsCancelled() const noexcept {
  return cancelled_.load();
}

std::optional<CredentialProvider> ParseCredentialProvider(std::string_view value) {
  const auto normalized = Lower(std::string{value});
  if (normalized == "openai") return CredentialProvider::OpenAi;
  if (normalized == "anthropic") return CredentialProvider::Anthropic;
  if (normalized == "gemini") return CredentialProvider::Gemini;
  if (normalized == "openrouter") return CredentialProvider::OpenRouter;
  if (normalized == "codex") return CredentialProvider::Codex;
  if (normalized == "custom") return CredentialProvider::Custom;
  return std::nullopt;
}

std::string ToString(CredentialProvider provider) {
  switch (provider) {
    case CredentialProvider::OpenAi: return "openai";
    case CredentialProvider::Anthropic: return "anthropic";
    case CredentialProvider::Gemini: return "gemini";
    case CredentialProvider::OpenRouter: return "openrouter";
    case CredentialProvider::Codex: return "codex";
    case CredentialProvider::Custom: return "custom";
  }
  return {};
}

std::string ResolvedCredential::RedactedDescription() const {
  return account_id ? "ResolvedCredential(account_id=present)"
                    : "ResolvedCredential(account_id=absent)";
}

std::string CredentialError::Message() const {
  switch (code) {
    case CredentialErrorCode::None: return {};
    case CredentialErrorCode::Cancelled: return "credential operation was cancelled";
    case CredentialErrorCode::MissingCredential: return "the selected provider has no configured credential";
    case CredentialErrorCode::InvalidCredential: return "the provider credential is invalid";
    case CredentialErrorCode::UnsupportedProvider: return "the selected provider does not support this credential operation";
    case CredentialErrorCode::NativeStoreUnavailable: return "the native credential store is unavailable";
    case CredentialErrorCode::ExternalPathRejected: return "the external Codex auth file path must be absolute";
    case CredentialErrorCode::ExternalFileUnavailable: return "the external Codex auth file is unavailable";
    case CredentialErrorCode::ExternalFileRejected: return "the external Codex auth path is not a regular no-follow file";
    case CredentialErrorCode::ExternalCredentialUnowned: return "the Codex credential is managed by the configured external auth file; llm-rewriter cannot configure or clear it";
    case CredentialErrorCode::OAuthTransportFailed: return "the ChatGPT OAuth service could not be reached";
    case CredentialErrorCode::OAuthRejected: return "ChatGPT OAuth rejected the credential operation";
    case CredentialErrorCode::OAuthTimedOut: return "ChatGPT device authorization timed out";
    case CredentialErrorCode::InvalidOAuthResponse: return "ChatGPT OAuth returned invalid credential data";
  }
  return "credential operation failed";
}

std::string BrowserAuthorization::RedactedDescription() const {
  return "BrowserAuthorization(state=redacted)";
}

std::string DeviceAuthorization::RedactedDescription() const {
  return "DeviceAuthorization(user_code=redacted)";
}

struct CredentialResolver::Impl {
  struct PendingBrowser {
    std::string verifier;
    std::string redirect_uri;
  };
  struct PendingDevice {
    std::string device_auth_id;
    std::string user_code;
  };

  std::optional<std::filesystem::path> external_codex_auth;
  CredentialDependencies dependencies;
  CredentialError construction_error;
  mutable std::mutex mutex;
  std::map<std::string, PendingBrowser> browser;
  std::map<std::string, PendingDevice> device;
  std::map<CredentialProvider, ResolvedCredential> api_cache;
  std::optional<OAuthTokens> codex_cache;
};

CredentialResolver::CredentialResolver(
    std::optional<std::filesystem::path> external_codex_auth,
    CredentialDependencies dependencies)
    : impl_(std::make_shared<Impl>()) {
  if (external_codex_auth) {
    impl_->external_codex_auth = ExpandUserPath(*external_codex_auth);
  }
  impl_->dependencies = std::move(dependencies);
  if (impl_->external_codex_auth &&
      !impl_->external_codex_auth->is_absolute()) {
    impl_->construction_error = {.code = CredentialErrorCode::ExternalPathRejected};
  }
  if (!impl_->dependencies.store) impl_->dependencies.store = std::make_shared<UnavailableStore>();
  if (!impl_->dependencies.environment) impl_->dependencies.environment = std::make_shared<ProcessEnvironment>();
  if (!impl_->dependencies.clock) impl_->dependencies.clock = std::make_shared<SystemClock>();
  if (!impl_->dependencies.random) impl_->dependencies.random = std::make_shared<SystemRandom>();
  if (!impl_->dependencies.file) impl_->dependencies.file = std::make_shared<SafeCredentialFile>();
}

CredentialError CredentialResolver::ConstructionError() const {
  return impl_->construction_error;
}

CredentialResolution CredentialResolver::Resolve(
    std::string_view provider_name, std::string_view configured_credential,
    const CancellationToken* cancellation) {
  CredentialResolution result;
  if (impl_->construction_error) {
    result.error = impl_->construction_error;
    return result;
  }
  if (cancellation && cancellation->IsCancelled()) {
    result.error = {.code = CredentialErrorCode::Cancelled};
    return result;
  }
  const auto provider = ParseCredentialProvider(provider_name);
  if (!provider) {
    result.error = {.code = CredentialErrorCode::UnsupportedProvider};
    return result;
  }
  const auto environment_names = EnvironmentNames(*provider);
  for (const auto& environment_name : environment_names) {
    if (const auto value =
            impl_->dependencies.environment->Get(environment_name)) {
      result.source = CredentialSource::Environment;
      if (!ValidSecret(*value)) {
        result.error = {.code = CredentialErrorCode::InvalidCredential};
        return result;
      }
      if (*provider == CredentialProvider::Codex) {
        const auto claims = ReadJwtMetadata(*value);
        if (!claims || !claims->account_id ||
            claims->expires_at <=
                impl_->dependencies.clock->NowEpochSeconds() +
                    kFreshnessMarginSeconds) {
          result.error = {.code = CredentialErrorCode::InvalidCredential};
          return result;
        }
        result.credential = ResolvedCredential{.access_token = *value,
                                               .account_id = claims->account_id};
      } else {
        result.credential =
            ResolvedCredential{.access_token = *value, .account_id = std::nullopt};
      }
      return result;
    }
  }

  if (*provider != CredentialProvider::Codex && !configured_credential.empty()) {
    result.source = CredentialSource::Configuration;
    if (!ValidSecret(configured_credential)) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    result.credential = ResolvedCredential{.access_token = std::string{configured_credential},
                                           .account_id = std::nullopt};
    return result;
  }

  if (*provider == CredentialProvider::Custom) {
    result.source = CredentialSource::Keyless;
    return result;
  }
  if (*provider == CredentialProvider::Codex && impl_->external_codex_auth) {
    result.source = CredentialSource::ExternalFile;
    CredentialError file_error;
    const auto content =
        impl_->dependencies.file->Read(*impl_->external_codex_auth, file_error);
    if (!content) {
      result.error = file_error.ok()
                         ? CredentialError{
                               .code = CredentialErrorCode::ExternalFileUnavailable}
                         : file_error;
      return result;
    }
    if (content->size() > kMaxCredentialBytes) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    Json document;
    try {
      document = Json::parse(*content);
    } catch (...) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    Json* token_root = &document;
    if (document.contains("tokens")) {
      token_root = &document["tokens"];
      if (!token_root->is_object()) {
        result.error = {.code = CredentialErrorCode::InvalidCredential};
        return result;
      }
    }
    const auto access = JsonString(*token_root, "access_token");
    if (!access || !ValidSecret(*access)) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    const auto claims = ReadJwtMetadata(*access);
    if (!claims) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    std::optional<std::string> account = claims->account_id;
    if (!account) {
      if (const auto value = JsonString(*token_root, "account_id");
          value && ValidIdentifier(*value)) {
        account = value;
      }
    }
    const auto refresh = JsonString(*token_root, "refresh_token");
    if (refresh && !ValidSecret(*refresh)) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    OAuthTokens tokens{.access_token = *access,
                       .refresh_token = refresh,
                       .account_id = account,
                       .expires_at = claims->expires_at};
    if (IsFresh(tokens, *impl_->dependencies.clock) && account) {
      result.credential = ResolvedCredential{.access_token = *access,
                                             .account_id = account};
      return result;
    }
    if (!refresh || !ValidSecret(*refresh)) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    if (!impl_->dependencies.http) {
      result.error = {.code = CredentialErrorCode::OAuthTransportFailed};
      return result;
    }
    HttpRequest request;
    request.method = "POST";
    request.url = std::string{kOAuthIssuer} + "/oauth/token";
    request.body = FormBody({{"grant_type", "refresh_token"},
                             {"refresh_token", *refresh},
                             {"client_id", kOAuthClientId}});
    request.content_type = "application/x-www-form-urlencoded";
    request.follow_redirects = false;
    request.max_response_bytes = kMaxOAuthResponseBytes;
    request.cancellation_token = cancellation;
    const auto response = impl_->dependencies.http->Send(request);
    if (cancellation && cancellation->IsCancelled()) {
      result.error = {.code = CredentialErrorCode::Cancelled};
      return result;
    }
    if (!response.error.empty()) {
      result.error = {.code = CredentialErrorCode::OAuthTransportFailed};
      return result;
    }
    if (response.status < 200 || response.status >= 300) {
      result.error = {.code = CredentialErrorCode::OAuthRejected};
      return result;
    }
    CredentialError parse_error;
    auto refreshed =
        ParseTokens(response.body, *impl_->dependencies.clock, parse_error);
    if (!refreshed) {
      result.error = parse_error;
      return result;
    }
    if (!IsFresh(*refreshed, *impl_->dependencies.clock)) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    if (!refreshed->refresh_token) refreshed->refresh_token = refresh;
    if (!refreshed->account_id) refreshed->account_id = account;
    if (!refreshed->account_id) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    Json updated = document;
    Json* updated_root = &updated;
    if (updated.contains("tokens")) updated_root = &updated["tokens"];
    (*updated_root)["access_token"] = refreshed->access_token;
    (*updated_root)["refresh_token"] = *refreshed->refresh_token;
    (*updated_root)["account_id"] = *refreshed->account_id;
    const auto serialized = updated.dump(2) + "\n";
    if (serialized.size() > kMaxCredentialBytes) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    CredentialError rewrite_error;
    if (!impl_->dependencies.file->Rewrite(*impl_->external_codex_auth,
                                           serialized, rewrite_error)) {
      result.error = rewrite_error.ok()
                         ? CredentialError{
                               .code = CredentialErrorCode::ExternalFileUnavailable}
                         : rewrite_error;
      return result;
    }
    result.credential = ResolvedCredential{.access_token = refreshed->access_token,
                                           .account_id = refreshed->account_id};
    return result;
  }

  if (*provider == CredentialProvider::Codex) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->codex_cache && IsFresh(*impl_->codex_cache,
                                       *impl_->dependencies.clock) &&
        impl_->codex_cache->account_id) {
      result.source = CredentialSource::NativeStore;
      result.credential = ResolvedCredential{
          .access_token = impl_->codex_cache->access_token,
          .account_id = impl_->codex_cache->account_id};
      return result;
    }
  }

  const auto account = StoreAccount(*provider);
  if (!account) {
    result.error = {.code = CredentialErrorCode::MissingCredential};
    return result;
  }
  {
    std::scoped_lock lock(impl_->mutex);
    if (const auto cached = impl_->api_cache.find(*provider);
        cached != impl_->api_cache.end()) {
      result.source = CredentialSource::NativeStore;
      result.credential = cached->second;
      return result;
    }
  }
  const auto secret = impl_->dependencies.store->Get(*account);
  if (!secret) {
    result.error = impl_->dependencies.store->LastError();
    if (result.error.ok()) result.error = {.code = CredentialErrorCode::MissingCredential};
    return result;
  }
  result.source = CredentialSource::NativeStore;
  if (*provider != CredentialProvider::Codex) {
    if (!ValidSecret(*secret)) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
    result.credential =
        ResolvedCredential{.access_token = *secret, .account_id = std::nullopt};
    std::scoped_lock lock(impl_->mutex);
    impl_->api_cache[*provider] = *result.credential;
    return result;
  }

  if (!ValidSecret(*secret) || !impl_->dependencies.http) {
    result.error = {.code = !ValidSecret(*secret)
                              ? CredentialErrorCode::InvalidCredential
                              : CredentialErrorCode::OAuthTransportFailed};
    return result;
  }
  HttpRequest request;
  request.method = "POST";
  request.url = std::string{kOAuthIssuer} + "/oauth/token";
  request.body = FormBody({{"grant_type", "refresh_token"},
                           {"refresh_token", *secret},
                           {"client_id", kOAuthClientId}});
  request.content_type = "application/x-www-form-urlencoded";
  request.follow_redirects = false;
  request.max_response_bytes = kMaxOAuthResponseBytes;
  request.cancellation_token = cancellation;
  const auto response = impl_->dependencies.http->Send(request);
  if (cancellation && cancellation->IsCancelled()) {
    result.error = {.code = CredentialErrorCode::Cancelled};
    return result;
  }
  if (!response.error.empty()) {
    result.error = {.code = CredentialErrorCode::OAuthTransportFailed};
    return result;
  }
  if (response.status < 200 || response.status >= 300) {
    result.error = {.code = CredentialErrorCode::OAuthRejected};
    return result;
  }
  CredentialError parse_error;
  auto tokens = ParseTokens(response.body, *impl_->dependencies.clock, parse_error);
  if (!tokens) {
    result.error = parse_error;
    return result;
  }
  if (!IsFresh(*tokens, *impl_->dependencies.clock)) {
    result.error = {.code = CredentialErrorCode::InvalidCredential};
    return result;
  }
  if (!tokens->refresh_token) tokens->refresh_token = *secret;
  if (!tokens->account_id) {
    result.error = {.code = CredentialErrorCode::InvalidCredential};
    return result;
  }
  if (tokens->refresh_token != secret &&
      !impl_->dependencies.store->Set(*account, *tokens->refresh_token)) {
    result.error = impl_->dependencies.store->LastError();
    if (result.error.ok()) result.error = {.code = CredentialErrorCode::NativeStoreUnavailable};
    return result;
  }
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->codex_cache = *tokens;
  }
  result.credential = ResolvedCredential{.access_token = tokens->access_token,
                                         .account_id = tokens->account_id};
  return result;
}

CredentialStatus CredentialResolver::Status(
    std::string_view provider_name,
    std::string_view configured_credential) const {
  CredentialStatus result;
  if (impl_->construction_error) {
    result.error = impl_->construction_error;
    return result;
  }
  const auto provider = ParseCredentialProvider(provider_name);
  if (!provider) {
    result.error = {.code = CredentialErrorCode::UnsupportedProvider};
    return result;
  }
  const auto names = EnvironmentNames(*provider);
  for (const auto& name : names) {
    if (const auto value = impl_->dependencies.environment->Get(name)) {
      result.source = CredentialSource::Environment;
      if (!ValidSecret(*value)) result.error = {.code = CredentialErrorCode::InvalidCredential};
      return result;
    }
  }

  if (*provider != CredentialProvider::Codex && !configured_credential.empty()) {
    result.source = CredentialSource::Configuration;
    if (!ValidSecret(configured_credential)) {
      result.error = {.code = CredentialErrorCode::InvalidCredential};
    }
    return result;
  }

  if (*provider == CredentialProvider::Custom) {
    result.source = CredentialSource::Keyless;
    return result;
  }
  if (*provider == CredentialProvider::Codex && impl_->external_codex_auth) {
    result.source = CredentialSource::ExternalFile;
    return result;
  }
  const auto account = StoreAccount(*provider);
  if (!account) {
    result.source = CredentialSource::Unconfigured;
    return result;
  }
  const auto value = impl_->dependencies.store->Get(*account);
  if (value) {
    result.source = CredentialSource::NativeStore;
  } else {
    result.error = impl_->dependencies.store->LastError();
    if (result.error.ok()) result.source = CredentialSource::Unconfigured;
  }
  return result;
}

CredentialError CredentialResolver::ConfigureApiKey(std::string_view provider_name,
                                                    std::string_view api_key) {
  if (impl_->construction_error) return impl_->construction_error;
  const auto provider = ParseCredentialProvider(provider_name);
  if (!provider) return {.code = CredentialErrorCode::UnsupportedProvider};
  if (*provider == CredentialProvider::Codex && impl_->external_codex_auth) {
    return {.code = CredentialErrorCode::ExternalCredentialUnowned};
  }
  const auto account = StoreAccount(*provider);
  if (!account || *provider == CredentialProvider::Codex) {
    return {.code = CredentialErrorCode::UnsupportedProvider};
  }
  if (!ValidSecret(api_key)) return {.code = CredentialErrorCode::InvalidCredential};
  if (!impl_->dependencies.store->Set(*account, api_key)) {
    auto error = impl_->dependencies.store->LastError();
    return error.ok() ? CredentialError{.code = CredentialErrorCode::NativeStoreUnavailable}
                      : error;
  }
  std::scoped_lock lock(impl_->mutex);
  impl_->api_cache.erase(*provider);
  return {};
}

CredentialError CredentialResolver::Clear(std::string_view provider_name) {
  if (impl_->construction_error) return impl_->construction_error;
  const auto provider = ParseCredentialProvider(provider_name);
  if (!provider) return {.code = CredentialErrorCode::UnsupportedProvider};
  if (*provider == CredentialProvider::Codex && impl_->external_codex_auth) {
    return {.code = CredentialErrorCode::ExternalCredentialUnowned};
  }
  const auto account = StoreAccount(*provider);
  if (!account) return {.code = CredentialErrorCode::UnsupportedProvider};
  if (!impl_->dependencies.store->Remove(*account)) {
    auto error = impl_->dependencies.store->LastError();
    if (!error.ok()) return error;
    return {.code = CredentialErrorCode::NativeStoreUnavailable};
  }
  std::scoped_lock lock(impl_->mutex);
  impl_->api_cache.erase(*provider);
  impl_->codex_cache.reset();
  return {};
}

BrowserAuthorization CredentialResolver::BeginBrowserAuthorization(
    std::string_view redirect_uri, CredentialError& error) {
  BrowserAuthorization result;
  if (impl_->construction_error) {
    error = impl_->construction_error;
    return result;
  }
  if (impl_->external_codex_auth) {
    error = {.code = CredentialErrorCode::ExternalCredentialUnowned};
    return result;
  }
  if (!ValidateRedirect(redirect_uri)) {
    error = {.code = CredentialErrorCode::OAuthRejected};
    return result;
  }
  const auto verifier = impl_->dependencies.random->UrlSafe(64);
  const auto state = impl_->dependencies.random->UrlSafe(32);
  if (!ValidSecret(verifier) || !ValidSecret(state)) {
    error = {.code = CredentialErrorCode::OAuthTransportFailed};
    return result;
  }
  const auto challenge = ToBase64Url(Sha256(verifier));
  result.redirect_uri = std::string{redirect_uri};
  result.expected_state = state;
  result.authorization_url =
      std::string{kOAuthIssuer} +
      "/oauth/authorize?response_type=code&client_id=" +
      UrlEncode(kOAuthClientId) + "&redirect_uri=" + UrlEncode(redirect_uri) +
      "&scope=" + UrlEncode(kOAuthScopes) + "&code_challenge=" +
      UrlEncode(challenge) + "&code_challenge_method=S256&state=" +
      UrlEncode(state) +
      "&id_token_add_organizations=true&codex_cli_simplified_flow=true&originator=llm-rewriter";
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->browser[state] =
        Impl::PendingBrowser{.verifier = verifier,
                             .redirect_uri = std::string{redirect_uri}};
  }
  error = {};
  return result;
}

CredentialError CredentialResolver::CompleteBrowserAuthorization(
    const BrowserAuthorization& authorization, std::string_view returned_state,
    std::string_view authorization_code, const CancellationToken* cancellation) {
  if (impl_->construction_error) return impl_->construction_error;
  if (impl_->external_codex_auth) {
    return {.code = CredentialErrorCode::ExternalCredentialUnowned};
  }
  if (cancellation && cancellation->IsCancelled()) return {.code = CredentialErrorCode::Cancelled};
  if (!ConstantTimeEqual(authorization.expected_state, returned_state) ||
      !ValidSecret(authorization_code)) {
    return {.code = CredentialErrorCode::OAuthRejected};
  }
  Impl::PendingBrowser pending;
  {
    std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->browser.find(authorization.expected_state);
    if (it == impl_->browser.end()) return {.code = CredentialErrorCode::OAuthRejected};
    pending = it->second;
    impl_->browser.erase(it);
  }
  if (!impl_->dependencies.http) return {.code = CredentialErrorCode::OAuthTransportFailed};
  HttpRequest request;
  request.method = "POST";
  request.url = std::string{kOAuthIssuer} + "/oauth/token";
  request.body = FormBody({{"grant_type", "authorization_code"},
                           {"code", std::string{authorization_code}},
                           {"redirect_uri", pending.redirect_uri},
                           {"client_id", kOAuthClientId},
                           {"code_verifier", pending.verifier}});
  request.content_type = "application/x-www-form-urlencoded";
  request.follow_redirects = false;
  request.max_response_bytes = kMaxOAuthResponseBytes;
  request.cancellation_token = cancellation;
  const auto response = impl_->dependencies.http->Send(request);
  if (cancellation && cancellation->IsCancelled()) {
    return {.code = CredentialErrorCode::Cancelled};
  }
  if (!response.error.empty()) return {.code = CredentialErrorCode::OAuthTransportFailed};
  if (response.status < 200 || response.status >= 300) return {.code = CredentialErrorCode::OAuthRejected};
  CredentialError parse_error;
  const auto tokens = ParseTokens(response.body, *impl_->dependencies.clock, parse_error);
  if (!tokens || !tokens->refresh_token)
    return tokens ? CredentialError{.code = CredentialErrorCode::InvalidOAuthResponse}
                  : parse_error;
  if (!IsFresh(*tokens, *impl_->dependencies.clock))
    return {.code = CredentialErrorCode::InvalidCredential};
  if (!impl_->dependencies.store->Set(kCodexStoreAccount, *tokens->refresh_token)) {
    auto error = impl_->dependencies.store->LastError();
    return error.ok() ? CredentialError{.code = CredentialErrorCode::NativeStoreUnavailable} : error;
  }
  std::scoped_lock lock(impl_->mutex);
  impl_->codex_cache = *tokens;
  return {};
}

DeviceAuthorization CredentialResolver::RequestDeviceAuthorization(
    const CancellationToken* cancellation, CredentialError& error) {
  DeviceAuthorization result;
  if (impl_->construction_error) { error = impl_->construction_error; return result; }
  if (impl_->external_codex_auth) {
    error = {.code = CredentialErrorCode::ExternalCredentialUnowned};
    return result;
  }
  if (cancellation && cancellation->IsCancelled()) { error = {.code = CredentialErrorCode::Cancelled}; return result; }
  if (!impl_->dependencies.http) { error = {.code = CredentialErrorCode::OAuthTransportFailed}; return result; }
  HttpRequest request;
  request.method = "POST";
  request.url = std::string{kOAuthIssuer} + "/api/accounts/deviceauth/usercode";
  request.body = Json{{"client_id", kOAuthClientId}}.dump();
  request.content_type = "application/json";
  request.follow_redirects = false;
  request.max_response_bytes = kMaxOAuthResponseBytes;
  request.cancellation_token = cancellation;
  const auto response = impl_->dependencies.http->Send(request);
  if (cancellation && cancellation->IsCancelled()) {
    error = {.code = CredentialErrorCode::Cancelled};
    return result;
  }
  if (!response.error.empty()) { error = {.code = CredentialErrorCode::OAuthTransportFailed}; return result; }
  if (response.status < 200 || response.status >= 300) { error = {.code = CredentialErrorCode::OAuthRejected}; return result; }
  if (response.body.size() > kMaxOAuthResponseBytes) { error = {.code = CredentialErrorCode::InvalidOAuthResponse}; return result; }
  Json value;
  try { value = Json::parse(response.body); } catch (...) { error = {.code = CredentialErrorCode::InvalidOAuthResponse}; return result; }
  const auto id = JsonString(value, "device_auth_id");
  const auto code = JsonString(value, "user_code").value_or(
      JsonString(value, "usercode").value_or(""));
  if (!id || !ValidSecret(*id) || !ValidIdentifier(code)) { error = {.code = CredentialErrorCode::InvalidOAuthResponse}; return result; }
  std::uint64_t interval = kDefaultDevicePollSeconds;
  if (const auto it = value.find("interval"); it != value.end()) {
    if (it->is_number_unsigned()) interval = it->get<std::uint64_t>();
    else if (it->is_string()) {
      try {
        const auto text = it->get<std::string>();
        if (text.empty() || !std::ranges::all_of(text, [](unsigned char value) {
              return std::isdigit(value);
            })) {
          throw std::invalid_argument("interval");
        }
        std::size_t parsed = 0;
        interval = std::stoull(text, &parsed);
        if (parsed != text.size()) throw std::invalid_argument("interval");
      }
      catch (...) { error = {.code = CredentialErrorCode::InvalidOAuthResponse}; return result; }
    } else {
      error = {.code = CredentialErrorCode::InvalidOAuthResponse};
      return result;
    }
  }
  if (interval > kMaxDevicePollSeconds) {
    error = {.code = CredentialErrorCode::InvalidOAuthResponse};
    return result;
  }
  interval = std::max(interval, kDefaultDevicePollSeconds);
  result.verification_url = std::string{kOAuthIssuer} + "/codex/device";
  result.user_code = code;
  result.interval_seconds = interval;
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->device[code] =
        Impl::PendingDevice{.device_auth_id = *id, .user_code = code};
  }
  error = {};
  return result;
}

CredentialError CredentialResolver::CompleteDeviceAuthorization(
    const DeviceAuthorization& authorization, const CancellationToken* cancellation) {
  if (impl_->construction_error) return impl_->construction_error;
  if (impl_->external_codex_auth) {
    return {.code = CredentialErrorCode::ExternalCredentialUnowned};
  }
  if (cancellation && cancellation->IsCancelled()) {
    return {.code = CredentialErrorCode::Cancelled};
  }
  if (!impl_->dependencies.http) return {.code = CredentialErrorCode::OAuthTransportFailed};
  Impl::PendingDevice pending;
  {
    std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->device.find(authorization.user_code);
    if (it == impl_->device.end()) return {.code = CredentialErrorCode::OAuthRejected};
    pending = it->second;
  }
  const auto started = impl_->dependencies.clock->NowEpochSeconds();
  const auto deadline =
      started > std::numeric_limits<std::uint64_t>::max() - kDeviceTimeoutSeconds
          ? std::numeric_limits<std::uint64_t>::max()
          : started + kDeviceTimeoutSeconds;
  for (;;) {
    if (cancellation && cancellation->IsCancelled()) return {.code = CredentialErrorCode::Cancelled};
    if (impl_->dependencies.clock->NowEpochSeconds() >= deadline) return {.code = CredentialErrorCode::OAuthTimedOut};
    HttpRequest request;
    request.method = "POST";
    request.url = std::string{kOAuthIssuer} + "/api/accounts/deviceauth/token";
    request.body = Json{{"device_auth_id", pending.device_auth_id},
                        {"user_code", pending.user_code}}.dump();
    request.content_type = "application/json";
    request.follow_redirects = false;
    request.max_response_bytes = kMaxOAuthResponseBytes;
    request.cancellation_token = cancellation;
    const auto response = impl_->dependencies.http->Send(request);
    if (cancellation && cancellation->IsCancelled()) {
      return {.code = CredentialErrorCode::Cancelled};
    }
    if (!response.error.empty()) return {.code = CredentialErrorCode::OAuthTransportFailed};
    if (response.status >= 200 && response.status < 300) {
      if (response.body.size() > kMaxOAuthResponseBytes) {
        return {.code = CredentialErrorCode::InvalidOAuthResponse};
      }
      Json value;
      try { value = Json::parse(response.body); } catch (...) { return {.code = CredentialErrorCode::InvalidOAuthResponse}; }
      const auto code = JsonString(value, "authorization_code");
      const auto verifier = JsonString(value, "code_verifier");
      if (!code || !verifier || !ValidSecret(*code) || !ValidSecret(*verifier)) return {.code = CredentialErrorCode::InvalidOAuthResponse};
      HttpRequest token_request;
      token_request.method = "POST";
      token_request.url = std::string{kOAuthIssuer} + "/oauth/token";
      token_request.body = FormBody({{"grant_type", "authorization_code"},
                                     {"code", *code},
                                     {"redirect_uri", std::string{kOAuthIssuer} + "/deviceauth/callback"},
                                     {"client_id", kOAuthClientId},
                                     {"code_verifier", *verifier}});
      token_request.content_type = "application/x-www-form-urlencoded";
      token_request.follow_redirects = false;
      token_request.max_response_bytes = kMaxOAuthResponseBytes;
      token_request.cancellation_token = cancellation;
      const auto token_response = impl_->dependencies.http->Send(token_request);
      if (cancellation && cancellation->IsCancelled()) {
        return {.code = CredentialErrorCode::Cancelled};
      }
      if (!token_response.error.empty()) return {.code = CredentialErrorCode::OAuthTransportFailed};
      if (token_response.status < 200 || token_response.status >= 300) return {.code = CredentialErrorCode::OAuthRejected};
      CredentialError parse_error;
      const auto tokens = ParseTokens(token_response.body, *impl_->dependencies.clock, parse_error);
      if (!tokens || !tokens->refresh_token)
        return tokens ? CredentialError{.code = CredentialErrorCode::InvalidOAuthResponse}
                      : parse_error;
      if (!IsFresh(*tokens, *impl_->dependencies.clock))
        return {.code = CredentialErrorCode::InvalidCredential};
      if (!impl_->dependencies.store->Set(kCodexStoreAccount, *tokens->refresh_token)) {
        auto error = impl_->dependencies.store->LastError();
        return error.ok() ? CredentialError{.code = CredentialErrorCode::NativeStoreUnavailable} : error;
      }
      std::scoped_lock lock(impl_->mutex);
      impl_->codex_cache = *tokens;
      impl_->device.erase(authorization.user_code);
      return {};
    }
    if (response.status != 403 && response.status != 404) return {.code = CredentialErrorCode::OAuthRejected};
    const auto interval = std::clamp(authorization.interval_seconds,
                                     kDefaultDevicePollSeconds,
                                     kMaxDevicePollSeconds);
    impl_->dependencies.clock->SleepFor(std::chrono::seconds(interval));
  }
}

}  // namespace llm_rewriter
