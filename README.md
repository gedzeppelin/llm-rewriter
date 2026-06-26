# llm-rewriter

Linux utility for rewriting drafts with an LLM while preserving the original text.

## Build

Arch Linux:

```sh
sudo pacman -S --needed \
  base-devel \
  cmake \
  ninja \
  pkgconf \
  gcc \
  wxwidgets-gtk3 \
  curl \
  nlohmann-json \
  cli11 \
  doctest \
  wl-clipboard \
  wtype \
  xclip \
  xsel
```

APT-based distributions:

```sh
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  libwxgtk3.2-dev \
  libcurl4-openssl-dev \
  nlohmann-json3-dev \
  libcli11-dev \
  doctest-dev \
  wl-clipboard \
  wtype \
  xclip \
  xsel \
  xdotool
```

Build and test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

For local development with clang and LSP support:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sf build/compile_commands.json compile_commands.json
cmake --build build
ctest --test-dir build --output-on-failure
```

Install:

```sh
cmake --install build --prefix ~/.local
```

## CLI Usage

```sh
llm-rewriter --input clipboard --output preview
llm-rewriter --input primary --output preview
llm-rewriter --input stdin --output stdout
llm-rewriter --input stdin --output clipboard
llm-rewriter --model openai/gpt-4.1-mini --reasoning-effort off --input stdin --output stdout
sleep 0.25 && llm-rewriter --input primary --output type
llm-rewriter --input clipboard --output paste --ctrl-c-before-output
```

Inputs:

- `clipboard`: normal clipboard text
- `primary`: Linux primary selection, when available
- `stdin`: standard input

Outputs:

- `preview`: open the UI with original and rewritten text
- `clipboard`: copy the rewrite
- `stdout`: print the rewrite
- `type`: type the rewrite with `wtype` or `xdotool`
- `paste`: copy the rewrite, then send the configured paste shortcut

`type` and `paste` are best-effort. If synthetic input is unavailable or fails, the rewrite is copied to the clipboard and the fallback is reported.

`--ctrl-c-before-output` applies only to `type` and `paste`. It can interrupt the focused application and is never enabled by default.

Use shell composition when a delay is needed before synthetic output:

```sh
sleep 0.25 && llm-rewriter --input clipboard --output paste
```

## UI Usage

The preview UI shows the original draft and the rewrite side by side. Press `Rewrite` to start the request. While refining, the original text is disabled and a spinner is shown. Notifications are not emitted by default while the UI is active.

Actions:

- `Copy Rewrite`
- `Copy Original`
- `Cancel`

The desktop application id is `llm_rewriter`. On Sway, use a window rule if you want the preview UI to float:

```ini
for_window [app_id="llm_rewriter"] floating enable
for_window [app_id="llm_rewriter"] resize set 900 650
for_window [app_id="llm_rewriter"] move position center
```

For Xwayland/X11 fallback rules, target the class:

```ini
for_window [class="llm_rewriter"] floating enable
```

## Configuration

Config defaults to:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/llm-rewriter/config.ini
```

On first execution, `llm-rewriter` creates this file if it does not exist. The generated file keeps optional features commented out and leaves only `model` active because it is required for LLM calls.

History defaults to append-only JSONL at:

```text
${XDG_DATA_HOME:-$HOME/.local/share}/llm-rewriter/history.jsonl
```

Example:

```ini
provider = openrouter
api_format = openai_chat
base_url = https://openrouter.ai/api/v1
model = openai/gpt-4.1-mini
reasoning_effort = low
max_output_tokens_limit = 65536

input = primary
output = type
paste_shortcut = ctrl_shift_v

api_key = ...    
api_key_env = OPENROUTER_API_KEY
timeout_ms = 30000
history_enabled = true

notification_mode = cli
notification_events = lifecycle

system_prompt = <<EOF
Rewrite the user's current message as a clear, concise prompt for an AI coding agent.
Return only the rewritten current prompt.
EOF
```

Supported `api_format` values:

- `openai_chat`
- `openai_responses`
- `anthropic_messages`

Input/output defaults:

- `input = clipboard | primary | stdin`
- `output = preview | clipboard | stdout | type | paste`

CLI flags override config values. `--model` overrides `model`; `--reasoning-effort` overrides `reasoning_effort`.

Paste shortcut values:

- `paste_shortcut = ctrl_v`
- `paste_shortcut = ctrl_shift_v`
- `paste_shortcut = shift_insert`

`ctrl_shift_v` is commonly useful for terminal paste flows.

Credentials:

- `api_key = ...` uses an inline API key.
- `api_key_env = ENV_NAME` reads an API key from an environment variable.
- If both are present, the first valid declaration in the config file wins.
- If neither is declared, the default `api_key_env = OPENROUTER_API_KEY` behavior is preserved.

Notifications:

- `notification_mode = cli`: default; notify for CLI workflows only
- `notification_mode = always`: notify for CLI and UI workflows
- `notification_mode = off`: never notify
- `notification_events = lifecycle`: start, success, and failure
- `notification_events = completion`: success and failure only
- `notification_events = errors`: failure only

## Completions

```sh
llm-rewriter --generate-completion bash
llm-rewriter --generate-completion zsh
llm-rewriter --generate-completion fish
```

Example install locations:

```sh
llm-rewriter --generate-completion bash > ~/.local/share/bash-completion/completions/llm-rewriter
llm-rewriter --generate-completion zsh > ~/.local/share/zsh/site-functions/_llm-rewriter
llm-rewriter --generate-completion fish > ~/.config/fish/completions/llm-rewriter.fish
```
