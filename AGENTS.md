# Repository guidance

C++20 native application for Linux and Windows, with a shared CLI and optional GTK4 frontend.

## Project decisions

- Keep `src/backend/` portable and independent of UI toolkits and OS integration. Platform layers own paths, clipboard, notifications, helper probing, synthetic output, and native HTTP transports; frontends consume them through project interfaces.
- This project has not had a stable release. Replace obsolete names, flags, config keys, and targets directly, without compatibility aliases; update affected tests and docs together.
- Use two-space indentation, `PascalCase` for types, functions, and enum values, and `snake_case` for local variables. Project symbols belong in `llm_rewriter`; public headers belong in `include/llm_rewriter/`.

## Build and verification

For dependencies, CMake options, and runtime setup, use [README.md](README.md). Common Linux commands:

```sh
make build          # CLI
make build GTK4=ON  # CLI and GTK4
make test           # Build and run doctest tests
```

Before a GTK4 build, initialize the pinned `peel` submodule with `git submodule update --init --recursive`. Generated bindings belong in `build/peel-generated`.

For behavior changes, add or update doctest coverage in `tests/CoreTests.cpp` and run `ctest --test-dir build --output-on-failure` after building the affected targets. Documentation-only changes need link and diff checks.

## Documentation

User-facing setup and usage belong in [README.md](README.md); future plans and internal notes belong in [INTERNAL.md](INTERNAL.md). For platform integration or credential changes, consult the architecture and secret-storage notes in `INTERNAL.md`.
