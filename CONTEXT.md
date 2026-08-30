# llm-rewriter Native

This context describes the native Linux and Windows llm-rewriter product.

## Language

**Native Product**:
The C++ application family for Linux and Windows, including the backend, CLI, and native desktop frontends.
_Avoid_: Android frontend, shared mobile backend

**Android Product**:
The standalone Android application maintained outside this repository.
_Avoid_: Android frontend, JNI frontend

**Provider Behavior**:
The product contract for turning rewrite settings and draft text into supported provider requests, then interpreting provider responses.
_Avoid_: shared backend behavior

**Custom Provider**:
A user-defined provider endpoint that uses a supported provider format with optional request customization.
_Avoid_: plugin provider

**Provider Credential**:
A secret resolved lazily immediately before one provider request. Environment
values take precedence; a configured credential may be stored in the JSON
configuration; Codex may then use its explicitly selected external auth file;
other credentials may use the native credential store. Credentials are never
written to history.
_Avoid_: request-time login

**External Credential File**:
A user-owned Codex auth file selected by a path (with `~` expanded to the
user's home directory). The application may refresh token and account fields
in place but never configures, clears, or deletes the file.
_Avoid_: native credential store, imported profile

**Provider Configuration**:
The explicit operation family for configuring, clearing, or inspecting a
Provider Credential. Model requests do not start this operation implicitly.
_Avoid_: automatic sign-in, request-time OAuth

**Codex Provider**:
The distinct ChatGPT-backed provider that uses Codex OAuth credentials and
account-aware request headers. It is not the OpenAI Platform provider even
though both use bearer authentication.
_Avoid_: OpenAI-compatible provider, Codex executable
