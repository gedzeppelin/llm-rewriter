#pragma once

#include <string>

namespace llm_rewriter {

enum class NotificationKind {
  Started,
  Succeeded,
  Failed,
};

void Notify(NotificationKind kind, const std::string& detail = {});

}  // namespace llm_rewriter
