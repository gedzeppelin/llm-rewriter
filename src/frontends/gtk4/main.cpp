#include "llm_rewriter/AppIdentity.hpp"
#include "llm_rewriter/Cli.hpp"
#include "llm_rewriter/Clipboard.hpp"
#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/Credentials.hpp"
#include "llm_rewriter/Doctor.hpp"
#include "llm_rewriter/Paths.hpp"
#include "llm_rewriter/RewriteService.hpp"

#include <peel/Adw/Adw.h>
#include <peel/GLib/functions.h>
#include <peel/Gdk/Clipboard.h>
#include <peel/Gio/Gio.h>
#include <peel/Gtk/Gtk.h>

#include <nlohmann/json.hpp>

#include <iostream>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
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
  std::string key;
  std::string previous_value;
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
  Adw::ComboRow *provider_row = nullptr;
  Adw::ComboRow *api_format_row = nullptr;
  Adw::ComboRow *input_row = nullptr;
  Gtk::Widget *base_url_row = nullptr;
  Gtk::Widget *credential_row = nullptr;
  Gtk::Widget *codex_auth_file_row = nullptr;
  Gtk::Widget *custom_options_row = nullptr;
  Gtk::TextView *system_prompt_view = nullptr;
  llm_rewriter::UserPaths paths;
  llm_rewriter::AppConfig config;
  llm_rewriter::InputMode input = llm_rewriter::InputMode::Clipboard;
  bool syncing_ui = false;
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
  state->rewrite_button->set_sensitive(!refining);
  state->copy_original_button->set_sensitive(!refining);
  state->copy_rewrite_button->set_sensitive(!refining);
  if (refining) {
    state->spinner->start();
  } else {
    state->spinner->stop();
  }
}

void SetEntryText(AppState *state, Gtk::Editable *editable,
                  const std::string &text) {
  state->syncing_ui = true;
  editable->set_text(text.c_str());
  state->syncing_ui = false;
}

std::string SelectedInputValue(Adw::ComboRow *row) {
  switch (row->get_selected()) {
  case 1:
    return "primary";
  case 2:
    return "stdin";
  default:
    return "clipboard";
  }
}

llm_rewriter::InputMode ParseInputValue(const std::string &value) {
  if (value == "primary") {
    return llm_rewriter::InputMode::Primary;
  }
  if (value == "stdin") {
    return llm_rewriter::InputMode::Stdin;
  }
  return llm_rewriter::InputMode::Clipboard;
}

void SetSelectedInput(AppState *state, const std::string &value) {
  state->syncing_ui = true;
  if (value == "primary") {
    state->input_row->set_selected(1);
  } else if (value == "stdin") {
    state->input_row->set_selected(2);
  } else {
    state->input_row->set_selected(0);
  }
  state->syncing_ui = false;
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

void ApplyReasoning(llm_rewriter::AppConfig &config, const std::string &value) {
  config.reasoning = value.empty() ? "none" : value;
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

void SetSelectedProvider(AppState *state, const std::string &value) {
  state->syncing_ui = true;
  unsigned selected = 0;
  if (value == "anthropic") selected = 1;
  else if (value == "gemini") selected = 2;
  else if (value == "openrouter") selected = 3;
  else if (value == "codex") selected = 4;
  else if (value == "custom") selected = 5;
  state->provider_row->set_selected(selected);
  state->syncing_ui = false;
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

void UpdateProviderFields(AppState *state) {
  const auto provider = state->config.provider;
  const bool custom = provider == "custom";
  const bool codex = provider == "codex";
  state->base_url_row->set_visible(custom);
  state->api_format_row->set_visible(custom);
  state->custom_options_row->set_visible(custom);
  state->credential_row->set_visible(!codex);
  state->codex_auth_file_row->set_visible(codex);
}

bool SyncConfigValue(AppState *state, const std::string &key,
                     const std::string &value);

void OnProviderChanged(AppState *state) {
  if (state->syncing_ui) return;
  const auto previous = state->config.provider;
  const auto value = SelectedProviderValue(state->provider_row);
  if (!SyncConfigValue(state, "provider", value)) {
    SetSelectedProvider(state, previous);
    return;
  }
  ApplyProvider(state->config, value);
  UpdateProviderFields(state);
}

void OnApiFormatChanged(AppState *state) {
  if (state->syncing_ui) return;
  const auto value = SelectedApiFormatValue(state->api_format_row);
  if (!SyncConfigValue(state, "api_format", value)) {
    SetSelectedApiFormat(state, state->config.api_format);
    return;
  }
  ApplyApiFormat(state->config, value);
}

bool SyncConfigValue(AppState *state, const std::string &key,
                     const std::string &value) {
  if (llm_rewriter::SetConfigValue(state->paths.config_file, key, value)) {
    return true;
  }
  ShowError(state, "Setting not saved",
            "The new value was rejected and the previous value was restored.");
  return false;
}

void OnEntryChanged(TextBinding *binding) {
  AppState *state = binding->state;
  if (state->syncing_ui) {
    return;
  }

  const std::string value = binding->editable->get_text();
  if (!SyncConfigValue(state, binding->key, value)) {
    SetEntryText(state, binding->editable, binding->previous_value);
    return;
  }

  binding->previous_value = value;
  if (binding->apply != nullptr) {
    binding->apply(state->config, value);
  }
}

void OnEntryApplied(TextBinding *binding) {
  AppState *state = binding->state;
  if (state->syncing_ui) {
    return;
  }

  const std::string value = binding->editable->get_text();
  if (!SyncConfigValue(state, binding->key, value)) {
    SetEntryText(state, binding->editable, binding->previous_value);
    return;
  }

  binding->previous_value = value;
  if (binding->apply != nullptr) {
    binding->apply(state->config, value);
  }
}

void OnInputChanged(AppState *state) {
  if (state->syncing_ui) {
    return;
  }

  const auto next = SelectedInputValue(state->input_row);
  const auto previous = state->config.input_mode;
  if (!SyncConfigValue(state, "input", next)) {
    SetSelectedInput(state, previous);
    return;
  }

  state->config.input_mode = next;
  state->input = ParseInputValue(next);
}

void RewriteDone(RewriteTask *task) {
  AppState *state = task->state;
  SetRefining(state, false);

  if (!task->result.ok) {
    ShowError(state, "Rewrite failed", task->result.error);
  } else {
    SetTextViewText(state->rewrite, task->result.text);
  }

  delete task;
}

void OnRewrite(AppState *state) {
  auto *task = new RewriteTask;
  task->state = state;
  task->paths = state->paths;
  task->config = state->config;
  task->input = TextViewText(state->original).c_str();
  SetTextViewText(state->rewrite, "");
  SetRefining(state, true);

  std::thread([task] {
    task->result = llm_rewriter::RewriteAndRecord(
        task->config, task->paths, {.input = task->input},
        llm_rewriter::RewriteContext::Ui);
    GLib::idle_add_once([task] { RewriteDone(task); });
  }).detach();
}

void CopyText(Gtk::TextView *source, Gtk::Widget *window) {
  const auto text = TextViewText(source);
  window->get_clipboard()->set_text(text.c_str() != nullptr ? text.c_str()
                                                            : "");
}

void OnSaveSystemPrompt(AppState *state) {
  const auto text = TextViewText(state->system_prompt_view);
  const std::string system_prompt = text.c_str() != nullptr ? text.c_str() : "";
  if (!SyncConfigValue(state, "system_prompt", system_prompt)) {
    SetTextViewText(state->system_prompt_view, state->config.system_prompt);
    return;
  }
  state->config.system_prompt = system_prompt;
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

void BindEntry(AppState *state, Gtk::Editable *editable, std::string key,
               std::string current_value,
               void (*apply)(llm_rewriter::AppConfig &, const std::string &)) {
  auto binding = std::make_unique<TextBinding>();
  binding->state = state;
  binding->editable = editable;
  binding->key = std::move(key);
  binding->previous_value = std::move(current_value);
  binding->apply = apply;
  auto *raw_binding = binding.get();
  editable->connect_changed(
      [raw_binding](Gtk::Editable *) { OnEntryChanged(raw_binding); });
  state->bindings.push_back(std::move(binding));
}

void BindEntryOnApply(AppState *state, Adw::EntryRow *row, std::string key,
                      std::string current_value,
                      void (*apply)(llm_rewriter::AppConfig &, const std::string &)) {
  auto binding = std::make_unique<TextBinding>();
  binding->state = state;
  binding->editable = row->cast<Gtk::Editable>();
  binding->key = std::move(key);
  binding->previous_value = std::move(current_value);
  binding->apply = apply;
  auto *raw_binding = binding.get();
  row->connect_apply(
      [raw_binding](Adw::EntryRow *) { OnEntryApplied(raw_binding); });
  state->bindings.push_back(std::move(binding));
}

peel::FloatPtr<Adw::EntryRow>
MakeEntryRow(AppState *state, Adw::PreferencesGroup *group, const char *title,
             const std::string &key, const std::string &value,
             void (*apply)(llm_rewriter::AppConfig &, const std::string &)) {
  auto row = Adw::EntryRow::create();
  row->set_title(title);
  row->cast<Gtk::Editable>()->set_text(value.c_str());
  BindEntry(state, row->cast<Gtk::Editable>(), key, value, apply);
  group->add(row->cast<Gtk::Widget>());
  return row;
}

peel::FloatPtr<Adw::EntryRow>
MakeExpanderEntryRow(AppState *state, Adw::ExpanderRow *expander,
                     const char *title, const std::string &key,
                     const std::string &value,
                     void (*apply)(llm_rewriter::AppConfig &, const std::string &)) {
  auto row = Adw::EntryRow::create();
  row->set_title(title);
  row->cast<Gtk::Editable>()->set_text(value.c_str());
  row->set_show_apply_button(true);
  BindEntryOnApply(state, row, key, value, apply);
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

peel::FloatPtr<Adw::PreferencesPage> MakePreferencesPage(AppState *state) {
  auto page = Adw::PreferencesPage::create();
  page->set_title("Preferences");
  page->set_icon_name("preferences-system-symbolic");

  auto provider = Adw::PreferencesGroup::create();
  provider->set_title("Provider");

  const char *provider_values[] = {"OpenAI", "Anthropic", "Gemini",
                                   "OpenRouter", "Codex", "Custom", nullptr};
  auto provider_options = Gtk::StringList::create(provider_values);
  auto provider_row = Adw::ComboRow::create();
  state->provider_row = provider_row;
  provider_row->set_title("Provider");
  provider_row->set_subtitle("Choose a built-in service or configure a custom endpoint");
  provider_row->set_model(provider_options);
  SetSelectedProvider(state, state->config.provider);
  provider_row->connect_notify(
      Adw::ComboRow::prop_selected(),
      [state](peel::GObject::Object *, peel::GObject::ParamSpec *) {
        OnProviderChanged(state);
      });
  provider->add(provider_row->cast<Gtk::Widget>());

  auto model_row = MakeEntryRow(state, provider, "Model", "model",
                                state->config.model, ApplyModel);
  auto reasoning_row = MakeEntryRow(state, provider, "Reasoning", "reasoning",
                                    state->config.reasoning, ApplyReasoning);

  auto base_url_row = MakeEntryRow(state, provider, "Base URL", "base_url",
                                   state->config.base_url, ApplyBaseUrl);
  state->base_url_row = base_url_row->cast<Gtk::Widget>();

  const char *format_values[] = {"OpenAI Chat Completions", "OpenAI Responses",
                                 "Anthropic Messages", nullptr};
  auto format_options = Gtk::StringList::create(format_values);
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
      state, custom_options, "Headers", "custom_provider.headers",
      StringMapJson(state->config.custom_provider.headers), ApplyCustomHeaders);
  auto query_row = MakeExpanderEntryRow(
      state, custom_options, "Query Parameters", "custom_provider.query_parameters",
      StringMapJson(state->config.custom_provider.query_parameters),
      ApplyCustomQueryParameters);
  auto body_row = MakeExpanderEntryRow(
      state, custom_options, "Request Body", "custom_provider.request_body",
      state->config.custom_provider.request_body.dump(), ApplyCustomRequestBody);
  provider->add(custom_options->cast<Gtk::Widget>());
  page->add(provider);

  auto credentials = Adw::PreferencesGroup::create();
  credentials->set_title("Credentials");
  credentials->set_description(
      "Canonical environment variables are checked before this configured value.");
  auto credential_row = Adw::PasswordEntryRow::create();
  state->credential_row = credential_row->cast<Gtk::Widget>();
  credential_row->set_title("Credential");
  credential_row->cast<Gtk::Editable>()->set_text(state->config.credential.c_str());
  BindEntry(state, credential_row->cast<Gtk::Editable>(), "credential",
            state->config.credential, ApplyCredential);
  credentials->add(credential_row->cast<Gtk::Widget>());

  const auto auth_file =
      state->config.codex_auth_file
          ? state->config.codex_auth_file->string()
          : std::string{};
  auto auth_file_row = MakeEntryRow(
      state, credentials, "Codex Auth File", "codex_auth_file", auth_file,
      ApplyCodexAuthFile);
  state->codex_auth_file_row = auth_file_row->cast<Gtk::Widget>();
  page->add(credentials);

  auto workflow = Adw::PreferencesGroup::create();
  workflow->set_title("Workflow");
  const char *input_values[] = {"clipboard", "primary", "stdin", nullptr};
  auto input_options = Gtk::StringList::create(input_values);
  auto input_row = Adw::ComboRow::create();
  state->input_row = input_row;
  input_row->set_title("Input");
  input_row->set_model(input_options);
  SetSelectedInput(state, llm_rewriter::ToString(state->input));
  input_row->connect_notify(
      Adw::ComboRow::prop_selected(),
      [state](peel::GObject::Object *, peel::GObject::ParamSpec *) {
        OnInputChanged(state);
      });
  workflow->add(input_row->cast<Gtk::Widget>());
  page->add(workflow);

  auto prompt = Adw::PreferencesGroup::create();
  prompt->set_title("System Prompt");
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
      [state](Gtk::Button *) { OnSaveSystemPrompt(state); });
  save_box->set_center_widget(std::move(save_button).cast<Gtk::Widget>());
  save_group->add(
      std::move(save_box).cast<Gtk::Widget>().release_floating_ptr());
  page->add(save_group);

  (void)model_row;
  (void)reasoning_row;
  UpdateProviderFields(state);
  return page;
}

void Activate(Adw::Application *app, StartupOptions *options) {
  auto *state = new AppState;
  state->paths = options->paths;
  state->config = options->config;
  state->input = options->input;
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
  auto switcher = Adw::InlineViewSwitcher::create();
  switcher->set_stack(stack_ptr);
  switcher->set_display_mode(Adw::InlineViewSwitcher::DisplayMode::BOTH);
  header->set_show_start_title_buttons(false);
  header->set_show_end_title_buttons(false);
  header->set_title_widget(std::move(switcher).cast<Gtk::Widget>());
  toolbar->add_top_bar(std::move(header).cast<Gtk::Widget>());
  toolbar->set_content(std::move(stack).cast<Gtk::Widget>());
  state->window->set_content(std::move(toolbar).cast<Gtk::Widget>());

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

  controls->append(std::move(rewrite_button).cast<Gtk::Widget>());
  controls->append(std::move(copy_rewrite_button).cast<Gtk::Widget>());
  controls->append(std::move(copy_original_button).cast<Gtk::Widget>());
  controls->append(std::move(spinner).cast<Gtk::Widget>());
  rewrite_page->append(std::move(controls).cast<Gtk::Widget>());

  stack_ptr->add_titled_with_icon(rewrite_page->cast<Gtk::Widget>(), "rewrite",
                                  "Rewrite", "document-edit-symbolic");
  auto preferences_page = MakePreferencesPage(state);
  stack_ptr->add_titled_with_icon(preferences_page->cast<Gtk::Widget>(),
                                  "preferences", "Preferences",
                                  "preferences-system-symbolic");

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
