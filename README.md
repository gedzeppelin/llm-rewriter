# llm-rewriter Native

C++20 application for rewriting drafts with an LLM on Linux and Windows. The CLI frontend is shared across platforms, with optional GTK4 presentation on Linux and platform-specific runtime services underneath.

## Dependencies

Arch Linux:

```sh
sudo pacman -S --needed \
    base-devel clang cli11 cmake curl doctest gtk4 libadwaita libnotify ninja nlohmann-json pkgconf wl-clipboard wtype xclip xdotool xsel ydotool
```

APT-based distributions:

```sh
sudo apt update
sudo apt install -y \
    build-essential clang cmake doctest-dev libadwaita-1-dev libcli11-dev libcurl4-openssl-dev libgtk-4-dev libnotify-bin libsecret-tools ninja-build nlohmann-json3-dev pkg-config wl-clipboard wtype xclip xdotool xsel ydotool
```

## Peel Submodule

The optional GTK4 frontend uses [peel], which is pinned in this repository as a Git submodule.

```sh
git submodule update --init --recursive
```

The CLI-only build does not require peel, but `make build GTK4=ON` does. CMake runs `peel/peel-gen.py` for that target and places generated headers in `build/peel-generated`.

[peel]: https://gitlab.gnome.org/bugaevc/peel

## Build And Test

Build and test the CLI:

```sh
make build
make test
```

Linux GTK4 frontend:

```sh
make build GTK4=ON
```

Equivalent CMake flow:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
ctest --test-dir build --output-on-failure
ln -sf build/compile_commands.json compile_commands.json
```

## Linux Desktop Installation

Configure and build the GTK4 frontend before installing it:

```sh
make build GTK4=ON
```

For a user-only installation, use `$HOME/.local` as the prefix:

```sh
cmake --install build --prefix "$HOME/.local"
```

This copies the launcher entry to `$HOME/.local/share/applications/llm_rewriter.desktop` and does not require root access. Ensure `$HOME/.local/bin` is in the environment inherited by your desktop session so the launcher's `Exec=llm-rewriter-gtk4 ...` command can find the executable. The install is application-only: it does not copy project headers or a separate backend shared library.

For a system-wide installation, install the same build under `/usr`:

```sh
sudo cmake --install build --prefix /usr
```

This copies the entry to `/usr/share/applications/llm_rewriter.desktop` and the executables to `/usr/bin`. Build as your normal user and use `sudo` only for the install command.

Uninstall the files recorded by the most recent install from this build tree:

```sh
cmake --build build --target uninstall
```

Use `sudo cmake --build build --target uninstall` if that manifest describes a system-wide `/usr` install. The target removes only paths listed in `build/install_manifest.txt`; it does not recursively delete installation directories. Running another install from the same build tree replaces that manifest, so use a separate build directory for each prefix when maintaining user and system installations at the same time.

On Windows, the same `llm-rewriter` CLI target is built. Its HTTP transport uses WinHTTP and clipboard input/output is native. Automatic typing and paste currently fall back to the clipboard, and notifications are not yet emitted.

## CLI Usage

```sh
llm-rewriter doctor
llm-rewriter doctor --live
llm-rewriter --input stdin --output stdout --model openai/gpt-4.1-mini
llm-rewriter --input stdin --output clipboard
sleep 0.25 && llm-rewriter --input primary --output type
llm-rewriter --input clipboard --output paste --ctrl-c-before-output
```

Inputs:

- `clipboard`: normal clipboard text
- `primary`: Linux primary selection; equivalent to the clipboard on Windows
- `stdin`: standard input

Outputs:

- `preview`: require a graphical frontend
- `clipboard`: copy the rewrite
- `stdout`: print the rewrite
- `type`: type with `wtype`, `xdotool`, or `ydotool`
- `paste`: copy the rewrite, then send the configured paste shortcut

`type` and `paste` are best-effort. If synthetic input is unavailable or fails, the rewrite is copied to the clipboard and the fallback is reported.

## Linux UI Usage

```sh
llm-rewriter-gtk4 --input clipboard
```

The GTK4 frontend uses libadwaita and exposes native preferences for provider, base URL, model, reasoning, input, credentials, and system prompt while keeping `config.json` as the native settings file.

## Configuration

Linux config defaults to:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/llm-rewriter/config.json
```

Linux history defaults to:

```text
${XDG_DATA_HOME:-$HOME/.local/share}/llm-rewriter/history.jsonl
```

Metadata-only request diagnostics are written alongside history as `diagnostics.jsonl`. They include request IDs, provider/model, HTTP status, provider request IDs when available, duration, and bounded error details; they never include prompts, output, or resolved credentials.

Windows config and history are stored under:

```text
%APPDATA%\llm-rewriter\
```

Built-in providers are `openai`, `anthropic`, `gemini`, `openrouter`, and `codex`. The `custom` provider covers local servers (for example Ollama, llama.cpp, or vLLM) and other compatible endpoints without a provider-specific integration.

`custom` exposes the endpoint `base_url`, an `api_format` (`openai_chat`, `openai_responses`, or `anthropic_messages`), and optional JSON headers, query parameters, and request-body overrides. Credentials are optional, so local servers that do not require authentication work without a key. Built-in providers use their well-known endpoints and formats automatically.

Supported API formats:

- `openai_chat`
- `openai_responses`
- `anthropic_messages`

Credentials are resolved at request time. For API-key providers, the canonical environment variable is checked first, followed by the optional `credentials.credential` value in `config.json`, then the platform credential store. The configured credential is the actual secret value, so protect the config file accordingly. Codex instead reuses the prefilled `~/.codex/auth.json` external auth file before its native store; that file is user-owned and may be refreshed in place but is never deleted or configured by this application. Secrets are never written to history.

Provider management is explicit:

    printf '%s' "$OPENROUTER_API_KEY" | llm-rewriter providers configure openrouter
    llm-rewriter providers status openrouter
    llm-rewriter providers clear openrouter
    llm-rewriter providers configure codex --device-code

The CLI never accepts secrets as command-line arguments and a missing credential never starts OAuth during a rewrite.

Canonical environment variables are fixed: `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `GEMINI_API_KEY`, `OPENROUTER_API_KEY`, and `CODEX_ACCESS_TOKEN`. Environment-variable names are not configurable.
