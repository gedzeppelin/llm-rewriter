#include "llm_rewriter/Notification.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

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

void SilenceChildOutput() {
  const int dev_null = open("/dev/null", O_WRONLY);
  if (dev_null < 0) {
    return;
  }
  dup2(dev_null, STDOUT_FILENO);
  dup2(dev_null, STDERR_FILENO);
  close(dev_null);
}

void NotifyWithCommand(const std::string& title, const std::string& message) {
  const pid_t pid = fork();
  if (pid != 0) {
    return;
  }
  SilenceChildOutput();
  execlp("notify-send", "notify-send", "--app-name=LLM Rewriter",
         title.c_str(), message.c_str(), static_cast<char*>(nullptr));
  _exit(127);
}

}  // namespace

void Notify(NotificationKind kind, const std::string& detail) {
  NotifyWithCommand(Title(kind), Message(kind, detail));
}

}  // namespace llm_rewriter
