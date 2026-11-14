# AGENTS.md

Agent-facing guidance for this repository. OpenCode loads this file
automatically; apply it in every session. It describes the code as it is: if it
contradicts the code, the code wins — fix this file in the same commit.

## What this is

SDL3MCP is an MCP server for SDL3 applications, in two pieces:

- **the `SDL3MCP` library**, linked into the application. It dials one MCP
  client, answers four tools on the application's own thread, and owns no
  window, thread or descriptor.
- **the `sdl3mcp` Handler**, a standalone Linux binary. It speaks MCP to a client
  (stdio, or an upstream socket in Relay mode), accepts one application at a
  time, presents captures as PNG, and spawns applications from `--targets` and
  `--web` sources.

This is release 1.0.0. The v0 tree was deleted in the rewrite; it survives in
`v0.1.0` and history.

## Where the truth lives

- **`README.md` is the contract**: the CLI grammar, the MCP schemas, the framing,
  the loop order, the public targets, the browser arm and the limits.
- **`README.md` §Build** has the configure, build and `ctest` flow, the presets
  (`debug`, `shared`, `asan`) and the Emscripten configure; §Tests lists the
  suites.
- **`STYLE.md`** fixes naming, formatting, headers, errors, tests and CMake; its
  §Tests carries the payload-floor and drained-child-output rules. **`CONTRACT.md`**
  is the frozen v0 archive, not the current contract.

## Architecture

`docs/architecture.md` holds the internals the contract does not name. Read the
section the branch names, not the whole file:

- **Touching the Handler** — the poll loop, forwarding, merging, spawn and the
  web stack → *The Handler*, *File descriptor ownership*.
- **The browser arm** — `src/WebSocketStream.cpp` and the page query → *The
  browser arm*.
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
  files; `scripts/check.sh` enforces it over every tracked C/C++ file.
- `scripts/check.sh` is the mechanical gate: `clang-format --dry-run --Werror`,
  the `debug` configure, the build and `ctest`. Run it before committing. The
  tracked `scripts/hooks/pre-commit` runs the same script, so `git commit` is
  rejected while the gate fails; a fresh clone enables it with one command,
  `scripts/install-hooks.sh` (it sets `core.hooksPath=scripts/hooks`). The hook
  reuses its build tree (`SDL3MCP_CHECK_BUILD_DIR`, default `build/check`), so it
  pays only for what changed rather than a clean configure each time. Set
  `CMAKE_PREFIX_PATH` when doctest is not installed as a package; the script
  forwards it to CMake.
- One atomic change per commit. Commit messages describe the change.
- No dead code, no commented-out code, no speculative parameters.
- Before each commit, check the diff against this file. Any statement the change
  contradicts or obsoletes is fixed in the same commit.

## Commits and branches

- Work on a ticket branch; never commit to `master` or `develop` and never push.
  Merging and pushing are the human's decisions.
- Keep `third_party/` untouched: those submodules are read-only upstreams.
  `nlohmann/json` and `stb` are vendored headers, not forks to edit.

## Session self-check

- Did this session avoid asking how to build or run? It should — README §Build.
- Did anything touch `third_party/`, reformat untouched code, or leave a thread
  in the library or Handler?
- Did the shared build still export only the C API?
- Do `ctest` (debug, shared, asan) and `readme_check` pass?
