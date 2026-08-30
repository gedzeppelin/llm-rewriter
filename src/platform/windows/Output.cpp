#include "llm_rewriter/Output.hpp"

#include "llm_rewriter/Clipboard.hpp"

namespace llm_rewriter {

bool WriteOutput(OutputMode mode,
                 const CliOptions&,
                 const std::string& text,
                 std::string& message) {
  switch (mode) {
    case OutputMode::Stdout:
      message.clear();
      return true;
    case OutputMode::Clipboard:
    case OutputMode::Paste:
      if (WriteClipboardText(text)) {
        message = mode == OutputMode::Paste
                      ? "copied rewrite to clipboard; automatic paste is not "
                        "implemented on Windows"
                      : "copied rewrite to clipboard";
        return true;
      }
      message = "failed to write clipboard";
      return false;
    case OutputMode::Type:
      if (WriteClipboardText(text)) {
        message = "automatic typing is not implemented on Windows; copied "
                  "rewrite to clipboard instead";
        return true;
      }
      message = "automatic typing is not implemented on Windows";
      return false;
    case OutputMode::Preview:
      message = "preview output requires a graphical frontend";
      return false;
  }
  message = "unsupported output mode";
  return false;
}

}  // namespace llm_rewriter
