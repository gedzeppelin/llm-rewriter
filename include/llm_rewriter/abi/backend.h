#ifndef LLM_REWRITER_ABI_BACKEND_H
#define LLM_REWRITER_ABI_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LLM_REWRITER_ABI_VERSION 1u

typedef struct llmr_context llmr_context;
typedef struct llmr_rewrite_result llmr_rewrite_result;

typedef struct llmr_http_request {
  const char* url;
  const char* body;
  const char* const* headers;
  uint64_t header_count;
  int64_t timeout_ms;
} llmr_http_request;

typedef struct llmr_http_response {
  long status;
  const char* body;
  const char* error;
} llmr_http_response;

typedef int (*llmr_http_post_json_fn)(void* user_data,
                                     const llmr_http_request* request,
                                     llmr_http_response* response);

uint32_t llmr_abi_version(void);

llmr_context* llmr_context_create(const char* config_path);
void llmr_context_destroy(llmr_context* context);

int llmr_ensure_default_config_file(const char* config_path);
int llmr_config_set_value(const char* config_path,
                          const char* key,
                          const char* value);
int llmr_context_set_model(llmr_context* context, const char* model);
int llmr_context_set_reasoning(llmr_context* context, const char* reasoning);
int llmr_context_set_http_transport(llmr_context* context,
                                    llmr_http_post_json_fn post_json,
                                    void* user_data);
const char* llmr_context_last_error(const llmr_context* context);

llmr_rewrite_result* llmr_rewrite(llmr_context* context, const char* input);
void llmr_result_destroy(llmr_rewrite_result* result);

int llmr_result_ok(const llmr_rewrite_result* result);
const char* llmr_result_text(const llmr_rewrite_result* result);
const char* llmr_result_error(const llmr_rewrite_result* result);
int llmr_result_estimated_input_tokens(const llmr_rewrite_result* result);
int llmr_result_max_output_tokens(const llmr_rewrite_result* result);

#ifdef __cplusplus
}
#endif

#endif
