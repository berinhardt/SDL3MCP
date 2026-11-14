# AGENTS.md

Agent-facing guidance for this repository. OpenCode loads this file
automatically; apply it in every session. It describes the code as it is: if it
contradicts the code, the code wins — fix this file in the same commit.

## What this is

SDL3MCP is release 1.0.0: an MCP server for SDL3 applications, in two pieces;
the v0 tree was deleted in the rewrite and survives in `v0.1.0` and history.

- **the `SDL3MCP` library**, linked into the application. It dials one MCP
  client, answers four tools on the application's own thread, and owns no
  window, thread or descriptor.
- **the `sdl3mcp` Handler**, a standalone Linux binary. It speaks MCP to a
  client (stdio, or an upstream socket in Relay mode), accepts one application
  at a time, presents captures as PNG, and spawns from `--targets` and `--web`.

## Where the truth lives

- **`README.md` is the contract**: CLI grammar, MCP schemas, framing, loop
  order, public targets, browser arm and limits; §Build has configure, build
  and `ctest`, the presets (`debug`, `shared`, `asan`), the Emscripten
  configure, and §Tests lists the suites.
- **`STYLE.md`** fixes naming, formatting, headers, errors, tests and CMake; its
  §Tests carries the payload-floor and drained-child-output rules.
  **`CONTRACT.md`** is the frozen v0 archive, not the current contract.

## Architecture

`docs/architecture.md` holds the internals the contract does not name; read the
section the branch names, not the whole file:

- **Touching the Handler** — the poll loop, forwarding, merging, spawn, the web
  stack → *The Handler*, *File descriptor ownership*.
- **The browser arm** — `WebSocketStream.cpp`, the page query → *The browser arm*.
- **The wire protocol or a tool result** — `Session`, `Stream`, framing, input →
  *The wire protocol*.
- **The build targets or test layout** — object libraries, visibility, linkage →
  *Build composition*, *ABI and linkage*.

## Working on the spec

- Wire names are snake_case (`max_frame_bytes`); C++ names are camelCase; types
  are PascalCase; files are CamelCase. `STYLE.md` is the authority.
- Change the implementation and the matching README section together, including
  the tool JSON schemas when a parameter changes.
- `tests/check_readme.py` enforces the two complete examples, the JSON blocks,
  the tool schemas, the `Example:` descriptions, the framing prose, the
  `sdl_input` text, the `SDL3MCP_WriteFrame` signature, the run command and the
  loop order. Update it when the contract changes; never weaken it.
- `CHANGELOG.md` records breaking changes; add an entry for every user-visible
  break, not for internal refactors.

## Editing etiquette

- Minimal diffs. Do not reformat untouched code. `.clang-format` (Google base,
  3-space indent, no column limit, left-aligned pointers) applies to touched
  files; `scripts/check.sh` enforces it over every first-party C/C++ source.
- `scripts/check.sh` is the gate (formatting, `debug` build, `ctest`), run by
  the tracked `scripts/hooks/pre-commit`; `scripts/install-hooks.sh` sets
  `core.hooksPath=scripts/hooks` in a fresh clone. It reuses
  `SDL3MCP_CHECK_BUILD_DIR` (default `build/check`) and forwards
  `CMAKE_PREFIX_PATH` to CMake for doctest.
- One atomic change per commit, with a message that describes it; no dead code,
  no commented-out code, no speculative parameters. Before each commit, check
  the diff against this file and fix any statement the change contradicts.

## Commits and branches

- Work on a ticket branch; never commit to `master` or `develop`, never push
  (merging and pushing are the human's decisions); keep `third_party/` untouched:
  those submodules are read-only upstreams, and `nlohmann/json` and `stb` are
  vendored headers, not forks to edit.

## Session self-check

- Did this session avoid asking how to build or run (README §Build), touch
  `third_party/`, reformat untouched code, leave a thread, break the shared
  build's C-only export, or fail `ctest` (debug, shared, asan) or `readme_check`?
