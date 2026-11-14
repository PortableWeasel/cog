# Cog

An LLM harness and agent framework written in C.

Inspired by the architecture of tools like `pi` and `opencode`, Cog brings the same loop-driven, event-based agent orchestration to a lightweight, embeddable C codebase.

## Intended architecture

- **cog-core** — agent loop, session management, tool dispatch, state tracking
- **cog-provider** — LLM provider abstraction (OpenAI, Anthropic, local models)
- **cog-toolkit** — tool definitions, file operations, shell execution, MCP integration
- **cog-cli** — terminal UI entry point

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Status

Pre-alpha. Scaffold only.