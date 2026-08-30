#include "llm_rewriter/Clipboard.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace llm_rewriter {
namespace {

std::string ReadCommand(const char* command) {
  std::array<char, 4096> buffer{};
  std::string output;
  FILE* pipe = popen(command, "r");
  if (!pipe) {
    return {};
  }
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output.append(buffer.data());
  }
  pclose(pipe);
  return output;
}

bool RunWriteCommand(const char* command, const std::string& text) {
  FILE* pipe = popen(command, "w");
  if (!pipe) {
    return false;
  }
  const auto written = fwrite(text.data(), 1, text.size(), pipe);
  const auto status = pclose(pipe);
  return written == text.size() && status == 0;
}

std::string ReadClipboard(bool primary) {
  if (primary) {
    if (auto text = ReadCommand("wl-paste --primary --no-newline 2>/dev/null");
        !text.empty()) {
      return text;
    }
    if (auto text = ReadCommand("wl-paste -p -n 2>/dev/null"); !text.empty()) {
      return text;
    }
    if (auto text = ReadCommand("xclip -selection primary -out 2>/dev/null");
        !text.empty()) {
      return text;
    }
    return ReadCommand("xsel --primary --output 2>/dev/null");
  }

  if (auto text = ReadCommand("wl-paste --no-newline 2>/dev/null");
      !text.empty()) {
    return text;
  }
  if (auto text = ReadCommand("wl-paste -n 2>/dev/null"); !text.empty()) {
    return text;
  }
  if (auto text = ReadCommand("xclip -selection clipboard -out 2>/dev/null");
      !text.empty()) {
    return text;
  }
  return ReadCommand("xsel --clipboard --output 2>/dev/null");
}

}  // namespace

std::string ReadClipboardText() {
  return ReadClipboard(false);
}

std::string ReadPrimarySelectionText() {
  return ReadClipboard(true);
}

bool WriteClipboardText(const std::string& text) {
  return RunWriteCommand("wl-copy 2>/dev/null", text) ||
         RunWriteCommand("xclip -selection clipboard -in 2>/dev/null", text) ||
         RunWriteCommand("xsel --clipboard --input 2>/dev/null", text);
}

}  // namespace llm_rewriter
