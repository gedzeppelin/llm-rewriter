#include "llm_rewriter/abi/backend.h"

#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/LlmClient.hpp"

#include <exception>
#include <filesystem>
#include <string>
#include <vector>

struct llmr_context {
  llm_rewriter::AppConfig config;
  llmr_http_post_json_fn post_json = nullptr;
  void* transport_user_data = nullptr;
  std::string last_error;
};

struct llmr_rewrite_result {
  llm_rewriter::RewriteResult result;
};

namespace {

llm_rewriter::AppConfig DefaultConfig() {
  llm_rewriter::AppConfig config;
  config.system_prompt = llm_rewriter::DefaultSystemPrompt();
  return config;
}

int StoreString(std::string& target, const char* value) {
  if (value == nullptr) {
    return 1;
  }
  target = value;
  return 0;
}

const char* StringOrEmpty(const std::string& value) {
  return value.empty() ? "" : value.c_str();
}

class CallbackHttpTransport final : public llm_rewriter::IHttpTransport {
 public:
  CallbackHttpTransport(llmr_http_post_json_fn post_json, void* user_data)
      : post_json_(post_json), user_data_(user_data) {}

  llm_rewriter::HttpResponse PostJson(
      const llm_rewriter::HttpRequest& request) override {
    std::vector<const char*> headers;
    headers.reserve(request.headers.size());
    for (const auto& header : request.headers) {
      headers.push_back(header.c_str());
    }

    const llmr_http_request abi_request{
        .url = request.url.c_str(),
        .body = request.body.c_str(),
        .headers = headers.data(),
        .header_count = static_cast<uint64_t>(headers.size()),
        .timeout_ms = request.timeout.count(),
    };
    llmr_http_response abi_response{};
    if (post_json_(user_data_, &abi_request, &abi_response) != 0) {
      return {.status = 0,
              .body = {},
              .error = "HTTP transport callback failed"};
    }
    return {
        .status = abi_response.status,
        .body = abi_response.body != nullptr ? abi_response.body : "",
        .error = abi_response.error != nullptr ? abi_response.error : "",
    };
  }

 private:
  llmr_http_post_json_fn post_json_;
  void* user_data_;
};

class MissingHttpTransport final : public llm_rewriter::IHttpTransport {
 public:
  llm_rewriter::HttpResponse PostJson(
      const llm_rewriter::HttpRequest&) override {
    return {.status = 0,
            .body = {},
            .error = "HTTP transport is not configured"};
  }
};

}  // namespace

extern "C" {

uint32_t llmr_abi_version(void) {
  return LLM_REWRITER_ABI_VERSION;
}

llmr_context* llmr_context_create(const char* config_path) {
  try {
    auto* context = new llmr_context;
    if (config_path != nullptr && *config_path != '\0') {
      context->config =
          llm_rewriter::LoadConfig(std::filesystem::path{config_path});
    } else {
      context->config = DefaultConfig();
    }
    return context;
  } catch (const std::exception&) {
    return nullptr;
  }
}

void llmr_context_destroy(llmr_context* context) {
  delete context;
}

int llmr_ensure_default_config_file(const char* config_path) {
  if (config_path == nullptr || *config_path == '\0') {
    return 1;
  }
  try {
    const std::filesystem::path path{config_path};
    if (std::filesystem::exists(path)) {
      return 0;
    }
    return llm_rewriter::EnsureDefaultConfigFile(path) ? 0 : 1;
  } catch (const std::exception&) {
    return 1;
  }
}

int llmr_config_set_value(const char* config_path,
                          const char* key,
                          const char* value) {
  if (config_path == nullptr || key == nullptr || value == nullptr) {
    return 1;
  }
  try {
    return llm_rewriter::SetConfigValue(std::filesystem::path{config_path}, key,
                                        value)
               ? 0
               : 1;
  } catch (const std::exception&) {
    return 1;
  }
}

int llmr_context_set_model(llmr_context* context, const char* model) {
  if (context == nullptr) {
    return 1;
  }
  return StoreString(context->config.model, model);
}

int llmr_context_set_reasoning(llmr_context* context, const char* reasoning) {
  if (context == nullptr) {
    return 1;
  }
  return StoreString(context->config.reasoning, reasoning);
}

int llmr_context_set_http_transport(llmr_context* context,
                                    llmr_http_post_json_fn post_json,
                                    void* user_data) {
  if (context == nullptr || post_json == nullptr) {
    return 1;
  }
  context->post_json = post_json;
  context->transport_user_data = user_data;
  return 0;
}

const char* llmr_context_last_error(const llmr_context* context) {
  if (context == nullptr) {
    return "context is null";
  }
  return StringOrEmpty(context->last_error);
}

llmr_rewrite_result* llmr_rewrite(llmr_context* context, const char* input) {
  if (context == nullptr) {
    return nullptr;
  }
  try {
    auto* output = new llmr_rewrite_result;
    if (context->post_json != nullptr) {
      CallbackHttpTransport transport(context->post_json,
                                      context->transport_user_data);
      output->result =
          llm_rewriter::RewriteWithLlm(
              context->config, {.input = input != nullptr ? input : ""},
              transport);
    } else {
      MissingHttpTransport transport;
      output->result =
          llm_rewriter::RewriteWithLlm(
              context->config, {.input = input != nullptr ? input : ""},
              transport);
    }
    context->last_error = output->result.error;
    return output;
  } catch (const std::exception& error) {
    context->last_error = error.what();
    return nullptr;
  }
}

void llmr_result_destroy(llmr_rewrite_result* result) {
  delete result;
}

int llmr_result_ok(const llmr_rewrite_result* result) {
  return result != nullptr && result->result.ok ? 1 : 0;
}

const char* llmr_result_text(const llmr_rewrite_result* result) {
  if (result == nullptr) {
    return "";
  }
  return StringOrEmpty(result->result.text);
}

const char* llmr_result_error(const llmr_rewrite_result* result) {
  if (result == nullptr) {
    return "result is null";
  }
  return StringOrEmpty(result->result.error);
}

int llmr_result_estimated_input_tokens(const llmr_rewrite_result* result) {
  return result == nullptr ? 0 : result->result.estimated_input_tokens;
}

int llmr_result_max_output_tokens(const llmr_rewrite_result* result) {
  return result == nullptr ? 0 : result->result.max_output_tokens;
}

}  // extern "C"
