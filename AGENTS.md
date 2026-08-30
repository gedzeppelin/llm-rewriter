# Repository Guidelines

## Project Structure & Module Organization

This is a C++20 native application repository for Linux and Windows. Public headers live in `include/llm_rewriter/`; implementation files are split by layer under `src/`. Portable backend code belongs in `src/backend/`, OS runtime integration in `src/platform/linux/` and `src/platform/windows/`, the shared CLI in `src/cli/`, and the optional GTK frontend in `src/frontends/gtk4/`.

Tests are in `tests/CoreTests.cpp` and use doctest. Desktop integration assets are in `data/`. User-facing setup and usage docs belong in `README.md`; future plans and internal notes belong in `INTERNAL.md`.

The optional GTK4 frontend depends on the pinned `peel` submodule. Initialize it with `git submodule update --init --recursive` before GTK4 builds; generated bindings belong in `build/peel-generated`.

## Build, Test, And Development Commands

Configure a normal debug build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Use the Make wrapper for common Linux target sets:

```sh
make build
make build GTK4=ON
make test
```

Configure with clang and LSP support:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
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
build/llm-rewriter doctor
```

## Coding Style & Naming Conventions

Use two-space indentation and keep code C++20-compatible. Types and functions use `PascalCase` where the existing code does, enum values use `PascalCase`, and local variables use `snake_case`. Keep all project symbols in the `llm_rewriter` namespace and headers under `include/llm_rewriter/`.

Keep the backend free of UI toolkit and OS integration dependencies. Use the platform layers for paths, clipboard, notifications, helper probing, synthetic output, and native HTTP transports. Frontends consume platform services through generic project interfaces. Prefer small, focused modules and avoid unrelated refactors.

GTK UI code should use modern libadwaita application structure and widgets. `config.json` remains the native desktop source of truth.

Do not add compatibility aliases for names, flags, config keys, or targets. This project has not had a first stable launch yet, so old names do not need to be preserved. Replace obsolete names directly and update tests and docs in the same change.

## Testing Guidelines

Add doctest coverage for config parsing, CLI behavior, payload construction, token limits, notification policy, doctor reports, C ABI behavior, and path-sensitive behavior when changed. Name test cases as short behavior descriptions. Run `ctest --test-dir build --output-on-failure` before handing off changes.

## Security & Configuration Tips

Do not commit API keys or generated user config. Linux runtime config is created under `${XDG_CONFIG_HOME:-$HOME/.config}/llm-rewriter/config.json`; Linux history is stored under `${XDG_DATA_HOME:-$HOME/.local/share}/llm-rewriter/history.jsonl`. Windows runtime state belongs under `%APPDATA%\llm-rewriter\`.
