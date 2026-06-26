# Repository Guidelines

## Project Structure & Module Organization

This is a C++20 wxWidgets application built with CMake. Public headers live in
`include/llm_rewriter/`; implementation files live in `src/`. Keep module pairs
aligned when possible, for example `Config.hpp` with `Config.cpp` and
`LlmClient.hpp` with `LlmClient.cpp`.

Tests are in `tests/CoreTests.cpp` and use doctest. Desktop integration assets
are in `data/`, currently `data/llm_rewriter.desktop`. User-facing setup and
usage docs belong in `README.md`; future plans and internal notes belong in
`INTERNAL.md`.

## Build, Test, and Development Commands

Configure a normal debug build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Configure with clang and LSP support:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sf build/compile_commands.json compile_commands.json
```

Build and test:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

Run locally from the build directory:

```sh
build/llm-rewriter --input stdin --output stdout --model openai/gpt-4.1-mini
```

## Coding Style & Naming Conventions

Use two-space indentation and keep code C++20-compatible. Types and functions use
`PascalCase` where the existing code does, enum values use `PascalCase`, and
local variables use `snake_case`. Keep all project symbols in the
`llm_rewriter` namespace and headers under `include/llm_rewriter/`.

Prefer small, focused modules and avoid unrelated refactors. Use standard
library facilities and existing project helpers before adding dependencies.

## Testing Guidelines

Add doctest coverage for config parsing, CLI behavior, payload construction,
token limits, notification policy, and path-sensitive behavior when changed.
Name test cases as short behavior descriptions. Run `ctest --test-dir build
--output-on-failure` before handing off changes.

## Commit & Pull Request Guidelines

There is no established commit history yet. Use concise imperative commit
messages, for example `Add model CLI override` or `Fix Wayland clipboard
fallback`.

Pull requests should include a short summary, test results, and any platform
limitations. For UI changes, include screenshots or a clear description of the
visible behavior.

## Security & Configuration Tips

Do not commit API keys or generated user config. Runtime config is created under
`${XDG_CONFIG_HOME:-$HOME/.config}/llm-rewriter/config.ini`; history is stored
under `${XDG_DATA_HOME:-$HOME/.local/share}/llm-rewriter/history.jsonl`.
