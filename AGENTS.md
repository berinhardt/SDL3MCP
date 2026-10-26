# AGENTS.md

SDL3MCP is a library that embeds an MCP server in any SDL3 application: public
C API, C++20 internals, stdio transport, synthetic input, and PNG frame capture.
`README.md` is the design contract and the implementation follows it: the
transport, input replay, frame pipeline, and all three MCP tools are in `src/`,
and `examples/minimal` is a complete host.

## Current state

- The implementation is complete for v0; there is no declared-but-missing entry
  point and no stub tool. `README.md`, `LICENSE`, `CMakeLists.txt`,
  `include/SDL3MCP/`, `src/`, `tests/`, `examples/minimal/`, and
  `third_party/` describe and implement the whole surface.
- Build and test with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
  -DSDL3MCP_BUILD_EXAMPLES=ON`, then `cmake --build build --parallel`, then
  `ctest --test-dir build --output-on-failure`. For the shared configuration use
  a separate directory with `-DSDL3MCP_BUILD_SHARED=ON`. `CMakePresets.json`
  wraps the same configurations as `debug`, `shared`, `asan`, and `tsan`
  presets (the `asan` suite runs with leak detection on and `tsan` filters to
  `queue_tests` and `server_tests`). The direct `cmake -S . -B build` flow stays
  the documented path and no CI workflow runs the presets.
- The example target is `minimal`, built from `examples/CMakeLists.txt`, so the
  binary is `build/examples/minimal`; run `./build/examples/minimal --mcp` from
  the repository root (or `cd build && ./examples/minimal --mcp`). There is no
  `examples/minimal/minimal`.
- `tests/check_readme.py` compiles the README's two complete C examples, parses
  every JSON block, and checks the schemas, the run command, and the loop order.
  CTest registers it as `readme_check` when Python 3 is found.
- Target SDL 3.2+. There, `SDL_RenderReadPixels(renderer, rect)` returns an
  `SDL_Surface *`; the old five-argument form is retired. Keep examples on 3.2
  APIs (no `SDL_RenderDebugText` or other 3.4-only calls).

## Decisions not to regress

- **stdio only.** TCP was considered and removed. The default transport is
  `SDL3MCP_TRANSPORT_NONE`; `--mcp` (or `--mcp=stdio`) enables the server,
  `--mcp=off` disables it.
- **One lifecycle per process.** A second `SDL3MCP_AppInit` fails even after
  shutdown; `SDL3MCP_AppShutdown` is idempotent.
- **`SDL3MCP_AppIterate` is the last call of the frame**, after drawing, after
  `SDL_RenderPresent`, and after answering `SDL3MCP_FrameRequested`.
- **F+1 acknowledgement.** A batch applied during frame F has its events
  consumed by the app during F+1, so `sdl_input` returns during F+1 and reports
  frame F. At most one batch is in flight.
- **Standard MCP content.** `sdl_capture` returns `{"type":"image",
  "mimeType":"image/png","data":"<base64>"}`. A binary continuation framing was
  considered and rejected during design; do not reintroduce a custom client or a
  non-standard content type.
- **Frame contract.** `SDL3MCP_WriteFrame` accepts only tightly packed RGBA8888,
  `width * height * 4` bytes at the bound window's
  `SDL_GetWindowSizeInPixels`, top-left origin, no row padding and no pitch
  parameter. The app converts and packs when a readback surface is padded. PNG
  encoding and base64 happen on the server thread. Drops (no window, zero
  drawable pixels, size mismatch, `max_frame_bytes`, null data, non-positive
  dimension) leave the request pending and surface in the timeout text and
  `sdl_info`.
- **Capture region.** `region` is `{x, y, width, height}` in full-frame pixels,
  top-left origin, all four required, and must fit inside `pixelWidth` by
  `pixelHeight`; the crop happens before `max_width` downscaling. `max_width` is
  16..16384 and `timeout_ms` is 1..60000.
- **Input is event-based.** Actions become `SDL_PushEvent` traffic applied in
  `SDL3MCP_AppIterate` on the app thread. `SDL_GetKeyboardState` cannot be
  updated because SDL3 exposes no per-key setter; that is a documented
  limitation, not a bug to work around with SDL internals.
- **No `repeat` field.** Taps are repeated `press` actions; auto-repeat is a
  repeated `down` without an intervening `up`, and that event's `repeat` flag is
  set. Modifiers are valid only on `press`.
- **Key names resolve at apply time.** Names are SDL scancodes and aliases with
  `-`/`_` as word separators; single characters go through
  `SDL_GetScancodeFromKey` on the app thread. Unknown key or chord errors are
  reported at F+1; structural errors fail before queueing.
- **Text is one event.** A `text` action pushes a single
  `SDL_EVENT_TEXT_INPUT` with the whole UTF-8 string, requires
  `SDL_TextInputActive`, and the event's pointer is valid only until F+1; the
  library does not retain it and the app must not either. Do not reintroduce
  chunking.
- **Scroll is in notches.** `1.0` is one notch; integer wheel fields stay 0.
- **Warping needs no focus.** `SDL_WarpMouseInWindow` is best effort and a
  synthetic zero-delta motion accompanies the warp; Remote Desktop or headless
  sessions may ignore the OS warp while event-driven apps still see the move.
- **Requests are serialized.** One server thread handles one message at a time,
  so a blocking `sdl_input`/`sdl_capture` delays later requests, including
  `ping`, by up to its timeout.
- **fd ownership.** `NONE` leaves stdin/stdout untouched. `STDIO` duplicates the
  originals for MCP, redirects the host stdin to `/dev/null` and the host stdout
  to stderr, and restores both at shutdown. stderr stays separate and is where
  diagnostics go; the host must not write stdout before `SDL3MCP_AppInit`.
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
- **A shared build exports only the C surface.** The C++ internals stay hidden
  (`CXX_VISIBILITY_PRESET hidden`), so unit tests that include `src/` headers
  link the object library `sdl3mcp_objects` rather than the shared object. The
  sources are compiled once; do not add a second static copy for the tests or
  drop the hidden visibility to make them link.
- **The transport and stdio isolation are POSIX-only; Windows is a TODO.**
  `poll`/`read`/`write`, `dup`/`dup2`, `/dev/null`, and blocking `SIGPIPE` on
  the server thread are the shipped paths. `CMakeLists.txt` rejects a `WIN32`
  configure with a `FATAL_ERROR`, and `src/transport.cpp` and
  `src/stdio_isolation.cpp` carry a `TODO(windows)` comment naming the work:
  binary stdio (`_setmode` with `_O_BINARY`), a Win32 reader wake-up
  (`PeekNamedPipe`/`WaitForSingleObject`), `_dup`/`_dup2` with `NUL` for stdio
  isolation, and test guards. Do not add untested Win32 code; later Windows
  work removes the guard, adds those paths, and extends the test matrix to a
  Windows host or CI.
- **Dependencies stay SDL3-only at runtime.** `stb_image_write.h` is vendored in
  `third_party/stb` and compiled once in `src/stb_image_write_impl.cpp`.
  `nlohmann/json.hpp` is resolved system-first (`SDL3MCP_USE_SYSTEM_JSON`) with
  the vendored `third_party/nlohmann` header as the fallback. There is no
  `SDL3MCP_USE_SYSTEM_STB` option.

## Working on the spec

- `README.md` is the single source of truth for API contracts, CLI grammar, MCP
  schemas, and threading rules.
- Wire names are snake_case (`max_frame_bytes`); C++ names are camelCase.
- Change the implementation and the matching README section together, including
  the tool JSON schemas when a parameter changes.
- `tests/check_readme.py` enforces the run path (`./build/examples/minimal
  --mcp`), the schema limits, the two complete examples, the stale phrases, and
  `SDL3MCP_AppIterate` after `SDL_RenderPresent`; update it when the contract
  changes rather than weakening it.
