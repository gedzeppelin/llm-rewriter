#include "llm_rewriter/LlmClient.hpp"

#include "llm_rewriter/TokenEstimate.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace llm_rewriter {
namespace {

using Json = nlohmann::json;

struct HttpResponse {
  long status = 0;
  std::string body;
  std::string error;
};

size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string JoinUrl(std::string base_url, const std::string& path) {
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  return base_url + path;
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

HttpResponse PostJson(const AppConfig& config,
                      const std::string& url,
                      const Json& payload,
                      const std::vector<std::string>& extra_headers) {
  HttpResponse response;
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                           curl_easy_cleanup);
  if (!curl) {
    response.error = "failed to initialize libcurl";
    return response;
  }

  const auto body = payload.dump();
  curl_slist* raw_headers = nullptr;
  raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
  for (const auto& header : extra_headers) {
    raw_headers = curl_slist_append(raw_headers, header.c_str());
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      raw_headers, curl_slist_free_all);

  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteBody);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                   static_cast<long>(config.timeout.count()));
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

  const auto code = curl_easy_perform(curl.get());
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
  if (code != CURLE_OK) {
    response.error = curl_easy_strerror(code);
  }
  return response;
}

std::vector<std::string> AuthHeaders(const AppConfig& config) {
  std::vector<std::string> headers;
  auto token = config.api_key;
  if (token.empty()) {
    token = EnvValue(config.api_key_env);
  }
  if (token.empty()) {
    token = EnvValue(config.bearer_token_env);
  }
  if (!token.empty()) {
    headers.push_back("Authorization: Bearer " + token);
  }
  if (config.api_format == ApiFormat::AnthropicMessages) {
    headers.push_back("x-api-key: " + token);
    headers.push_back("anthropic-version: 2023-06-01");
  }
  return headers;
}

std::string ExtractError(const HttpResponse& response) {
  if (!response.error.empty()) {
    return response.error;
  }
  if (response.body.empty()) {
    return "HTTP " + std::to_string(response.status);
  }
  try {
    const auto json = Json::parse(response.body);
    if (json.contains("error")) {
      if (json["error"].is_string()) {
        return json["error"].get<std::string>();
      }
      if (json["error"].contains("message")) {
        return json["error"]["message"].get<std::string>();
      }
    }
  } catch (...) {
  }
  return "HTTP " + std::to_string(response.status) + ": " + response.body;
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
  if (config.reasoning_effort != "off") {
    payload["reasoning_effort"] = config.reasoning_effort;
  }
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
  if (config.reasoning_effort != "off") {
    payload["reasoning"] = Json{{"effort", config.reasoning_effort}};
  }
  return payload;
}

Json AnthropicPayload(const AppConfig& config,
                      const RewriteRequest& request,
                      int max_output_tokens) {
  return Json{{"model", config.model},
              {"system", config.system_prompt},
              {"messages",
               Json::array({Json{{"role", "user"},
                                  {"content", request.input}}})},
              {"temperature", 0.2},
              {"max_tokens", max_output_tokens}};
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

std::string Endpoint(const AppConfig& config) {
  switch (config.api_format) {
    case ApiFormat::OpenAiChat:
      return JoinUrl(config.base_url, "/chat/completions");
    case ApiFormat::OpenAiResponses:
      return JoinUrl(config.base_url, "/responses");
    case ApiFormat::AnthropicMessages:
      return JoinUrl(config.base_url, "/v1/messages");
  }
  return config.base_url;
}

Json Payload(const AppConfig& config,
             const RewriteRequest& request,
             int max_output_tokens) {
  switch (config.api_format) {
    case ApiFormat::OpenAiChat:
      return OpenAiChatPayload(config, request, max_output_tokens);
    case ApiFormat::OpenAiResponses:
      return OpenAiResponsesPayload(config, request, max_output_tokens);
    case ApiFormat::AnthropicMessages:
      return AnthropicPayload(config, request, max_output_tokens);
  }
  return {};
}

}  // namespace

RewriteResult RewriteWithLlm(const AppConfig& config,
                             const RewriteRequest& request) {
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

  const auto response =
      PostJson(config, Endpoint(config),
               Payload(config, request, result.max_output_tokens),
               AuthHeaders(config));
  if (!response.error.empty() || response.status < 200 || response.status >= 300) {
    result.error = ExtractError(response);
    return result;
  }

  try {
    result.text = ExtractText(config.api_format, response.body);
  } catch (const std::exception& error) {
    result.error = error.what();
    return result;
  }

  if (result.text.empty()) {
    result.error = "LLM returned empty output";
    return result;
  }

  result.ok = true;
  return result;
}

std::string BuildLlmPayloadForTest(const AppConfig& config,
                                   const RewriteRequest& request,
                                   int max_output_tokens) {
  return Payload(config, request, max_output_tokens).dump();
}

}  // namespace llm_rewriter
