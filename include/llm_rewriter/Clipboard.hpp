#pragma once

#include <string>

namespace llm_rewriter {

std::string ReadClipboardText();
std::string ReadPrimarySelectionText();
bool WriteClipboardText(const std::string& text);

}  // namespace llm_rewriter
