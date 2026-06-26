#include "llm_rewriter/Clipboard.hpp"

#include <string>

#if defined(__linux__)
#include <array>
#include <cstdio>
#endif

#include <wx/app.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/string.h>

namespace llm_rewriter {
namespace {

#if defined(__linux__)
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
#endif

std::string ReadExternalClipboard(bool primary) {
#if defined(__linux__)
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

  if (auto text = ReadCommand("wl-paste --no-newline 2>/dev/null"); !text.empty()) {
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
#else
  (void)primary;
  return {};
#endif
}

std::string ReadWxClipboard(bool primary) {
  if (wxTheClipboard == nullptr) {
    return {};
  }
  if (!wxTheClipboard->Open()) {
    return {};
  }

#if defined(__WXGTK__)
  wxTheClipboard->UsePrimarySelection(primary);
#else
  (void)primary;
#endif

  std::string text;
  if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
    wxTextDataObject data;
    if (wxTheClipboard->GetData(data)) {
      text = data.GetText().ToStdString();
    }
  }

#if defined(__WXGTK__)
  wxTheClipboard->UsePrimarySelection(false);
#endif
  wxTheClipboard->Close();
  return text;
}

std::string ReadClipboard(bool primary) {
  if (wxTheApp == nullptr) {
    return ReadExternalClipboard(primary);
  }
  if (auto text = ReadWxClipboard(primary); !text.empty()) {
    return text;
  }
  return ReadExternalClipboard(primary);
}

}  // namespace

std::string ReadClipboardText() {
  return ReadClipboard(false);
}

std::string ReadPrimarySelectionText() {
  return ReadClipboard(true);
}

bool WriteClipboardText(const std::string& text) {
  if (wxTheApp == nullptr || wxTheClipboard == nullptr) {
#if defined(__linux__)
    return RunWriteCommand("wl-copy 2>/dev/null", text) ||
           RunWriteCommand("xclip -selection clipboard -in 2>/dev/null", text) ||
           RunWriteCommand("xsel --clipboard --input 2>/dev/null", text);
#else
    return false;
#endif
  }
  if (!wxTheClipboard->Open()) {
#if defined(__linux__)
    return RunWriteCommand("wl-copy 2>/dev/null", text) ||
           RunWriteCommand("xclip -selection clipboard -in 2>/dev/null", text) ||
           RunWriteCommand("xsel --clipboard --input 2>/dev/null", text);
#else
    return false;
#endif
  }
  const bool ok =
      wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
  wxTheClipboard->Close();
  if (ok) {
    return true;
  }
#if defined(__linux__)
  return RunWriteCommand("wl-copy 2>/dev/null", text) ||
         RunWriteCommand("xclip -selection clipboard -in 2>/dev/null", text) ||
         RunWriteCommand("xsel --clipboard --input 2>/dev/null", text);
#else
  return false;
#endif
}

}  // namespace llm_rewriter
