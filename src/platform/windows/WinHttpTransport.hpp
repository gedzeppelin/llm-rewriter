#pragma once

#include "llm_rewriter/HttpTransport.hpp"

namespace llm_rewriter {

class WinHttpTransport final : public IHttpTransport {
 public:
  HttpResponse PostJson(const HttpRequest& request) override;
  HttpResponse Send(const HttpRequest& request) override;
};

}  // namespace llm_rewriter
