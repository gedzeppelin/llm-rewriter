#include "llm_rewriter/Output.hpp"

#include "llm_rewriter/Clipboard.hpp"

#include <cstdlib>
#include <string>

#if defined(__linux__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace llm_rewriter {
namespace {

bool EnvSet(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0';
}

bool CommandExists(const char* command) {
  std::string check = "command -v ";
  check += command;
  check += " >/dev/null 2>&1";
  return std::system(check.c_str()) == 0;
}

bool RunCommand(const std::string& command) {
  return std::system(command.c_str()) == 0;
}

#if defined(__linux__)
bool RunProgramWithText(const char* program,
                        const char* arg1,
                        const char* arg2,
                        const std::string& text) {
  const pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    if (arg2 != nullptr) {
      execlp(program, program, arg1, arg2, text.c_str(),
             static_cast<char*>(nullptr));
    } else if (arg1 != nullptr) {
      execlp(program, program, arg1, text.c_str(), static_cast<char*>(nullptr));
    } else {
      execlp(program, program, text.c_str(), static_cast<char*>(nullptr));
    }
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool SendCtrlCWayland() {
  return RunCommand("wtype -M ctrl -k c -m ctrl >/dev/null 2>&1");
}

bool SendCtrlCX11() {
  return RunCommand("xdotool key ctrl+c >/dev/null 2>&1");
}

bool SendPasteWayland(PasteShortcut shortcut) {
  switch (shortcut) {
    case PasteShortcut::CtrlV:
      return RunCommand("wtype -M ctrl -k v -m ctrl >/dev/null 2>&1");
    case PasteShortcut::CtrlShiftV:
      return RunCommand(
          "wtype -M ctrl -M shift -k v -m shift -m ctrl >/dev/null 2>&1");
    case PasteShortcut::ShiftInsert:
      return RunCommand("wtype -M shift -k insert -m shift >/dev/null 2>&1");
  }
  return false;
}

bool SendPasteX11(PasteShortcut shortcut) {
  switch (shortcut) {
    case PasteShortcut::CtrlV:
      return RunCommand("xdotool key ctrl+v >/dev/null 2>&1");
    case PasteShortcut::CtrlShiftV:
      return RunCommand("xdotool key ctrl+shift+v >/dev/null 2>&1");
    case PasteShortcut::ShiftInsert:
      return RunCommand("xdotool key shift+Insert >/dev/null 2>&1");
  }
  return false;
}

bool TypeWayland(const std::string& text) {
  return RunProgramWithText("wtype", nullptr, nullptr, text);
}

bool TypeX11(const std::string& text) {
  return RunProgramWithText("xdotool", "type", "--clearmodifiers", text);
}

bool FallbackClipboard(const std::string& text, std::string& message) {
  if (WriteClipboardText(text)) {
    message = "Linux synthetic input unavailable or failed; copied rewrite to "
              "clipboard instead.";
    return true;
  }
  message = "Linux synthetic input failed, and clipboard fallback also failed.";
  return false;
}
#endif

}  // namespace

bool WriteOutput(OutputMode mode,
                 const CliOptions& options,
                 const std::string& text,
                 std::string& message) {
  switch (mode) {
    case OutputMode::Preview:
      message = "preview output is handled by the UI";
      return false;
    case OutputMode::Stdout:
      message.clear();
      return true;
    case OutputMode::Clipboard:
      if (WriteClipboardText(text)) {
        message = "copied rewrite to clipboard";
        return true;
      }
      message = "failed to write clipboard";
      return false;
#if defined(__linux__)
    case OutputMode::Type: {
      const bool wayland = EnvSet("WAYLAND_DISPLAY");
      const bool x11 = EnvSet("DISPLAY");

      if (options.ctrl_c_before_output) {
        if (wayland && CommandExists("wtype")) {
          SendCtrlCWayland();
        } else if (x11 && CommandExists("xdotool")) {
          SendCtrlCX11();
        }
      }

      if (wayland && CommandExists("wtype") && TypeWayland(text)) {
        message = "typed rewrite with wtype";
        return true;
      }
      if (x11 && CommandExists("xdotool") && TypeX11(text)) {
        message = "typed rewrite with xdotool";
        return true;
      }

      return FallbackClipboard(text, message);
    }
    case OutputMode::Paste: {
      const bool wayland = EnvSet("WAYLAND_DISPLAY");
      const bool x11 = EnvSet("DISPLAY");

      if (options.ctrl_c_before_output) {
        if (wayland && CommandExists("wtype")) {
          SendCtrlCWayland();
        } else if (x11 && CommandExists("xdotool")) {
          SendCtrlCX11();
        }
      }

      if (!WriteClipboardText(text)) {
        message = "failed to write clipboard before paste";
        return false;
      }

      if (wayland && CommandExists("wtype") &&
          SendPasteWayland(options.paste_shortcut)) {
        message = "copied rewrite and pasted with wtype";
        return true;
      }
      if (x11 && CommandExists("xdotool") &&
          SendPasteX11(options.paste_shortcut)) {
        message = "copied rewrite and pasted with xdotool";
        return true;
      }

      message = "paste helper unavailable or failed; copied rewrite to "
                "clipboard instead.";
      return true;
    }
#endif
  }
  message = "unsupported output mode";
  return false;
}

}  // namespace llm_rewriter
