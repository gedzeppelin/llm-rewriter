#include "llm_rewriter/abi/backend.h"

int main(void) {
  return llmr_abi_version() == LLM_REWRITER_ABI_VERSION ? 0 : 1;
}
