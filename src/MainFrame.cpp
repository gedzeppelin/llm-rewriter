#include "llm_rewriter/MainFrame.hpp"

#include "llm_rewriter/AppIdentity.hpp"
#include "llm_rewriter/Clipboard.hpp"
#include "llm_rewriter/RewriteService.hpp"

#include <thread>

#include <wx/app.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/stattext.h>

namespace llm_rewriter {

MainFrame::MainFrame(AppConfig config, UserPaths paths, std::string original_text)
    : wxFrame(nullptr, wxID_ANY, wxString::FromUTF8(kApplicationName), wxDefaultPosition,
              wxSize{900, 650}),
      config_(std::move(config)),
      paths_(std::move(paths)) {
  auto* panel = new wxPanel(this);
  auto* root = new wxBoxSizer(wxVERTICAL);

  auto* original_label = new wxStaticText(panel, wxID_ANY, "Original");
  original_ = new wxTextCtrl(panel, wxID_ANY, wxString::FromUTF8(original_text),
                             wxDefaultPosition, wxDefaultSize,
                             wxTE_MULTILINE | wxTE_RICH2);

  auto* rewrite_label = new wxStaticText(panel, wxID_ANY, "Rewrite");
  rewrite_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition,
                            wxDefaultSize, wxTE_MULTILINE | wxTE_RICH2);

  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  rewrite_button_ = new wxButton(panel, wxID_ANY, "Rewrite");
  copy_rewrite_button_ = new wxButton(panel, wxID_ANY, "Copy Rewrite");
  copy_original_button_ = new wxButton(panel, wxID_ANY, "Copy Original");
  cancel_button_ = new wxButton(panel, wxID_ANY, "Cancel");
  activity_ = new wxActivityIndicator(panel);
  status_ = new wxStaticText(panel, wxID_ANY, "");

  buttons->Add(rewrite_button_, 0, wxRIGHT, 8);
  buttons->Add(copy_rewrite_button_, 0, wxRIGHT, 8);
  buttons->Add(copy_original_button_, 0, wxRIGHT, 8);
  buttons->Add(activity_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  buttons->Add(status_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  buttons->AddStretchSpacer();
  buttons->Add(cancel_button_, 0);

  root->Add(original_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
  root->Add(original_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
  root->Add(rewrite_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
  root->Add(rewrite_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
  root->Add(buttons, 0, wxEXPAND | wxALL, 12);
  panel->SetSizer(root);

  rewrite_button_->Bind(wxEVT_BUTTON, &MainFrame::OnRewrite, this);
  copy_rewrite_button_->Bind(wxEVT_BUTTON, &MainFrame::OnCopyRewrite, this);
  copy_original_button_->Bind(wxEVT_BUTTON, &MainFrame::OnCopyOriginal, this);
  cancel_button_->Bind(wxEVT_BUTTON, &MainFrame::OnCancel, this);
  Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
  copy_rewrite_button_->Enable(false);
}

void MainFrame::OnRewrite(wxCommandEvent& event) {
  (void)event;
  const auto original = original_->GetValue().ToStdString();
  const auto config = config_;
  const auto paths = paths_;
  SetRefining(true);
  rewrite_->Clear();

  std::thread([this, config, paths, original]() {
    auto result = RewriteAndRecord(config, paths, {.input = original},
                                   RewriteContext::Ui);
    wxTheApp->CallAfter([this, result = std::move(result)]() mutable {
      SetRefining(false);
      if (!result.ok) {
        rewrite_->Clear();
        wxMessageBox(wxString::FromUTF8(result.error), "Rewrite failed",
                     wxOK | wxICON_ERROR, this);
        return;
      }
      rewrite_->SetValue(wxString::FromUTF8(result.text));
      copy_rewrite_button_->Enable(true);
    });
  }).detach();
}

void MainFrame::OnCopyRewrite(wxCommandEvent& event) {
  (void)event;
  WriteClipboardText(rewrite_->GetValue().ToStdString());
}

void MainFrame::OnCopyOriginal(wxCommandEvent& event) {
  (void)event;
  WriteClipboardText(original_->GetValue().ToStdString());
}

void MainFrame::OnCancel(wxCommandEvent& event) {
  (void)event;
  Close();
}

void MainFrame::OnClose(wxCloseEvent& event) {
  if (refining_) {
    event.Veto();
    return;
  }
  event.Skip();
}

void MainFrame::SetRefining(bool refining) {
  refining_ = refining;
  original_->Enable(!refining);
  rewrite_button_->Enable(!refining);
  copy_rewrite_button_->Enable(!refining && !rewrite_->GetValue().empty());
  copy_original_button_->Enable(!refining);
  cancel_button_->Enable(!refining);
  if (refining) {
    status_->SetLabel("Rewriting...");
    activity_->Start();
  } else {
    status_->SetLabel("");
    activity_->Stop();
  }
}

}  // namespace llm_rewriter
