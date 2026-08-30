#pragma once

#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/LlmClient.hpp"
#include "llm_rewriter/Notification.hpp"
#include "llm_rewriter/Paths.hpp"

namespace llm_rewriter {

enum class RewriteContext {
  Cli,
  Ui,
};

RewriteResult RewriteAndRecord(const AppConfig& config,
                               const UserPaths& paths,
                               const RewriteRequest& request,
                               RewriteContext context = RewriteContext::Cli);

RewriteResult RewriteAndRecord(const AppConfig& config,
                               const UserPaths& paths,
                               const RewriteRequest& request,
                               CredentialResolver& credentials,
                               IHttpTransport& transport,
                               RewriteContext context = RewriteContext::Cli);

bool ShouldNotifyForTest(const AppConfig& config,
                         RewriteContext context,
                         NotificationKind kind);

}  // namespace llm_rewriter
