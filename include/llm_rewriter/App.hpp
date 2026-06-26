#pragma once

#include <wx/app.h>

namespace llm_rewriter {

class App final : public wxApp {
 public:
  bool OnInit() override;
};

}  // namespace llm_rewriter
