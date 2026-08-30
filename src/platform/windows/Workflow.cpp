#include "llm_rewriter/RewriteService.hpp"

#include "WinHttpTransport.hpp"
#include "llm_rewriter/Diagnostics.hpp"
#include "llm_rewriter/History.hpp"

namespace llm_rewriter {
namespace {

bool ShouldNotify(const AppConfig& config,
                  RewriteContext context,
                  NotificationKind kind) {
  if (config.notification_mode == NotificationMode::Off ||
      (config.notification_mode == NotificationMode::Cli &&
       context != RewriteContext::Cli)) {
    return false;
  }
  switch (config.notification_events) {
    case NotificationEvents::Errors:
      return kind == NotificationKind::Failed;
    case NotificationEvents::Completion:
      return kind != NotificationKind::Started;
    case NotificationEvents::All:
      return true;
  }
  return false;
}

}  // namespace

RewriteResult RewriteAndRecord(const AppConfig& config,
                               const UserPaths& paths,
                               const RewriteRequest& request,
                               RewriteContext context) {
  WinHttpTransport transport;
  CredentialDependencies dependencies;
  dependencies.http = &transport;
  CredentialResolver credentials(config.codex_auth_file, dependencies);
  return RewriteAndRecord(config, paths, request, credentials, transport,
                          context);
}

RewriteResult RewriteAndRecord(const AppConfig& config,
                               const UserPaths& paths,
                               const RewriteRequest& request,
                               CredentialResolver& credentials,
                               IHttpTransport& transport,
                               RewriteContext context) {
  JsonlDiagnosticSink diagnostics(paths.diagnostics_file);
  if (ShouldNotify(config, context, NotificationKind::Started)) {
    Notify(NotificationKind::Started);
  }
  auto result = RewriteWithLlm(config, request, transport, credentials,
                               nullptr, &diagnostics);
  AppendHistory(paths.history_file, config, request.input, result);
  const auto kind = result.ok ? NotificationKind::Succeeded
                              : NotificationKind::Failed;
  if (ShouldNotify(config, context, kind)) {
    Notify(kind, result.error);
  }
  return result;
}

bool ShouldNotifyForTest(const AppConfig& config,
                         RewriteContext context,
                         NotificationKind kind) {
  return ShouldNotify(config, context, kind);
}

}  // namespace llm_rewriter
