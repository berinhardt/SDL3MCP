# AGENTS.md

SDL3MCP is a library that embeds an MCP server in any SDL3 application: public
C API, C++20 internals, stdio transport, synthetic input, and PNG frame capture.
`README.md` is the design contract and the implementation follows it: the
transport, input replay, frame pipeline, and all four MCP tools are in `src/`,
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
  presets. All four pass `-DSDL3MCP_BUILD_SUPERVISOR=ON`, so every preset builds
  `sdl3mcp-supervisor` and registers the supervisor tests; the `asan` suite runs
  with leak detection on (the real-example supervisor test scopes it off for
  itself and its SDL child) and `tsan` filters to `queue_tests`, `server_tests`,
  and `supervisor_tests`. The direct `cmake -S . -B build` flow stays the
  documented path and no CI workflow runs the presets.
- The example target is `minimal`, built from `examples/CMakeLists.txt`, so the
  binary is `build/examples/minimal`; run `./build/examples/minimal --mcp` from
  the repository root (or `cd build && ./examples/minimal --mcp`). There is no
  `examples/minimal/minimal`.
- `tests/check_readme.py` compiles the README's two complete C examples, parses
  every JSON block, and checks both `sdl_capture` schemas, the raw-framing
  prose, the region's captured-frame dimensions, the four-argument
  `SDL3MCP_WriteFrame` signature, the run command, the supervisor states, and
  the loop order. CTest registers it as `readme_check` when Python 3 is found.
- `extras/sdl-vision/` is optional OpenCode agent configuration — a subagent
  plus a skill that drives `sdl_capture` through a vision model. It is
  not built or installed by CMake, has no `ctest` entry, and
  `tests/check_readme.py` ignores it.
- `SDL3MCP_BUILD_SUPERVISOR=ON` builds `build/sdl3mcp-supervisor`, the optional
  single-endpoint supervisor over a restartable app. Its sources live in
  `supervisor/` (CLI, child process, proxy, scaling, PNG/base64) and its tests
  are `supervisor_scaling_tests` and `supervisor_png_tests` (the moved
  presentation code), `supervisor_tests` (fake MCP child over real pipes) and
  `supervisor_real_app` (the real `examples/minimal` under
  `SDL_VIDEODRIVER=dummy`); the last is registered only when examples are
  built. `opencode.json` in the repository root points the local MCP client at
  the supervisor.
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
- **Capture framing: raw in the library, image/png at the MCP edge.**
  `sdl_capture` is the one tool whose library result is not a content array: it
  writes one result line
  `{"format":"rgba8888","width":W,"height":H,"byteLength":B}` and then exactly
  `B` tightly packed RGBA bytes, with no newline and no base64. `region` stays in
  the library; downscaling, PNG, and base64 are supervisor policy. The
  supervisor reads that framing, validates the header and payload size, and
  returns the standard `{"type":"image","mimeType":"image/png","data":"<base64>"}`
  content block, which stays the client-facing contract. Do not reintroduce a
  custom client, a non-standard content type, or PNG encoding in the library.
- **Frame contract.** `SDL3MCP_WriteFrame(data, size, width, height)` accepts
  only tightly packed RGBA8888 with `size == width * height * 4`, top-left
  origin, no row padding and no pitch parameter. The app supplies the
  dimensions, so any render target is valid and the library never reads the
  window size to validate a frame. The app converts and packs when a readback
  surface is padded. The server thread crops the optional region and writes the
  raw framing; PNG encoding and base64 happen only in the supervisor. Drops
  (size mismatch for the supplied dimensions, `max_frame_bytes`, null data,
  non-positive dimension) leave the request pending and surface in the timeout
  text and `sdl_info`.
- **Capture region.** `region` is `{x, y, width, height}` in pixels of the
  captured frame, top-left origin, all four required, and must fit inside
  `capture.lastWidth` by `capture.lastHeight`; the library crops it before the
  supervisor's `max_width` downscaling. `max_width` is 16..16384 and exists only
  in the supervisor's schema. `timeout_ms` is 1..60000.
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
- **`sdl_terminate` only sets a flag.** The server thread stores the request and
  returns `termination requested`; it never calls SDL. `SDL3MCP_AppIterate`
  exchanges the flag and pushes exactly one zeroed `SDL_EVENT_QUIT`, restoring
  the flag to retry when `SDL_PushEvent` fails. It still returns
  `SDL_APP_CONTINUE` and leaves the app's own quit handling untouched.
- **The supervisor is an optional separate process with one state machine.** It
  speaks MCP on its own stdio and runs one target child at a time. `Idle`
  advertises only `spawn`, `Starting` advertises nothing, and `Running` mirrors
  the committed child tools except `sdl_capture`, which the supervisor replaces
  with its own definition (region, `max_width`, `timeout_ms`) because it owns
  presentation. `spawn` is synchronous: fork/exec, initialize, `tools/list`, then
  poll `sdl_info` until a window is bound or `--startup-timeout-ms` expires. A
  second `spawn` while not idle is an error. Every transition that changes the
  tool view sends one `notifications/tools/list_changed`; do not make it fire
  twice or cache the tool list on the client side.
- **The terminate watchdog is supervisor policy, not library policy.**
  `sdl_terminate` is forwarded like any other running tool, but its timer starts
  before the forward and the supervisor waits for the child to exit. Clean exit
  returns the text `terminated`; on `--terminate-timeout-ms` (default 60000) the
  watchdog sends SIGTERM, waits about two seconds, then SIGKILL, and says the
  termination was forced. The child's exit handler, whether from the request, a
  crash, or the watchdog, restores `Idle`/`spawn` and sends the single
  `list_changed`.
- **Supervisor shutdown never orphans the child.** stdin EOF, SIGINT/SIGTERM,
  and a normal return all run the same shutdown: SIGTERM, grace, SIGKILL, then
  join and reap. Signals are blocked process-wide and consumed by a `sigwait`
  thread because the handler cannot take the state or writer mutexes.
- **The supervisor reuses the library plumbing.** CLI parsing, the line
  transport, and JSON-RPC dispatch come from `supervisor/cli.*`,
  `src/transport.*`, and `src/json_rpc.*`; the supervisor links
  `sdl3mcp_objects` and `sdl3mcp_supervisor_objects` rather than duplicating
  them. It never calls SDL and is POSIX-only (`fork`/`exec`, pipes, signals),
  matching the platform stance above.
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
- **Dependencies stay SDL3-only at runtime.** `nlohmann/json.hpp` is resolved
  system-first (`SDL3MCP_USE_SYSTEM_JSON`) with the vendored
  `third_party/nlohmann` header as the fallback. `stb_image_write.h` is vendored
  in `third_party/stb` but used only by the supervisor: it is a private include
  and one implementation unit in `supervisor/`, compiled into
  `sdl3mcp_supervisor_objects`, and the library archive must not contain an stb
  symbol. There is no `SDL3MCP_USE_SYSTEM_STB` option.

## Working on the spec

- `README.md` is the single source of truth for API contracts, CLI grammar, MCP
  schemas, and threading rules.
- Wire names are snake_case (`max_frame_bytes`); C++ names are camelCase.
- Change the implementation and the matching README section together, including
  the tool JSON schemas when a parameter changes.
- `tests/check_readme.py` enforces the run path (`./build/examples/minimal
  --mcp`), both `sdl_capture` schemas, the capture framing prose, the two
  complete examples, the stale phrases, and `SDL3MCP_AppIterate` after
  `SDL_RenderPresent`; update it when the contract changes rather than
  weakening it.
