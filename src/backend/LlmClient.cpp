#include "llm_rewriter/LlmClient.hpp"

#include "llm_rewriter/Diagnostics.hpp"
#include "llm_rewriter/TokenEstimate.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llm_rewriter {
namespace {

using Json = nlohmann::json;

std::string JoinUrl(std::string base_url, const std::string& path) {
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  return base_url + path;
}

std::string NormalizedProvider(const AppConfig& config) {
  std::string provider = config.provider;
  std::ranges::transform(provider, provider.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return provider;
}

bool IsCustomProvider(const AppConfig& config) {
  return NormalizedProvider(config) == "custom";
}

ApiFormat EffectiveApiFormat(const AppConfig& config) {
  const auto provider = NormalizedProvider(config);
  if (provider == "anthropic") return ApiFormat::AnthropicMessages;
  if (provider != "custom") return ApiFormat::OpenAiChat;
  return config.api_format;
}

std::string ProviderBaseUrl(const AppConfig& config) {
  const auto provider = NormalizedProvider(config);
  if (provider == "openai") return "https://api.openai.com/v1";
  if (provider == "anthropic") return "https://api.anthropic.com";
  if (provider == "gemini") {
    return "https://generativelanguage.googleapis.com/v1beta/openai";
  }
  if (provider == "openrouter") return "https://openrouter.ai/api/v1";
  if (provider == "custom") return config.base_url;
  return config.base_url;
}

bool IsUnreservedUrlChar(unsigned char value) {
  return std::isalnum(value) || value == '-' || value == '.' || value == '_' ||
         value == '~';
}

std::string UrlEncode(std::string_view value) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string output;
  for (const unsigned char character : value) {
    if (IsUnreservedUrlChar(character)) {
      output.push_back(static_cast<char>(character));
      continue;
    }
    output.push_back('%');
    output.push_back(digits[character >> 4]);
    output.push_back(digits[character & 0x0F]);
  }
  return output;
}

std::string AppendQueryParameters(
    std::string url,
    const std::map<std::string, std::string>& query_parameters) {
  if (query_parameters.empty()) {
    return url;
  }
  char separator = url.find('?') == std::string::npos ? '?' : '&';
  for (const auto& [key, value] : query_parameters) {
    if (key.empty()) {
      continue;
    }
    url.push_back(separator);
    separator = '&';
    url += UrlEncode(key);
    url.push_back('=');
    url += UrlEncode(value);
  }
  return url;
}

std::string EnvValue(const std::string& name) {
  if (name.empty()) {
    return {};
  }
  if (const char* value = std::getenv(name.c_str())) {
    return value;
  }
  return {};
}

bool IsCodexProvider(const AppConfig& config) {
  return NormalizedProvider(config) == "codex";
}

std::string NewRequestId() {
  static std::atomic<unsigned long long> sequence{0};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return "rw-" + std::to_string(now) + "-" +
         std::to_string(++sequence);
}

std::string CanonicalEnvironmentName(const AppConfig& config) {
  const auto provider = NormalizedProvider(config);
  if (provider == "openai") return "OPENAI_API_KEY";
  if (provider == "anthropic") return "ANTHROPIC_API_KEY";
  if (provider == "gemini") return "GEMINI_API_KEY";
  if (provider == "openrouter") return "OPENROUTER_API_KEY";
  if (provider == "codex") return "CODEX_ACCESS_TOKEN";
  return {};
}

std::string Lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string ResponseHeader(const HttpResponse& response,
                           std::string_view wanted) {
  const auto normalized = Lower(std::string{wanted});
  for (const auto& [name, value] : response.headers) {
    if (Lower(name) == normalized) {
      return value;
    }
  }
  return {};
}

}  // namespace

namespace {

std::vector<std::string> AuthHeaders(
    const AppConfig& config,
    const std::optional<ResolvedCredential>& credential) {
  std::vector<std::string> headers;
  const auto token =
      credential ? credential->access_token : std::string{};
  const bool codex = IsCodexProvider(config);
  const auto provider = NormalizedProvider(config);
  const bool anthropic = EffectiveApiFormat(config) == ApiFormat::AnthropicMessages;
  const bool gemini = provider == "gemini";
  if (!token.empty() && !anthropic && !gemini) {
    headers.push_back("Authorization: Bearer " + token);
  }
  if (codex) {
    if (credential && credential->account_id) {
      headers.push_back("ChatGPT-Account-ID: " + *credential->account_id);
    }
    headers.push_back("originator: codex_cli_rs");
    headers.push_back("OpenAI-Beta: responses=experimental");
    headers.push_back("User-Agent: codex_cli_rs/0.0.1");
    headers.push_back("Accept: text/event-stream");
  }
  if (anthropic) {
    if (!token.empty()) {
      headers.push_back("x-api-key: " + token);
    }
    headers.push_back("anthropic-version: 2023-06-01");
  }
  if (gemini) headers.push_back("x-goog-api-key: " + token);
  if (IsCustomProvider(config)) {
    for (const auto& [name, value] : config.custom_provider.headers) {
      if (!name.empty()) {
        headers.push_back(name + ": " + value);
      }
    }
  }
  return headers;
}

std::string SanitizeErrorDetail(std::string detail) {
  constexpr std::size_t kMaxErrorLength = 512;
  if (detail.size() > kMaxErrorLength) {
    detail.resize(kMaxErrorLength);
    detail += "...";
  }
  for (auto& character : detail) {
    if (character == '\n' || character == '\r' || character == '\t') {
      character = ' ';
    }
  }
  return detail;
}

std::pair<std::string, std::string> JsonErrorFields(const Json& json) {
  if (!json.is_object()) {
    return {};
  }
  const Json* error = &json;
  if (const auto it = json.find("error"); it != json.end()) {
    if (it->is_string()) {
      return {SanitizeErrorDetail(it->get<std::string>()), {}};
    }
    error = &*it;
  } else if (const auto it = json.find("response"); it != json.end() &&
             it->is_object()) {
    if (const auto nested = it->find("error"); nested != it->end()) {
      error = &*nested;
    }
  }
  if (!error->is_object()) {
    return {};
  }
  std::string message;
  std::string code;
  if (const auto it = error->find("message"); it != error->end() &&
      it->is_string()) {
    message = it->get<std::string>();
  }
  if (const auto it = error->find("code"); it != error->end() &&
      it->is_string()) {
    code = it->get<std::string>();
  }
  if (message.empty()) {
    if (const auto it = error->find("type"); it != error->end() &&
        it->is_string()) {
      message = it->get<std::string>();
    }
  }
  if (message.empty()) {
    if (const auto it = error->find("detail"); it != error->end() &&
        it->is_string()) {
      message = it->get<std::string>();
    }
  }
  return {SanitizeErrorDetail(std::move(message)), SanitizeErrorDetail(std::move(code))};
}

std::pair<std::string, std::string> ErrorFieldsFromBody(
    const std::string& body) {
  if (body.empty() || body.size() > 64 * 1024) {
    return {};
  }
  try {
    const auto json = Json::parse(body);
    return JsonErrorFields(json);
  } catch (const std::exception&) {
    return {};
  }
}

std::string ExtractError(const HttpResponse& response) {
  if (!response.error.empty()) {
    return SanitizeErrorDetail(response.error);
  }
  const auto [message, code] = ErrorFieldsFromBody(response.body);
  std::string error = "HTTP " + std::to_string(response.status);
  if (!message.empty()) {
    error += ": " + message;
  }
  if (!code.empty()) {
    error += " (" + code + ")";
  }
  return error;
}

Json OpenAiChatPayload(const AppConfig& config,
                       const RewriteRequest& request,
                       int max_output_tokens) {
  auto payload =
      Json{{"model", config.model},
           {"messages",
            Json::array({Json{{"role", "system"},
                               {"content", config.system_prompt}},
                         Json{{"role", "user"}, {"content", request.input}}})},
           {"temperature", 0.2},
           {"max_tokens", max_output_tokens}};
  payload["reasoning_effort"] = config.reasoning;
  return payload;
}

Json OpenAiResponsesPayload(const AppConfig& config,
                            const RewriteRequest& request,
                            int max_output_tokens) {
  auto payload = Json{{"model", config.model},
                      {"instructions", config.system_prompt},
                      {"input", request.input},
                      {"temperature", 0.2},
                      {"max_output_tokens", max_output_tokens}};
  payload["reasoning"] = Json{{"effort", config.reasoning}};
  return payload;
}

Json CodexResponsesPayload(const AppConfig& config,
                           const RewriteRequest& request,
                           int /*max_output_tokens*/) {
  // The Codex endpoint accepts the Responses request shape used by the
  // official Codex client, not every field accepted by the public API.
  auto payload = Json{{"model", config.model},
                      {"instructions", config.system_prompt},
                      {"input", Json::array({Json{
                          {"type", "message"},
                          {"role", "user"},
                          {"content", Json::array({Json{
                              {"type", "input_text"},
                              {"text", request.input},
                          }})},
                      }})},
                      {"tool_choice", "auto"},
                      {"parallel_tool_calls", true}};
  payload["store"] = false;
  payload["stream"] = true;
  payload["text"] = Json{{"verbosity", "medium"}};
  payload["reasoning"] = Json{{"summary", "auto"},
                              {"effort", config.reasoning}};
  payload["include"] = Json::array({"reasoning.encrypted_content"});
  return payload;
}

Json AnthropicPayload(const AppConfig& config,
                      const RewriteRequest& request,
                      int max_output_tokens) {
  auto payload =
      Json{{"model", config.model},
           {"system", config.system_prompt},
           {"messages",
            Json::array({Json{{"role", "user"}, {"content", request.input}}})},
           {"temperature", 0.2},
           {"max_tokens", max_output_tokens}};
  if (config.reasoning != "none" && config.reasoning != "off") {
    payload["thinking"] = Json{{"type", "adaptive"}};
    payload["output_config"] = Json{{"effort", config.reasoning}};
  }
  return payload;
}

std::string ExtractText(ApiFormat format, const std::string& body) {
  const auto json = Json::parse(body);
  switch (format) {
    case ApiFormat::OpenAiChat:
      return json.at("choices").at(0).at("message").at("content")
          .get<std::string>();
    case ApiFormat::OpenAiResponses:
      if (json.contains("output_text")) {
        return json.at("output_text").get<std::string>();
      }
      if (json.contains("output")) {
        std::ostringstream text;
        for (const auto& item : json.at("output")) {
          if (!item.contains("content")) {
            continue;
          }
          for (const auto& content : item.at("content")) {
            if (content.value("type", "") == "output_text" &&
                content.contains("text")) {
              text << content.at("text").get<std::string>();
            }
          }
        }
        return text.str();
      }
      break;
    case ApiFormat::AnthropicMessages:
      if (json.contains("content")) {
        std::ostringstream text;
        for (const auto& item : json.at("content")) {
          if (item.value("type", "") == "text" && item.contains("text")) {
            text << item.at("text").get<std::string>();
          }
        }
        return text.str();
      }
      break;
  }
  return {};
}

struct ParsedCodexResponse {
  std::string text;
  std::string error;
  std::string error_code;
};

ParsedCodexResponse ExtractCodexResponse(const std::string& body) {
  ParsedCodexResponse result;
  std::string event_name;
  std::string data;

  const auto consume = [&]() {
    if (data.empty() || data == "[DONE]") {
      event_name.clear();
      data.clear();
      return;
    }
    const auto json = Json::parse(data, nullptr, false);
    if (json.is_discarded()) {
      event_name.clear();
      data.clear();
      return;
    }
    const auto type = json.value("type", event_name);
    if (type == "response.output_text.delta") {
      if (const auto it = json.find("delta"); it != json.end() &&
          it->is_string()) {
        result.text += it->get<std::string>();
      }
    } else if (type == "response.failed") {
      const auto [message, code] = JsonErrorFields(json);
      result.error = message.empty() ? "Codex response failed" : message;
      result.error_code = code;
    } else if (type == "response.completed" && result.text.empty()) {
      if (const auto response = json.find("response");
          response != json.end() && response->is_object()) {
        result.text = ExtractText(ApiFormat::OpenAiResponses, response->dump());
      }
    }
    event_name.clear();
    data.clear();
  };

  std::istringstream stream(body);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      consume();
    } else if (line.rfind("event:", 0) == 0) {
      event_name = line.substr(6);
      while (!event_name.empty() && event_name.front() == ' ') {
        event_name.erase(event_name.begin());
      }
    } else if (line.rfind("data:", 0) == 0) {
      if (!data.empty()) {
        data.push_back('\n');
      }
      data += line.substr(5);
      while (!data.empty() && data.front() == ' ') {
        data.erase(data.begin());
      }
    }
  }
  consume();
  return result;
}

std::string Endpoint(const AppConfig& config) {
  if (IsCodexProvider(config)) {
    return "https://chatgpt.com/backend-api/codex/responses";
  }
  const auto base_url = ProviderBaseUrl(config);
  switch (EffectiveApiFormat(config)) {
    case ApiFormat::OpenAiChat:
      return JoinUrl(base_url, "/chat/completions");
    case ApiFormat::OpenAiResponses:
      return JoinUrl(base_url, "/responses");
    case ApiFormat::AnthropicMessages:
      return JoinUrl(base_url, "/v1/messages");
  }
  return base_url;
}

Json Payload(const AppConfig& config,
             const RewriteRequest& request,
             int max_output_tokens) {
  Json payload;
  switch (EffectiveApiFormat(config)) {
    case ApiFormat::OpenAiChat:
      payload = OpenAiChatPayload(config, request, max_output_tokens);
      break;
    case ApiFormat::OpenAiResponses:
      payload = OpenAiResponsesPayload(config, request, max_output_tokens);
      break;
    case ApiFormat::AnthropicMessages:
      payload = AnthropicPayload(config, request, max_output_tokens);
      break;
  }
  if (IsCodexProvider(config)) {
    payload = CodexResponsesPayload(config, request, max_output_tokens);
  }
  if (IsCustomProvider(config) &&
      config.custom_provider.request_body.is_object()) {
    for (const auto& item : config.custom_provider.request_body.items()) {
      payload[item.key()] = item.value();
    }
  }
  return payload;
}

}  // namespace

namespace {

RewriteResult RewriteWithResolvedCredential(
    const AppConfig& config,
    const RewriteRequest& request,
    IHttpTransport& transport,
    const std::optional<ResolvedCredential>& credential,
    const CancellationToken* cancellation,
    IDiagnosticSink* diagnostics) {
  RewriteResult result;
  result.request_id = NewRequestId();
  result.estimated_input_tokens = EstimateTokens(request.input);
  result.max_output_tokens =
      ChooseMaxOutputTokens(config, result.estimated_input_tokens);

  const auto started = std::chrono::steady_clock::now();
  const auto endpoint = Endpoint(config);
  const auto finish = [&](RewriteResult value) {
    if (value.duration.count() == 0) {
      value.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started);
    }
    RecordDiagnostic(diagnostics, config, endpoint, value);
    return value;
  };

  if (request.input.empty()) {
    result.error = "input is empty";
    return finish(result);
  }
  if (config.model.empty()) {
    result.error = "config key 'model' is required";
    return finish(result);
  }
  if (IsCustomProvider(config) && config.base_url.empty()) {
    result.error = "config key 'base_url' is required for custom providers";
    return finish(result);
  }

  HttpRequest http_request;
  http_request.url =
      IsCustomProvider(config)
          ? AppendQueryParameters(Endpoint(config),
                                  config.custom_provider.query_parameters)
          : Endpoint(config);
  http_request.body = Payload(config, request, result.max_output_tokens).dump();
  http_request.headers = AuthHeaders(config, credential);
  http_request.request_id = result.request_id;
  http_request.timeout = config.timeout;
  http_request.cancellation_token = cancellation;

  const auto response = transport.PostJson(http_request);
  result.http_status = response.status;
  result.provider_request_id = ResponseHeader(response, "x-request-id");
  if (result.provider_request_id.empty()) {
    result.provider_request_id = ResponseHeader(response, "request-id");
  }
  result.duration = response.duration;
  if (cancellation && cancellation->IsCancelled()) {
    result.error = "rewrite was cancelled";
    return finish(result);
  }
  if (!response.error.empty() || response.status < 200 || response.status >= 300) {
    result.error = ExtractError(response);
    const auto [message, code] = ErrorFieldsFromBody(response.body);
    result.error_code = code;
    if (result.error_code.empty() && !message.empty()) {
      result.error_code = "provider_error";
    }
    return finish(result);
  }

  try {
    if (IsCodexProvider(config)) {
      const auto parsed = ExtractCodexResponse(response.body);
      result.text = parsed.text;
      result.error = parsed.error;
      result.error_code = parsed.error_code;
      if (result.text.empty() && result.error.empty() &&
          response.body.find("data:") == std::string::npos) {
        result.text = ExtractText(ApiFormat::OpenAiResponses, response.body);
      }
      if (!result.error.empty()) {
        return finish(result);
      }
    } else {
      result.text = ExtractText(EffectiveApiFormat(config), response.body);
    }
  } catch (const std::exception& error) {
    result.error = SanitizeErrorDetail(error.what());
    return finish(result);
  }

  if (result.text.empty()) {
    result.error = "LLM returned empty output";
    return finish(result);
  }

  result.ok = true;
  return finish(result);
}

}  // namespace

RewriteResult RewriteWithLlm(const AppConfig& config,
                             const RewriteRequest& request,
                             IHttpTransport& transport,
                             IDiagnosticSink* diagnostics) {
  // This compatibility overload is intentionally environment-only.  Normal
  // workflows use the resolver overload below, which also supports refresh,
  // native storage, and external Codex credentials.
  std::optional<ResolvedCredential> credential;
  const auto env_name = CanonicalEnvironmentName(config);
  auto value = EnvValue(env_name);
  if (value.empty()) value = config.credential;
  if (!value.empty()) {
    credential =
        ResolvedCredential{.access_token = value, .account_id = std::nullopt};
  }
  return RewriteWithResolvedCredential(config, request, transport, credential,
                                       nullptr, diagnostics);
}

RewriteResult RewriteWithLlm(const AppConfig& config,
                             const RewriteRequest& request,
                             IHttpTransport& transport,
                             CredentialResolver& credentials,
                             const CancellationToken* cancellation,
                             IDiagnosticSink* diagnostics) {
  RewriteResult result;
  result.estimated_input_tokens = EstimateTokens(request.input);
  result.max_output_tokens =
      ChooseMaxOutputTokens(config, result.estimated_input_tokens);
  if (request.input.empty()) {
    result.error = "input is empty";
    return result;
  }
  if (config.model.empty()) {
    result.error = "config key 'model' is required";
    return result;
  }
  const auto resolved =
      credentials.Resolve(config.provider, config.credential, cancellation);
  if (!resolved.ok()) {
    result.error = resolved.error.Message();
    return result;
  }
  if (cancellation && cancellation->IsCancelled()) {
    result.error = "credential operation was cancelled";
    return result;
  }
  return RewriteWithResolvedCredential(config, request, transport,
                                       resolved.credential, cancellation,
                                       diagnostics);
}

std::string BuildLlmPayloadForTest(const AppConfig& config,
                                   const RewriteRequest& request,
                                   int max_output_tokens) {
  return Payload(config, request, max_output_tokens).dump();
}

}  // namespace llm_rewriter
