#include "llm_rewriter/AppIdentity.hpp"
#include "llm_rewriter/Cli.hpp"
#include "llm_rewriter/Clipboard.hpp"
#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/Credentials.hpp"
#include "llm_rewriter/Doctor.hpp"
#include "llm_rewriter/History.hpp"
#include "llm_rewriter/Paths.hpp"
#include "llm_rewriter/RewriteService.hpp"

#include <peel/Adw/Adw.h>
#include <peel/GLib/functions.h>
#include <peel/Gdk/Clipboard.h>
#include <peel/Gio/Gio.h>
#include <peel/Gtk/Gtk.h>

#include <nlohmann/json.hpp>

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Adw = peel::Adw;
namespace Gio = peel::Gio;
namespace GLib = peel::GLib;
namespace Gtk = peel::Gtk;

struct AppState;

struct TextBinding {
  AppState *state = nullptr;
  Gtk::Editable *editable = nullptr;
  void (*apply)(llm_rewriter::AppConfig &, const std::string &) = nullptr;
};

struct AppState {
  Adw::ApplicationWindow *window = nullptr;
  Gtk::TextView *original = nullptr;
  Gtk::TextView *rewrite = nullptr;
  Gtk::Button *rewrite_button = nullptr;
  Gtk::Button *copy_rewrite_button = nullptr;
  Gtk::Button *copy_original_button = nullptr;
  Gtk::Spinner *spinner = nullptr;
  Adw::ToastOverlay *toast_overlay = nullptr;
  Adw::ViewStack *stack = nullptr;
  Adw::OverlaySplitView *history_split = nullptr;
  Gtk::ListBox *history_list = nullptr;
  Gtk::SearchEntry *history_search = nullptr;
  Adw::ComboRow *provider_row = nullptr;
  Adw::ComboRow *api_format_row = nullptr;
  Adw::ComboRow *reasoning_row = nullptr;
  Adw::ComboRow *input_row = nullptr;
  Adw::ComboRow *output_row = nullptr;
  Adw::ComboRow *notification_mode_row = nullptr;
  Adw::ComboRow *notification_events_row = nullptr;
  Adw::ComboRow *paste_shortcut_row = nullptr;
  Adw::SpinRow *timeout_row = nullptr;
  Adw::SpinRow *min_output_tokens_row = nullptr;
  Adw::SpinRow *max_output_tokens_row = nullptr;
  Adw::SpinRow *output_token_multiplier_row = nullptr;
  Adw::SpinRow *output_token_padding_row = nullptr;
  Adw::SwitchRow *history_enabled_row = nullptr;
  Gtk::Widget *base_url_row = nullptr;
  Gtk::Widget *credential_row = nullptr;
  Gtk::Widget *codex_auth_file_row = nullptr;
  Gtk::Widget *custom_options_row = nullptr;
  Gtk::TextView *system_prompt_view = nullptr;
  llm_rewriter::UserPaths paths;
  llm_rewriter::AppConfig config;
  std::unique_ptr<llm_rewriter::HistorySearchIndex> history_index;
  std::vector<llm_rewriter::HistoryEntry> history_results;
  std::string history_query;
  std::size_t history_total_count = 0;
  std::vector<std::string> reasoning_values;
  bool syncing_ui = false;
  bool credential_changed = false;
  std::vector<std::unique_ptr<TextBinding>> bindings;
};

struct RewriteTask {
  AppState *state = nullptr;
  llm_rewriter::UserPaths paths;
  llm_rewriter::AppConfig config;
  std::string input;
  llm_rewriter::RewriteResult result;
};

struct StartupOptions {
  llm_rewriter::UserPaths paths;
  llm_rewriter::AppConfig config;
  llm_rewriter::InputMode input = llm_rewriter::InputMode::Clipboard;
};

std::string ReadStdin() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

std::string ReadInput(llm_rewriter::InputMode mode) {
  switch (mode) {
  case llm_rewriter::InputMode::Stdin:
    return ReadStdin();
  case llm_rewriter::InputMode::Primary:
    return llm_rewriter::ReadPrimarySelectionText();
  case llm_rewriter::InputMode::Clipboard:
    return llm_rewriter::ReadClipboardText();
  }
  return {};
}

peel::String TextViewText(Gtk::TextView *view) {
  Gtk::TextBuffer *buffer = view->get_buffer();
  Gtk::TextIter start;
  Gtk::TextIter end;
  buffer->get_bounds(&start, &end);
  return buffer->get_text(&start, &end, true);
}

void SetTextViewText(Gtk::TextView *view, const std::string &text) {
  view->get_buffer()->set_text(text.c_str(), -1);
}

void ShowError(AppState *state, const char *title, const std::string &message) {
  auto dialog = Adw::AlertDialog::create(title, message.c_str());
  dialog->present(state->window->cast<Gtk::Widget>());
}

void SetRefining(AppState *state, bool refining) {
  state->original->set_editable(!refining);
  state->rewrite->set_editable(!refining);
  state->rewrite_button->set_sensitive(!refining);
  state->copy_original_button->set_sensitive(!refining);
  state->copy_rewrite_button->set_sensitive(!refining);
  state->spinner->set_visible(refining);
  if (refining) {
    state->spinner->start();
  } else {
    state->spinner->stop();
  }
}

void ApplyProvider(llm_rewriter::AppConfig &config, const std::string &value) {
  config.provider = value;
}

void ApplyBaseUrl(llm_rewriter::AppConfig &config, const std::string &value) {
  config.base_url = value;
}

void ApplyApiFormat(llm_rewriter::AppConfig &config, const std::string &value) {
  if (const auto parsed = llm_rewriter::ParseApiFormat(value)) {
    config.api_format = *parsed;
  }
}

void ApplyModel(llm_rewriter::AppConfig &config, const std::string &value) {
  config.model = value;
}

void ApplyCredential(llm_rewriter::AppConfig &config, const std::string &value) {
  config.credential = value;
}

void ApplyCodexAuthFile(llm_rewriter::AppConfig &config,
                        const std::string &value) {
  config.codex_auth_file = value.empty()
                               ? std::nullopt
                               : std::optional<std::filesystem::path>{value};
}

void ApplyTimeout(llm_rewriter::AppConfig &config, double value) {
  config.timeout = std::chrono::milliseconds{
      static_cast<std::int64_t>(std::llround(value))};
}

void ApplyMinOutputTokens(llm_rewriter::AppConfig &config, double value) {
  config.min_output_tokens = static_cast<int>(std::llround(value));
}

void ApplyMaxOutputTokens(llm_rewriter::AppConfig &config, double value) {
  config.max_output_tokens_limit = static_cast<int>(std::llround(value));
}

void ApplyOutputTokenMultiplier(llm_rewriter::AppConfig &config, double value) {
  config.output_token_multiplier = value;
}

void ApplyOutputTokenPadding(llm_rewriter::AppConfig &config, double value) {
  config.output_token_padding = static_cast<int>(std::llround(value));
}

void ApplyCustomMap(std::map<std::string, std::string> &target,
                    const std::string &value) {
  const auto parsed = nlohmann::json::parse(value, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return;
  }
  target.clear();
  for (const auto &[key, item] : parsed.items()) {
    if (item.is_string()) {
      target[key] = item.get<std::string>();
    } else if (item.is_boolean() || item.is_number()) {
      target[key] = item.dump();
    }
  }
}

void ApplyCustomHeaders(llm_rewriter::AppConfig &config,
                        const std::string &value) {
  ApplyCustomMap(config.custom_provider.headers, value);
}

void ApplyCustomQueryParameters(llm_rewriter::AppConfig &config,
                                const std::string &value) {
  ApplyCustomMap(config.custom_provider.query_parameters, value);
}

void ApplyCustomRequestBody(llm_rewriter::AppConfig &config,
                            const std::string &value) {
  const auto parsed = nlohmann::json::parse(value, nullptr, false);
  if (!parsed.is_discarded() && parsed.is_object()) {
    config.custom_provider.request_body = parsed;
  }
}

std::string SelectedProviderValue(Adw::ComboRow *row) {
  switch (row->get_selected()) {
  case 1:
    return "anthropic";
  case 2:
    return "gemini";
  case 3:
    return "openrouter";
  case 4:
    return "codex";
  case 5:
    return "custom";
  default:
    return "openai";
  }
}

std::string SelectedApiFormatValue(Adw::ComboRow *row) {
  switch (row->get_selected()) {
  case 1:
    return "openai_responses";
  case 2:
    return "anthropic_messages";
  default:
    return "openai_chat";
  }
}

void SetSelectedApiFormat(AppState *state, llm_rewriter::ApiFormat format) {
  state->syncing_ui = true;
  unsigned selected = 0;
  if (format == llm_rewriter::ApiFormat::OpenAiResponses) selected = 1;
  else if (format == llm_rewriter::ApiFormat::AnthropicMessages) selected = 2;
  state->api_format_row->set_selected(selected);
  state->syncing_ui = false;
}

std::string SelectedValue(Adw::ComboRow *row,
                          const std::vector<std::string> &values) {
  const auto selected = row->get_selected();
  return selected < values.size() ? values[selected] : std::string{};
}

void SetSelectedValue(AppState *state, Adw::ComboRow *row,
                      const std::vector<std::string> &values,
                      const std::string &value) {
  state->syncing_ui = true;
  const auto it = std::find(values.begin(), values.end(), value);
  row->set_selected(it == values.end()
                        ? 0U
                        : static_cast<unsigned>(std::distance(values.begin(), it)));
  state->syncing_ui = false;
}

void ApplyReasoningValue(AppState *state) {
  state->config.reasoning = SelectedValue(state->reasoning_row,
                                          state->reasoning_values);
  if (state->config.reasoning.empty()) state->config.reasoning = "none";
}

void ApplyInputValue(AppState *state) {
  state->config.input_mode =
      SelectedValue(state->input_row, {"clipboard", "primary", "stdin"});
}

void ApplyOutputValue(AppState *state) {
#if defined(__linux__)
  const std::vector<std::string> values = {"preview", "clipboard", "stdout",
                                            "type", "paste"};
#else
  const std::vector<std::string> values = {"preview", "clipboard", "stdout"};
#endif
  state->config.output_mode = SelectedValue(state->output_row, values);
}

void UpdateOutputFields(AppState *state) {
  if (state->paste_shortcut_row == nullptr) return;
  state->paste_shortcut_row->set_visible(state->config.output_mode == "paste");
}

void ApplyNotificationModeValue(AppState *state) {
  state->config.notification_mode = llm_rewriter::ParseNotificationMode(
                                        SelectedValue(
                                            state->notification_mode_row,
                                            {"cli", "always", "off"}))
                                        .value_or(llm_rewriter::NotificationMode::Cli);
}

void ApplyNotificationEventsValue(AppState *state) {
  state->config.notification_events = llm_rewriter::ParseNotificationEvents(
                                          SelectedValue(
                                              state->notification_events_row,
                                              {"errors", "completion", "lifecycle"}))
                                          .value_or(llm_rewriter::NotificationEvents::All);
}

void ApplyPasteShortcutValue(AppState *state) {
  state->config.paste_shortcut = llm_rewriter::ParsePasteShortcut(
                                     SelectedValue(
                                         state->paste_shortcut_row,
                                         {"ctrl_v", "ctrl_shift_v", "shift_insert"}))
                                     .value_or(llm_rewriter::PasteShortcut::CtrlV);
}

void UpdateProviderFields(AppState *state) {
  const auto provider = state->config.provider;
  const bool custom = provider == "custom";
  const bool codex = provider == "codex";
  state->base_url_row->set_visible(custom);
  state->api_format_row->set_visible(custom);
  state->custom_options_row->set_visible(custom);
  state->credential_row->set_visible(!codex && !custom);
  state->codex_auth_file_row->set_visible(codex);
}

void OnProviderChanged(AppState *state) {
  if (state->syncing_ui) return;
  const auto value = SelectedProviderValue(state->provider_row);
  ApplyProvider(state->config, value);
  UpdateProviderFields(state);
}

void OnApiFormatChanged(AppState *state) {
  if (state->syncing_ui) return;
  const auto value = SelectedApiFormatValue(state->api_format_row);
  ApplyApiFormat(state->config, value);
}

void OnReasoningChanged(AppState *state) {
  if (!state->syncing_ui) ApplyReasoningValue(state);
}

void OnInputChanged(AppState *state) {
  if (!state->syncing_ui) ApplyInputValue(state);
}

void OnOutputChanged(AppState *state) {
  if (!state->syncing_ui) ApplyOutputValue(state);
  UpdateOutputFields(state);
}

void OnNotificationModeChanged(AppState *state) {
  if (!state->syncing_ui) ApplyNotificationModeValue(state);
}

void OnNotificationEventsChanged(AppState *state) {
  if (!state->syncing_ui) ApplyNotificationEventsValue(state);
}

void OnPasteShortcutChanged(AppState *state) {
  if (!state->syncing_ui) ApplyPasteShortcutValue(state);
}

void OnEntryChanged(TextBinding *binding) {
  AppState *state = binding->state;
  if (state->syncing_ui) {
    return;
  }

  const std::string value = binding->editable->get_text();
  if (binding->apply == ApplyCredential) {
    state->credential_changed = true;
  }
  if (binding->apply != nullptr) {
    binding->apply(state->config, value);
  }
}

std::string HistoryTime(std::int64_t timestamp_ms) {
  if (timestamp_ms <= 0) return "Unknown time";
  const auto point = std::chrono::system_clock::time_point{
      std::chrono::milliseconds{timestamp_ms}};
  const auto time = std::chrono::system_clock::to_time_t(point);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  std::ostringstream output;
  output << std::put_time(&local, "%Y-%m-%d %H:%M");
  return output.str();
}

std::string HistorySnippet(const std::string &text, std::size_t limit) {
  std::string snippet;
  snippet.reserve(std::min(limit, text.size()));
  for (const char character : text) {
    if (character == '\n' || character == '\r' || character == '\t') {
      if (!snippet.empty() && snippet.back() != ' ') snippet.push_back(' ');
    } else {
      snippet.push_back(character);
    }
    if (snippet.size() >= limit) break;
  }
  while (!snippet.empty() && snippet.back() == ' ') snippet.pop_back();
  if (snippet.size() == limit && limit >= 3) {
    snippet.resize(limit - 3);
    snippet += "...";
  }
  return snippet.empty() ? "(empty rewrite)" : snippet;
}

void SelectHistoryEntry(AppState *state,
                        const llm_rewriter::HistoryEntry &entry) {
  // History is a selector for the editor, not a second editor.  Keep one pair
  // of text views so edits and rewrites always have a single source of truth.
  SetTextViewText(state->original, entry.input);
  SetTextViewText(state->rewrite, entry.output);
  if (state->stack != nullptr) state->stack->set_visible_child_name("rewrite");
}

constexpr std::size_t kHistoryPageSize = 50;

void RenderHistoryPage(AppState *state) {
  if (state->history_list == nullptr || state->history_index == nullptr) return;

  state->history_list->remove_all();
  for (std::size_t index = 0; index < state->history_results.size(); ++index) {
    const auto &entry = state->history_results[index];
    auto row = Adw::ActionRow::create();
    row->set_title(HistorySnippet(entry.input, 72).c_str());
    std::string subtitle = HistoryTime(entry.timestamp_ms);
    if (!entry.provider.empty() || !entry.model.empty()) {
      subtitle += "  •  ";
      subtitle += entry.provider;
      if (!entry.model.empty()) {
        subtitle += " / ";
        subtitle += entry.model;
      }
    }
    if (!entry.ok) subtitle += "  •  Failed";
    row->set_subtitle(subtitle.c_str());
    row->set_subtitle_lines(2);
    row->set_activatable(true);
    row->connect_activated([state, index](Adw::ActionRow *) {
      if (index < state->history_results.size()) {
        SelectHistoryEntry(state, state->history_results[index]);
      }
    });
    state->history_list->append(row->cast<Gtk::Widget>());
  }

  if (state->history_results.size() < state->history_total_count) {
    auto more_row = Adw::ActionRow::create();
    more_row->set_title("More history");
    const auto remaining =
        state->history_total_count - state->history_results.size();
    const std::string remaining_text =
        std::to_string(remaining) + " more entr" +
        (remaining == 1 ? "y" : "ies") + " available";
    more_row->set_subtitle(remaining_text.c_str());
    auto more_button = Gtk::Button::create_with_label("Load more");
    more_button->add_css_class("flat");
    more_button->set_valign(Gtk::Align::CENTER);
    more_button->connect_clicked([state](Gtk::Button *) {
      if (state->history_index == nullptr) return;
      auto page = state->history_index->SearchPage(
          state->history_query, state->history_results.size(),
          kHistoryPageSize);
      state->history_total_count = page.total_matches;
      state->history_results.insert(
          state->history_results.end(),
          std::make_move_iterator(page.entries.begin()),
          std::make_move_iterator(page.entries.end()));
      RenderHistoryPage(state);
    });
    more_row->add_suffix(
        std::move(more_button).cast<Gtk::Widget>().release_floating_ptr());
    state->history_list->append(more_row->cast<Gtk::Widget>());
  }

  if (state->history_results.empty()) {
    auto empty_row = Adw::ActionRow::create();
    empty_row->set_title("No matching history");
    empty_row->set_subtitle("Try a different search term.");
    state->history_list->append(empty_row->cast<Gtk::Widget>());
  } else {
    if (auto *row = state->history_list->get_row_at_index(0)) {
      state->history_list->select_row(row);
    }
  }
}

void RenderHistory(AppState *state) {
  if (state->history_list == nullptr || state->history_index == nullptr) return;
  const char *query_text = state->history_search == nullptr
                               ? nullptr
                               : state->history_search->get_text();
  state->history_query =
      query_text == nullptr ? std::string{} : std::string{query_text};
  auto page = state->history_index->SearchPage(
      state->history_query, 0, kHistoryPageSize);
  state->history_total_count = page.total_matches;
  state->history_results = std::move(page.entries);
  RenderHistoryPage(state);
}

void RefreshHistory(AppState *state) {
  if (state->history_list == nullptr) return;
  state->history_index = std::make_unique<llm_rewriter::HistorySearchIndex>(
      llm_rewriter::LoadHistory(state->paths.history_file));
  RenderHistory(state);
}

void RewriteDone(RewriteTask *task) {
  AppState *state = task->state;
  SetRefining(state, false);

  if (!task->result.ok) {
    ShowError(state, "Rewrite failed", task->result.error);
  } else {
    SetTextViewText(state->rewrite, task->result.text);
  }

  RefreshHistory(state);

  delete task;
}

void OnRewrite(AppState *state) {
  auto *task = new RewriteTask;
  task->state = state;
  task->paths = state->paths;
  task->config = state->config;
  task->input = TextViewText(state->original).c_str();
  SetRefining(state, true);
  SetTextViewText(state->rewrite, "");

  std::thread([task] {
    task->result = llm_rewriter::RewriteAndRecord(
        task->config, task->paths, {.input = task->input},
        llm_rewriter::RewriteContext::Ui);
    GLib::idle_add_once([task] { RewriteDone(task); });
  }).detach();
}

void CopyText(Gtk::TextView *source, Gtk::Widget *window) {
  const auto text = TextViewText(source);
  const std::string value = text.c_str() != nullptr ? text.c_str() : "";
  // GDK's clipboard provider is owned by this process and disappears when
  // the window closes. Prefer the platform clipboard integration so the
  // copied rewrite remains available after the app exits; retain the GDK
  // path as a fallback when no native clipboard helper is available.
  if (!llm_rewriter::WriteClipboardText(value)) {
    window->get_clipboard()->set_text(value.c_str());
  }
}

bool ValidatePreferences(const llm_rewriter::AppConfig &config,
                         std::string &message) {
  if (!llm_rewriter::ParseCredentialProvider(config.provider)) {
    message = "Choose a supported provider.";
    return false;
  }
  if (!llm_rewriter::ParseApiFormat(llm_rewriter::ToString(config.api_format))) {
    message = "Choose a supported API format.";
    return false;
  }
  const auto valid_input = config.input_mode == "clipboard" ||
                           config.input_mode == "primary" ||
                           config.input_mode == "stdin";
  if (!valid_input) {
    message = "Input mode must be clipboard, primary, or stdin.";
    return false;
  }
  const auto valid_output =
#if defined(__linux__)
      config.output_mode == "preview" || config.output_mode == "clipboard" ||
      config.output_mode == "stdout" || config.output_mode == "type" ||
      config.output_mode == "paste";
#else
      config.output_mode == "preview" || config.output_mode == "clipboard" ||
      config.output_mode == "stdout";
#endif
  if (!valid_output) {
    message = "The selected output mode is not available on this platform.";
    return false;
  }
  if (config.timeout.count() <= 0 || config.min_output_tokens < 0 ||
      config.max_output_tokens_limit <= 0 ||
      config.output_token_multiplier <= 0.0 ||
      config.output_token_padding < 0) {
    message = "Generation limits and timeout must be positive values.";
    return false;
  }
  return true;
}

void OnSavePreferences(AppState *state) {
  const auto text = TextViewText(state->system_prompt_view);
  const std::string system_prompt = text.c_str() != nullptr ? text.c_str() : "";
  state->config.system_prompt = system_prompt;

  std::string validation_error;
  if (!ValidatePreferences(state->config, validation_error)) {
    ShowError(state, "Invalid settings", validation_error);
    return;
  }

  auto persisted = state->config;
  const auto provider = llm_rewriter::ParseCredentialProvider(
      state->config.provider);
  if (provider &&
      *provider != llm_rewriter::CredentialProvider::Codex &&
      *provider != llm_rewriter::CredentialProvider::Custom &&
      (state->credential_changed || !state->config.credential.empty())) {
    llm_rewriter::CredentialDependencies dependencies;
    dependencies.store = llm_rewriter::CreatePlatformCredentialStore();
    llm_rewriter::CredentialResolver resolver(std::nullopt, dependencies);
    const auto credential_error = state->config.credential.empty()
                                      ? resolver.Clear(state->config.provider)
                                      : resolver.ConfigureApiKey(
                                            state->config.provider,
                                            state->config.credential);
    if (credential_error) {
      ShowError(state, "Credential not saved", credential_error.Message());
      return;
    }
    // Secrets entered through the UI are owned by the platform store, never
    // by config.json.  Keep the in-memory model redacted after migration.
    persisted.credential.clear();
    state->config.credential.clear();
    state->syncing_ui = true;
    if (state->credential_row != nullptr) {
      state->credential_row->cast<Gtk::Editable>()->set_text("");
    }
    state->syncing_ui = false;
    state->credential_changed = false;
  }

  // Preferences never writes credentials.credential, including a legacy value
  // that was loaded for compatibility but was not edited in this session.
  persisted.credential.clear();

  if (!llm_rewriter::SaveConfig(state->paths.config_file, persisted)) {
    ShowError(state, "Settings not saved",
              "The settings could not be saved. Check the values and try again.");
    return;
  }
  state->toast_overlay->add_toast(Adw::Toast::create("Settings saved"));
}

peel::FloatPtr<Gtk::Widget> MakeHistorySidebar(AppState *state) {
  // Give the overlay its own compact navigation toolbar.  Because the split
  // view wraps the complete application toolbar, this header starts at the
  // same top edge as the main title bar instead of creating a second tier.
  auto sidebar_toolbar = Adw::ToolbarView::create();
  auto sidebar_header = Adw::HeaderBar::create();
  sidebar_header->set_show_start_title_buttons(false);
  sidebar_header->set_show_end_title_buttons(false);
  auto sidebar_title = Gtk::Label::create("History");
  sidebar_title->add_css_class("title");
  sidebar_header->set_title_widget(std::move(sidebar_title).cast<Gtk::Widget>());

  auto close_button =
      Gtk::Button::create_from_icon_name("window-close-symbolic");
  close_button->set_has_frame(false);
  close_button->set_tooltip_text("Close history");
  close_button->connect_clicked([state](Gtk::Button *) {
    if (state->history_split != nullptr) {
      state->history_split->set_show_sidebar(false);
    }
  });
  sidebar_header->pack_end(std::move(close_button).cast<Gtk::Widget>());
  sidebar_toolbar->add_top_bar(std::move(sidebar_header).cast<Gtk::Widget>());

  auto sidebar = Gtk::Box::create(Gtk::Orientation::VERTICAL, 12);
  sidebar->set_margin_top(12);
  sidebar->set_margin_bottom(12);
  sidebar->set_margin_start(12);
  sidebar->set_margin_end(12);

  auto search = Gtk::SearchEntry::create();
  state->history_search = search;
  search->set_placeholder_text("Search rewrite history");
  search->set_search_delay(150);
  search->connect_search_changed([state](Gtk::SearchEntry *) {
    RenderHistory(state);
  });
  sidebar->append(std::move(search).cast<Gtk::Widget>());

  auto list = Gtk::ListBox::create();
  state->history_list = list;
  list->set_selection_mode(Gtk::SelectionMode::SINGLE);
  list->set_activate_on_single_click(true);
  auto list_scroll = Gtk::ScrolledWindow::create();
  list_scroll->set_vexpand(true);
  list_scroll->set_child(std::move(list).cast<Gtk::Widget>());
  sidebar->append(std::move(list_scroll).cast<Gtk::Widget>());
  RefreshHistory(state);
  sidebar_toolbar->set_content(std::move(sidebar).cast<Gtk::Widget>());
  return std::move(sidebar_toolbar).cast<Gtk::Widget>();
}

void ToggleHistorySidebar(AppState *state) {
  if (state->history_split == nullptr) return;
  if (state->stack != nullptr) state->stack->set_visible_child_name("rewrite");
  const bool show_sidebar = !state->history_split->get_show_sidebar();
  state->history_split->set_show_sidebar(show_sidebar);
  if (show_sidebar) {
    if (state->history_index == nullptr) {
      RefreshHistory(state);
    } else {
      RenderHistory(state);
    }
    if (state->history_search != nullptr) state->history_search->grab_focus();
  }
}

peel::FloatPtr<Gtk::Box> MakeTextPage(peel::FloatPtr<Gtk::TextView> original,
                                      peel::FloatPtr<Gtk::TextView> rewrite) {
  auto root = Gtk::Box::create(Gtk::Orientation::VERTICAL, 12);
  root->set_margin_top(18);
  root->set_margin_bottom(18);
  root->set_margin_start(18);
  root->set_margin_end(18);

  auto original_label = Gtk::Label::create("Original");
  original_label->set_halign(Gtk::Align::START);
  root->append(std::move(original_label).cast<Gtk::Widget>());

  auto original_scroll = Gtk::ScrolledWindow::create();
  original_scroll->set_child(std::move(original).cast<Gtk::Widget>());
  original_scroll->set_vexpand(true);
  root->append(std::move(original_scroll).cast<Gtk::Widget>());

  auto rewrite_label = Gtk::Label::create("Rewrite");
  rewrite_label->set_halign(Gtk::Align::START);
  root->append(std::move(rewrite_label).cast<Gtk::Widget>());

  auto rewrite_scroll = Gtk::ScrolledWindow::create();
  rewrite_scroll->set_child(std::move(rewrite).cast<Gtk::Widget>());
  rewrite_scroll->set_vexpand(true);
  root->append(std::move(rewrite_scroll).cast<Gtk::Widget>());
  return root;
}

void BindEntry(AppState *state, Gtk::Editable *editable,
               void (*apply)(llm_rewriter::AppConfig &, const std::string &)) {
  auto binding = std::make_unique<TextBinding>();
  binding->state = state;
  binding->editable = editable;
  binding->apply = apply;
  auto *raw_binding = binding.get();
  editable->connect_changed(
      [raw_binding](Gtk::Editable *) { OnEntryChanged(raw_binding); });
  state->bindings.push_back(std::move(binding));
}

peel::FloatPtr<Adw::EntryRow>
MakeEntryRow(AppState *state, Adw::PreferencesGroup *group, const char *title,
             const std::string &value,
             void (*apply)(llm_rewriter::AppConfig &, const std::string &)) {
  auto row = Adw::EntryRow::create();
  row->set_title(title);
  row->cast<Gtk::Editable>()->set_text(value.c_str());
  BindEntry(state, row->cast<Gtk::Editable>(), apply);
  group->add(row->cast<Gtk::Widget>());
  return row;
}

peel::FloatPtr<Adw::EntryRow>
MakeExpanderEntryRow(AppState *state, Adw::ExpanderRow *expander,
                     const char *title, const std::string &value,
                     void (*apply)(llm_rewriter::AppConfig &, const std::string &)) {
  auto row = Adw::EntryRow::create();
  row->set_title(title);
  row->cast<Gtk::Editable>()->set_text(value.c_str());
  BindEntry(state, row->cast<Gtk::Editable>(), apply);
  expander->add_row(row->cast<Gtk::Widget>());
  return row;
}

std::string StringMapJson(const std::map<std::string, std::string> &values) {
  nlohmann::json object = nlohmann::json::object();
  for (const auto &[key, value] : values) {
    object[key] = value;
  }
  return object.dump();
}

peel::RefPtr<Gtk::StringList> MakeStringList(
    const std::vector<std::string> &values) {
  std::vector<const char *> raw;
  raw.reserve(values.size() + 1);
  for (const auto &value : values) raw.push_back(value.c_str());
  raw.push_back(nullptr);
  return Gtk::StringList::create(peel::StrvRef::adopt(
      const_cast<const char *const *>(raw.data())));
}

peel::FloatPtr<Adw::ComboRow> MakeComboRow(
    AppState *state, Adw::PreferencesGroup *group, const char *title,
    const char *subtitle, const std::vector<std::string> &values,
    const std::string &selected, void (*changed)(AppState *)) {
  auto row = Adw::ComboRow::create();
  row->set_title(title);
  if (subtitle != nullptr) row->set_subtitle(subtitle);
  auto model = MakeStringList(values);
  row->set_model(model);
  SetSelectedValue(state, row, values, selected);
  row->connect_notify(
      Adw::ComboRow::prop_selected(),
      [state, changed](peel::GObject::Object *, peel::GObject::ParamSpec *) {
        if (changed != nullptr) changed(state);
      });
  group->add(row->cast<Gtk::Widget>());
  return row;
}

peel::FloatPtr<Adw::SpinRow> MakeSpinRow(
    AppState *state, Adw::PreferencesGroup *group, const char *title,
    const char *subtitle,
    double minimum, double maximum, double step, double value, unsigned digits,
    void (*apply)(llm_rewriter::AppConfig &, double)) {
  auto row = Adw::SpinRow::create_with_range(minimum, maximum, step);
  row->set_title(title);
  if (subtitle != nullptr) row->set_subtitle(subtitle);
  row->set_digits(digits);
  row->set_value(value);
  auto *row_ptr = static_cast<Adw::SpinRow *>(row);
  row->connect_notify(
      Adw::SpinRow::prop_value(),
      [state, row_ptr, apply](peel::GObject::Object *, peel::GObject::ParamSpec *) {
        if (!state->syncing_ui && apply != nullptr) {
          apply(state->config, row_ptr->get_value());
        }
      });
  group->add(row->cast<Gtk::Widget>());
  return row;
}

peel::FloatPtr<Adw::SwitchRow> MakeSwitchRow(
    AppState *state, Adw::PreferencesGroup *group, const char *title,
    const char *subtitle, bool active) {
  auto row = Adw::SwitchRow::create();
  row->set_title(title);
  if (subtitle != nullptr) row->set_subtitle(subtitle);
  row->set_active(active);
  auto *row_ptr = static_cast<Adw::SwitchRow *>(row);
  row->connect_notify(
      Adw::SwitchRow::prop_active(),
      [state, row_ptr](peel::GObject::Object *, peel::GObject::ParamSpec *) {
        if (!state->syncing_ui) state->config.history_enabled = row_ptr->get_active();
      });
  group->add(row->cast<Gtk::Widget>());
  return row;
}

peel::FloatPtr<Adw::PreferencesPage> MakePreferencesPage(AppState *state) {
  auto page = Adw::PreferencesPage::create();
  page->set_title("Preferences");
  page->set_icon_name("preferences-system-symbolic");

  auto provider = Adw::PreferencesGroup::create();
  provider->set_title("Provider");

  const std::vector<std::string> provider_values = {
      "openai", "anthropic", "gemini", "openrouter", "codex", "custom"};
  auto provider_options = MakeStringList(
      {"OpenAI", "Anthropic", "Gemini", "OpenRouter", "Codex", "Custom"});
  auto provider_row = Adw::ComboRow::create();
  state->provider_row = provider_row;
  provider_row->set_title("Provider");
  provider_row->set_subtitle("Choose a built-in service or configure a custom endpoint");
  provider_row->set_model(provider_options);
  SetSelectedValue(state, provider_row, provider_values, state->config.provider);
  provider_row->connect_notify(
      Adw::ComboRow::prop_selected(),
      [state](peel::GObject::Object *, peel::GObject::ParamSpec *) {
        OnProviderChanged(state);
      });
  provider->add(provider_row->cast<Gtk::Widget>());

  MakeEntryRow(state, provider, "Model", state->config.model, ApplyModel);

  auto base_url_row = MakeEntryRow(state, provider, "Base URL", state->config.base_url,
                                   ApplyBaseUrl);
  state->base_url_row = base_url_row->cast<Gtk::Widget>();

  auto format_options = MakeStringList({"OpenAI Chat Completions",
                                        "OpenAI Responses",
                                        "Anthropic Messages"});
  auto api_format_row = Adw::ComboRow::create();
  state->api_format_row = api_format_row;
  api_format_row->set_title("API Format");
  api_format_row->set_subtitle("Request and response protocol used by the endpoint");
  api_format_row->set_model(format_options);
  SetSelectedApiFormat(state, state->config.api_format);
  api_format_row->connect_notify(
      Adw::ComboRow::prop_selected(),
      [state](peel::GObject::Object *, peel::GObject::ParamSpec *) {
        OnApiFormatChanged(state);
      });
  provider->add(api_format_row->cast<Gtk::Widget>());

  auto custom_options = Adw::ExpanderRow::create();
  state->custom_options_row = custom_options->cast<Gtk::Widget>();
  custom_options->set_title("Advanced request options");
  custom_options->set_subtitle(
      "Optional headers, query parameters, and request body (JSON)");
  custom_options->set_expanded(false);

  auto headers_row = MakeExpanderEntryRow(
      state, custom_options, "Headers",
      StringMapJson(state->config.custom_provider.headers), ApplyCustomHeaders);
  auto query_row = MakeExpanderEntryRow(
      state, custom_options, "Query Parameters",
      StringMapJson(state->config.custom_provider.query_parameters),
      ApplyCustomQueryParameters);
  auto body_row = MakeExpanderEntryRow(
      state, custom_options, "Request Body",
      state->config.custom_provider.request_body.dump(), ApplyCustomRequestBody);
  provider->add(custom_options->cast<Gtk::Widget>());
  page->add(provider);

  auto credentials = Adw::PreferencesGroup::create();
  credentials->set_title("Credentials");
  credentials->set_description(
      "Credentials are stored in the platform secret store. Environment values "
      "still take precedence.");
  auto credential_row = Adw::PasswordEntryRow::create();
  state->credential_row = credential_row->cast<Gtk::Widget>();
  credential_row->set_title("Credential");
  credential_row->cast<Gtk::Editable>()->set_text(state->config.credential.c_str());
  BindEntry(state, credential_row->cast<Gtk::Editable>(), ApplyCredential);
  credentials->add(credential_row->cast<Gtk::Widget>());

  const auto auth_file =
      state->config.codex_auth_file
          ? state->config.codex_auth_file->string()
          : std::string{};
  auto auth_file_row = MakeEntryRow(state, credentials, "Codex Auth File",
                                    auth_file, ApplyCodexAuthFile);
  state->codex_auth_file_row = auth_file_row->cast<Gtk::Widget>();
  page->add(credentials);

  auto generation = Adw::PreferencesGroup::create();
  generation->set_title("Generation");
  state->reasoning_values = {"none", "low", "medium", "high"};
  if (std::find(state->reasoning_values.begin(), state->reasoning_values.end(),
                state->config.reasoning) == state->reasoning_values.end()) {
    state->reasoning_values.push_back(state->config.reasoning);
  }
  auto reasoning_row = MakeComboRow(
      state, generation, "Reasoning", "Provider-specific reasoning effort",
      state->reasoning_values, state->config.reasoning, OnReasoningChanged);
  state->reasoning_row = reasoning_row;
  auto history_enabled = MakeSwitchRow(
      state, generation, "History enabled",
      "Keep prompt and rewrite text in the local JSONL history file.",
      state->config.history_enabled);
  state->history_enabled_row = history_enabled;
  auto timeout_row = MakeSpinRow(
      state, generation, "Request timeout (ms)",
      "Maximum time to wait for a provider response before cancelling.", 1.0,
      2147483647.0, 100.0,
      static_cast<double>(std::max<std::int64_t>(1, state->config.timeout.count())),
      0, ApplyTimeout);
  state->timeout_row = timeout_row;
  auto min_output_row = MakeSpinRow(
      state, generation, "Minimum output tokens",
      "Lower bound for the calculated output-token budget.", 0.0,
      2147483647.0, 1.0,
      static_cast<double>(std::max(0, state->config.min_output_tokens)), 0,
      ApplyMinOutputTokens);
  state->min_output_tokens_row = min_output_row;
  auto max_output_row = MakeSpinRow(
      state, generation, "Maximum output token limit",
      "Hard cap on output tokens requested from the provider.", 1.0,
      2147483647.0, 1.0,
      static_cast<double>(std::max(1, state->config.max_output_tokens_limit)), 0,
      ApplyMaxOutputTokens);
  state->max_output_tokens_row = max_output_row;
  auto multiplier_row = MakeSpinRow(
      state, generation, "Output token multiplier",
      "Scales the estimated input tokens before reasoning adjustments.", 0.1,
      100.0, 0.1,
      std::max(0.1, state->config.output_token_multiplier), 2,
      ApplyOutputTokenMultiplier);
  state->output_token_multiplier_row = multiplier_row;
  auto padding_row = MakeSpinRow(
      state, generation, "Output token padding",
      "Extra output tokens added after applying the multiplier.", 0.0,
      2147483647.0, 1.0,
      static_cast<double>(std::max(0, state->config.output_token_padding)), 0,
      ApplyOutputTokenPadding);
  state->output_token_padding_row = padding_row;
  page->add(generation);

  auto workflow = Adw::PreferencesGroup::create();
  workflow->set_title("Workflow");
  const std::vector<std::string> input_values = {"clipboard", "primary", "stdin"};
  auto input_row = MakeComboRow(state, workflow, "Input mode",
                                "Where the draft is read when the app starts.",
                                input_values, state->config.input_mode,
                                OnInputChanged);
  state->input_row = input_row;
#if defined(__linux__)
  const std::vector<std::string> output_values = {"preview", "clipboard", "stdout",
                                                   "type", "paste"};
#else
  const std::vector<std::string> output_values = {"preview", "clipboard", "stdout"};
#endif
  auto output_row = MakeComboRow(
      state, workflow, "Output mode",
      "How a headless rewrite is delivered; type and paste are Linux-only.",
      output_values, state->config.output_mode, OnOutputChanged);
  state->output_row = output_row;
  const std::vector<std::string> paste_values = {"ctrl_v", "ctrl_shift_v",
                                                  "shift_insert"};
  auto paste_row = MakeComboRow(
      state, workflow, "Paste shortcut", "Shortcut used by paste output.",
      paste_values, llm_rewriter::ToString(state->config.paste_shortcut),
      OnPasteShortcutChanged);
  state->paste_shortcut_row = paste_row;
  UpdateOutputFields(state);
  page->add(workflow);

  auto notifications = Adw::PreferencesGroup::create();
  notifications->set_title("Notifications");
  auto notification_mode = MakeComboRow(
      state, notifications, "Notification mode", "When desktop notifications are emitted.",
      {"cli", "always", "off"},
      llm_rewriter::ToString(state->config.notification_mode),
      OnNotificationModeChanged);
  state->notification_mode_row = notification_mode;
  auto notification_events = MakeComboRow(
      state, notifications, "Notification events", "Which rewrite lifecycle events are announced.",
      {"errors", "completion", "lifecycle"},
      llm_rewriter::ToString(state->config.notification_events),
      OnNotificationEventsChanged);
  state->notification_events_row = notification_events;
  page->add(notifications);

  auto prompt = Adw::PreferencesGroup::create();
  prompt->set_title("System Prompt");
  prompt->set_description("The prompt sent with each rewrite.");
  auto prompt_scroll = Gtk::ScrolledWindow::create();
  prompt_scroll->set_size_request(-1, 220);
  auto prompt_view = Gtk::TextView::create();
  state->system_prompt_view = prompt_view;
  prompt_view->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
  SetTextViewText(state->system_prompt_view, state->config.system_prompt);
  prompt_scroll->set_child(std::move(prompt_view).cast<Gtk::Widget>());
  prompt->add(prompt_scroll->cast<Gtk::Widget>());
  page->add(prompt);

  auto save_group = Adw::PreferencesGroup::create();
  auto save_box = Gtk::CenterBox::create();
  save_box->set_margin_top(8);
  save_box->set_margin_bottom(8);
  auto save_button = Gtk::Button::create_with_label("Save");
  save_button->add_css_class("suggested-action");
  save_button->set_halign(Gtk::Align::CENTER);
  save_button->set_valign(Gtk::Align::CENTER);
  save_button->connect_clicked(
      [state](Gtk::Button *) { OnSavePreferences(state); });
  save_box->set_center_widget(std::move(save_button).cast<Gtk::Widget>());
  save_group->add(
      std::move(save_box).cast<Gtk::Widget>().release_floating_ptr());
  page->add(save_group);

  UpdateProviderFields(state);
  return page;
}

void Activate(Adw::Application *app, StartupOptions *options) {
  auto *state = new AppState;
  state->paths = options->paths;
  state->config = options->config;
  const std::string input_text = ReadInput(options->input);

  app->get_style_manager()->set_color_scheme(Adw::ColorScheme::DEFAULT);

  state->window = Adw::ApplicationWindow::create(app);
  state->window->set_title(llm_rewriter::kApplicationName);
  state->window->set_default_size(llm_rewriter::kDefaultWindowWidth,
                                  llm_rewriter::kDefaultWindowHeight);

  auto toolbar = Adw::ToolbarView::create();
  auto header = Adw::HeaderBar::create();
  auto stack = Adw::ViewStack::create();
  Adw::ViewStack *stack_ptr = stack;
  state->stack = stack_ptr;
  auto switcher = Adw::ViewSwitcher::create();
  switcher->set_stack(stack_ptr);
  switcher->set_policy(Adw::ViewSwitcher::Policy::WIDE);
  header->set_show_start_title_buttons(false);
  header->set_show_end_title_buttons(false);
  header->set_title_widget(std::move(switcher).cast<Gtk::Widget>());
  auto search_button =
      Gtk::Button::create_from_icon_name("system-search-symbolic");
  search_button->set_has_frame(false);
  search_button->set_tooltip_text("Search rewrite history");
  search_button->connect_clicked(
      [state](Gtk::Button *) { ToggleHistorySidebar(state); });
  header->pack_start(std::move(search_button).cast<Gtk::Widget>());
  toolbar->add_top_bar(std::move(header).cast<Gtk::Widget>());
  toolbar->set_content(std::move(stack).cast<Gtk::Widget>());

  auto original = Gtk::TextView::create();
  state->original = original;
  state->original->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
  SetTextViewText(state->original, input_text);
  auto rewrite = Gtk::TextView::create();
  state->rewrite = rewrite;
  state->rewrite->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);

  auto rewrite_page = Gtk::Box::create(Gtk::Orientation::VERTICAL, 0);
  auto text_page = MakeTextPage(std::move(original), std::move(rewrite));
  rewrite_page->append(std::move(text_page).cast<Gtk::Widget>());

  auto controls = Gtk::Box::create(Gtk::Orientation::HORIZONTAL, 8);
  controls->set_margin_start(18);
  controls->set_margin_end(18);
  controls->set_margin_bottom(18);

  auto rewrite_button = Gtk::Button::create_with_label("Rewrite");
  state->rewrite_button = rewrite_button;
  rewrite_button->add_css_class("suggested-action");
  rewrite_button->connect_clicked([state](Gtk::Button *) { OnRewrite(state); });
  auto copy_rewrite_button = Gtk::Button::create_with_label("Copy Rewrite");
  state->copy_rewrite_button = copy_rewrite_button;
  copy_rewrite_button->connect_clicked([state](Gtk::Button *) {
    CopyText(state->rewrite, state->window->cast<Gtk::Widget>());
  });
  auto copy_original_button = Gtk::Button::create_with_label("Copy Original");
  state->copy_original_button = copy_original_button;
  copy_original_button->connect_clicked([state](Gtk::Button *) {
    CopyText(state->original, state->window->cast<Gtk::Widget>());
  });
  auto spinner = Gtk::Spinner::create();
  state->spinner = spinner;
  spinner->set_visible(false);

  controls->append(std::move(rewrite_button).cast<Gtk::Widget>());
  controls->append(std::move(spinner).cast<Gtk::Widget>());
  auto controls_spacer = Gtk::Box::create(Gtk::Orientation::HORIZONTAL, 0);
  controls_spacer->set_hexpand(true);
  controls->append(std::move(controls_spacer).cast<Gtk::Widget>());
  controls->append(std::move(copy_rewrite_button).cast<Gtk::Widget>());
  controls->append(std::move(copy_original_button).cast<Gtk::Widget>());
  rewrite_page->append(std::move(controls).cast<Gtk::Widget>());

  auto history_split = Adw::OverlaySplitView::create();
  state->history_split = history_split;
  stack_ptr->add_titled_with_icon(
      std::move(rewrite_page).cast<Gtk::Widget>().release_floating_ptr(),
      "rewrite", "Rewrite",
      "document-edit-symbolic");
  auto preferences_page = MakePreferencesPage(state);
  stack_ptr->add_titled_with_icon(preferences_page->cast<Gtk::Widget>(),
                                  "preferences", "Preferences",
                                  "preferences-system-symbolic");

  // Keep the split view outside the toolbar so its sidebar overlays the
  // title bar as one coherent surface.  When it is hidden, the toolbar and
  // view stack behave exactly as before.
  history_split->set_content(std::move(toolbar).cast<Gtk::Widget>());
  history_split->set_sidebar(MakeHistorySidebar(state));
  history_split->set_sidebar_width_fraction(0.32);
  history_split->set_show_sidebar(false);
  stack_ptr->connect_notify(
      Adw::ViewStack::prop_visible_child_name(),
      [state](peel::GObject::Object *, peel::GObject::ParamSpec *) {
        const char *visible_name = state->stack->get_visible_child_name();
        if (visible_name != nullptr &&
            std::string_view{visible_name} == "preferences" &&
            state->history_split != nullptr) {
          state->history_split->set_show_sidebar(false);
        }
      });

  auto toast_overlay = Adw::ToastOverlay::create();
  state->toast_overlay = toast_overlay;
  toast_overlay->set_child(
      std::move(history_split).cast<Gtk::Widget>().release_floating_ptr());
  state->window->set_content(std::move(toast_overlay).cast<Gtk::Widget>());

  state->window->present();
}

} // namespace

int main(int argc, char **argv) {
  const auto parsed = llm_rewriter::ParseCli(argc, argv);
  if (parsed.exit) {
    return parsed.exit_code;
  }
  if (!parsed.options) {
    return 1;
  }

  auto options = *parsed.options;
  auto paths = llm_rewriter::ResolveUserPaths();
  if (!options.config_path.empty()) {
    paths.config_file = llm_rewriter::ExpandUserPath(options.config_path);
  }
  llm_rewriter::EnsureDefaultConfigFile(paths.config_file);
  auto config = llm_rewriter::LoadConfig(paths.config_file);
  llm_rewriter::ApplyCliOverrides(config, options.model, options.reasoning);
  llm_rewriter::ApplyConfigDefaults(options, config);

  if (options.command == llm_rewriter::CliCommand::Doctor) {
    const auto report =
        llm_rewriter::RunDoctor(config, options, paths, options.doctor_live);
    std::cout << report.text;
    return report.ok ? 0 : 1;
  }

  StartupOptions startup{
      .paths = paths, .config = config, .input = options.input};

  GLib::set_prgname(llm_rewriter::kApplicationId);
  auto app =
      Adw::Application::create(nullptr, Gio::Application::Flags::NON_UNIQUE);
  Adw::Application *app_ptr = app;
  app->connect_activate(
      [app_ptr, &startup](Gio::Application *) { Activate(app_ptr, &startup); });
  return app->run(0, nullptr);
}
