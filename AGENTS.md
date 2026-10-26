# AGENTS.md

SDL3MCP is a library that embeds an MCP server in any SDL3 application: public
C API, C++20 internals, stdio transport, synthetic input, and PNG frame capture.
`README.md` is the design contract, and the repository holds the first compiling
slice: `LICENSE`, the CMake skeleton, the public header, and smoke tests. Only
`SDL3MCP_AppShutdown` is implemented; the other five entry points are declared
but deliberately undefined.

## Current state

- `README.md` (the spec), `LICENSE`, `CMakeLists.txt`, `include/SDL3MCP/`,
  `src/api.cpp`, and `tests/` exist. There is no transport, input, frame, or MCP
  code yet.
- Build and test with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`, then
  `cmake --build build --parallel`, then
  `ctest --test-dir build --output-on-failure`. For the shared configuration use
  a separate directory with `-DSDL3MCP_BUILD_SHARED=ON`.
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
- **Linkage follows SDL.** Public symbols carry `SDL_DECLSPEC` and `SDLCALL`.
  The shared build sets `DEFINE_SYMBOL "DLL_EXPORT"`; the static build defines
  `SDL_STATIC_LIB` privately. Do not add a private dllimport/export macro such
  as `SDL3MCP_DECLSPEC`, `SDL3MCP_BUILDING_LIBRARY`, or `SDL3MCP_STATIC`.
- **Dependencies stay SDL3-only at runtime.** `stb_image_write.h` and
  `nlohmann/json.hpp` are resolved system-first with a vendored fallback in
  `third_party/`.

## Working on the spec

- `README.md` is the single source of truth for API contracts, CLI grammar, MCP
  schemas, and threading rules.
- Wire names are snake_case (`max_frame_bytes`); C++ names are camelCase.
- Change the implementation and the matching README section together, including
  the tool JSON schemas when a parameter changes.
