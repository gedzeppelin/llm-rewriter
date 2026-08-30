#include "llm_rewriter/RewriteService.hpp"

#include "CurlHttpTransport.hpp"
#include "llm_rewriter/Diagnostics.hpp"
#include "llm_rewriter/History.hpp"
#include "llm_rewriter/Notification.hpp"

namespace llm_rewriter {
namespace {

bool NotificationsEnabled(const AppConfig& config, RewriteContext context) {
  switch (config.notification_mode) {
    case NotificationMode::Off:
      return false;
    case NotificationMode::Cli:
      return context == RewriteContext::Cli;
    case NotificationMode::Always:
      return true;
  }
  return false;
}

bool ShouldNotify(const AppConfig& config,
                  RewriteContext context,
                  NotificationKind kind) {
  if (!NotificationsEnabled(config, context)) {
    return false;
  }
  switch (config.notification_events) {
    case NotificationEvents::Errors:
      return kind == NotificationKind::Failed;
    case NotificationEvents::Completion:
      return kind == NotificationKind::Succeeded ||
             kind == NotificationKind::Failed;
    case NotificationEvents::All:
      return true;
  }
  return false;
}

void MaybeNotify(const AppConfig& config,
                 RewriteContext context,
                 NotificationKind kind,
                 const std::string& detail = {}) {
  if (ShouldNotify(config, context, kind)) {
    Notify(kind, detail);
  }
}

}  // namespace

RewriteResult RewriteAndRecord(const AppConfig& config,
                               const UserPaths& paths,
                               const RewriteRequest& request,
                               RewriteContext context) {
  CurlHttpTransport transport;
  CredentialDependencies dependencies;
  dependencies.store = CreatePlatformCredentialStore();
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
  MaybeNotify(config, context, NotificationKind::Started);
  auto result = RewriteWithLlm(config, request, transport, credentials,
                               nullptr, &diagnostics);
  AppendHistory(paths.history_file, config, request.input, result);
  if (result.ok) {
    MaybeNotify(config, context, NotificationKind::Succeeded);
  } else {
    MaybeNotify(config, context, NotificationKind::Failed, result.error);
  }
  return result;
}

bool ShouldNotifyForTest(const AppConfig& config,
                         RewriteContext context,
                         NotificationKind kind) {
  return ShouldNotify(config, context, kind);
}

}  // namespace llm_rewriter
