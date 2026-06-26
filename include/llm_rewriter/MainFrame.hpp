#pragma once

#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/Paths.hpp"

#include <string>

#include <wx/activityindicator.h>
#include <wx/button.h>
#include <wx/frame.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace llm_rewriter {

class MainFrame final : public wxFrame {
 public:
  MainFrame(AppConfig config, UserPaths paths, std::string original_text);

 private:
  void OnRewrite(wxCommandEvent& event);
  void OnCopyRewrite(wxCommandEvent& event);
  void OnCopyOriginal(wxCommandEvent& event);
  void OnCancel(wxCommandEvent& event);
  void OnClose(wxCloseEvent& event);
  void SetRefining(bool refining);

  AppConfig config_;
  UserPaths paths_;
  wxTextCtrl* original_;
  wxTextCtrl* rewrite_;
  wxActivityIndicator* activity_;
  wxStaticText* status_;
  wxButton* rewrite_button_;
  wxButton* copy_rewrite_button_;
  wxButton* copy_original_button_;
  wxButton* cancel_button_;
  bool refining_ = false;
};

}  // namespace llm_rewriter
