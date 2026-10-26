# AGENTS.md

SDL3MCP is a planned library that embeds an MCP server in any SDL3 application:
public C API, C++20 internals, stdio transport, synthetic input, and PNG frame
capture. **There is no implementation yet.** The repository is `README.md` only,
on an unborn `master` branch; `README.md` is the design contract.

## Current state

- Only `README.md` exists. No `CMakeLists.txt`, `src/`, `tests/`, or CI. Do not
  invent build/test commands or assume those paths are populated.
- The README examples were syntax-checked with
  `gcc -std=c11 -fsyntax-only $(pkg-config --cflags sdl3)` against SDL 3.4.18.
  When code lands, replace this note with the real build/test commands.
- Target SDL 3.2+. There, `SDL_RenderReadPixels(renderer, rect)` returns an
  `SDL_Surface *`; the old five-argument form is retired.

## Decisions not to regress

- **stdio only.** TCP was considered and removed. The default transport is
  `SDL3MCP_TRANSPORT_NONE`; `--mcp` (or `--mcp=stdio`) enables the server,
  `--mcp=off` disables it.
- **Standard MCP content.** `sdl_capture` returns `{"type":"image",
  "mimeType":"image/png","data":"<base64>"}`. A binary continuation framing was
  considered and rejected during design; do not reintroduce a custom client or a
  non-standard content type.
- **Frame contract.** `SDL3MCP_WriteFrame` accepts only tightly packed RGBA8888,
  `width * height * 4` bytes at the bound window's
  `SDL_GetWindowSizeInPixels`, top-left origin, no row padding. PNG encoding and
  base64 happen on the server thread.
- **Input is event-based.** Actions become `SDL_PushEvent` traffic applied in
  `SDL3MCP_AppIterate` on the app thread. `SDL_GetKeyboardState` cannot be
  updated because SDL3 exposes no per-key setter; that is a documented
  limitation, not a bug to work around with SDL internals.
- **Threading.** The server thread never calls SDL. Geometry is snapshotted on
  the app thread; frames and input cross through queues/copies.
- **Public surface.** The six entry points plus the header-inline
  `SDL3MCP_DefaultConfiguration()`. Use `SDL3MCP_Configuration` (the original
  brief's `SDL3MCP_Configuratio` was a typo) and `(void)` prototypes. The C API
  is C11 and never throws; C++ exceptions stop at the boundary.
- **Dependencies stay SDL3-only at runtime.** `stb_image_write.h` and
  `nlohmann/json.hpp` are resolved system-first with a vendored fallback in
  `third_party/`.

## Working on the spec

- `README.md` is the single source of truth for API contracts, CLI grammar, MCP
  schemas, and threading rules.
- Wire names are snake_case (`max_frame_bytes`); C++ names are camelCase.
- Change the implementation and the matching README section together, including
  the tool JSON schemas when a parameter changes.
