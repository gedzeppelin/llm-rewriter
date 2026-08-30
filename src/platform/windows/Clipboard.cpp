#include "llm_rewriter/Clipboard.hpp"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>

namespace llm_rewriter {
namespace {

std::string Utf8(const wchar_t* value) {
  if (value == nullptr) {
    return {};
  }
  const int length = static_cast<int>(wcslen(value));
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0,
                                       nullptr, nullptr);
  std::string output(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, length, output.data(), size, nullptr,
                      nullptr);
  return output;
}

std::wstring Wide(const std::string& value) {
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      output.data(), size);
  return output;
}

}  // namespace

std::string ReadClipboardText() {
  if (!OpenClipboard(nullptr)) {
    return {};
  }
  const HANDLE data = GetClipboardData(CF_UNICODETEXT);
  const auto* text = data != nullptr
                         ? static_cast<const wchar_t*>(GlobalLock(data))
                         : nullptr;
  const auto output = Utf8(text);
  if (text != nullptr) {
    GlobalUnlock(data);
  }
  CloseClipboard();
  return output;
}

std::string ReadPrimarySelectionText() {
  return ReadClipboardText();
}

bool WriteClipboardText(const std::string& text) {
  const auto wide = Wide(text);
  const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (data == nullptr) {
    return false;
  }
  void* destination = GlobalLock(data);
  if (destination == nullptr) {
    GlobalFree(data);
    return false;
  }
  memcpy(destination, wide.c_str(), bytes);
  GlobalUnlock(data);
  if (!OpenClipboard(nullptr)) {
    GlobalFree(data);
    return false;
  }
  EmptyClipboard();
  const bool ok = SetClipboardData(CF_UNICODETEXT, data) != nullptr;
  CloseClipboard();
  if (!ok) {
    GlobalFree(data);
  }
  return ok;
}

}  // namespace llm_rewriter
