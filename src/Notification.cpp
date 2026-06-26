#include "llm_rewriter/Notification.hpp"

#include <cstdlib>
#include <sstream>
#include <string>

#include <wx/app.h>
#include <wx/notifmsg.h>
#include <wx/string.h>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace llm_rewriter {
namespace {

std::string Title(NotificationKind kind) {
  switch (kind) {
    case NotificationKind::Started:
      return "Rewrite started";
    case NotificationKind::Succeeded:
      return "Rewrite finished";
    case NotificationKind::Failed:
      return "Rewrite failed";
  }
  return "LLM Rewriter";
}

std::string Message(NotificationKind kind, const std::string& detail) {
  if (!detail.empty()) {
    return detail;
  }
  switch (kind) {
    case NotificationKind::Started:
      return "Rewrite started. Original text is kept until the request finishes.";
    case NotificationKind::Succeeded:
      return "Rewrite finished.";
    case NotificationKind::Failed:
      return "Rewrite failed. Original text was not changed.";
  }
  return {};
}

bool NotifyWithWx(const std::string& title, const std::string& message) {
  if (wxTheApp == nullptr) {
    return false;
  }
  wxNotificationMessage notification(wxString::FromUTF8(title),
                                     wxString::FromUTF8(message));
  return notification.Show();
}

#if defined(__linux__)
void SilenceChildOutput() {
  const int dev_null = open("/dev/null", O_WRONLY);
  if (dev_null < 0) {
    return;
  }
  dup2(dev_null, STDOUT_FILENO);
  dup2(dev_null, STDERR_FILENO);
  close(dev_null);
}

void NotifyWithNativeCommand(const std::string& title,
                             const std::string& message) {
  const pid_t pid = fork();
  if (pid != 0) {
    return;
  }
  SilenceChildOutput();
  execlp("notify-send", "notify-send", "--app-name=LLM Rewriter",
         title.c_str(), message.c_str(), static_cast<char*>(nullptr));
  _exit(127);
}
#elif defined(__APPLE__)
std::string AppleScriptString(const std::string& value) {
  std::ostringstream quoted;
  quoted << '"';
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      quoted << '\\';
    }
    quoted << c;
  }
  quoted << '"';
  return quoted.str();
}

void NotifyWithNativeCommand(const std::string& title,
                             const std::string& message) {
  const std::string script = "display notification " +
                             AppleScriptString(message) + " with title " +
                             AppleScriptString(title);
  const pid_t pid = fork();
  if (pid != 0) {
    return;
  }
  const int dev_null = open("/dev/null", O_WRONLY);
  if (dev_null >= 0) {
    dup2(dev_null, STDOUT_FILENO);
    dup2(dev_null, STDERR_FILENO);
    close(dev_null);
  }
  execlp("osascript", "osascript", "-e", script.c_str(),
         static_cast<char*>(nullptr));
  _exit(127);
}
#else
void NotifyWithNativeCommand(const std::string&, const std::string&) {}
#endif

}  // namespace

void Notify(NotificationKind kind, const std::string& detail) {
  const auto title = Title(kind);
  const auto message = Message(kind, detail);
  if (NotifyWithWx(title, message)) {
    return;
  }
  NotifyWithNativeCommand(title, message);
}

}  // namespace llm_rewriter
