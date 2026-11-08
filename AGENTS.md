# AGENTS.md

SDL3MCP is a library that embeds an MCP server in any SDL3 application: public
C API, C++20 internals, stdio and dialed-socket transports, synthetic input, and
PNG frame capture. `README.md` is the design contract and the implementation
follows it: the transport, input replay, frame pipeline, and all four MCP tools
are in `src/`, and `examples/minimal` is a complete host.

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
  presets. All four pass `-DSDL3MCP_BUILD_CLI=ON`, so every preset builds
  `sdl3mcp` and registers the supervisor tests; the `asan` suite runs
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
  the loop order. It also runs `sdl3mcp_test_supervisor --dump-tools` and
  requires every catalog and README tool description to carry a parseable
  `Example:` JSON object naming each argument its schema accepts. CTest
  registers it as `readme_check` when Python 3 is found.
- `SDL3MCP_BUILD_CLI=ON` builds `build/sdl3mcp`, one executable with
  three modes over one peer seam. `--target <executable>` is the child
  supervisor: it speaks MCP on its own stdio and runs one target child at a time
  over private pipes. `--listen=HOST:PORT` is a relay: it speaks MCP on its own
  stdio and proxies to whichever application dials in, one session at a time.
  `--connect=HOST:PORT --targets=PATH [--listen=HOST:PORT]` is a spawner: it
  dials a relay, advertises one `app_spawn_<alias>` per targets entry, and starts
  the chosen application with `--mcp=HOST:PORT` so it dials back to a listener
  the spawner owns. The modes vary only how the data-path `Peer`
  (`supervisor/peer.hpp`) is created: `stdio_peer.*` for the client's own pipes,
  `child_peer.*` for a `--target` child, and `socket_peer.*` for an accepted
  application connection (`--listen`) or the dialed relay (`--connect`). The
  proxy loop never knows which one it holds. Sources live in `supervisor/` (CLI, child process, peers, proxy,
  targets file, PNG/base64) and its tests are `supervisor_png_tests` (the moved
  presentation code), `supervisor_tests` (fake MCP child over real pipes),
  `relay_tests` (fake application over TCP), `spawner_tests` (spawner against a
  relay the test owns) and `supervisor_real_app` (the real `examples/minimal`
  under `SDL_VIDEODRIVER=dummy`); the last is registered only when examples are
  built. `connect_tests` covers the library's `--mcp=HOST:PORT` dial.
  `opencode.json` in the repository root points the local MCP client at the
  supervisor.
- Target SDL 3.2+. There, `SDL_RenderReadPixels(renderer, rect)` returns an
  `SDL_Surface *`; the old five-argument form is retired. Keep examples on 3.2
  APIs (no `SDL_RenderDebugText` or other 3.4-only calls).

## Decisions not to regress

- **The library never listens.** The default transport is
  `SDL3MCP_TRANSPORT_NONE`; `--mcp` (or `--mcp=stdio`) enables the stdio server,
  `--mcp=HOST:PORT` dials one endpoint with no retry, and `--mcp=off` disables
  it. A failed dial fails `SDL3MCP_AppInit` instead of falling back to stdio or
  to no server. Only the optional `sdl3mcp` relay binds a port.
- **One lifecycle per process.** A second `SDL3MCP_AppInit` fails even after
  shutdown; `SDL3MCP_AppShutdown` is idempotent.
- **`SDL3MCP_AppIterate` is the last call of the frame**, after drawing, after
  `SDL_RenderPresent`, and after answering `SDL3MCP_FrameRequested`.
- **F+1 acknowledgement.** A batch applied during frame F has its events
  consumed by the app during F+1, so `sdl_input` returns during F+1 and reports
  frame F. At most one batch is in flight. The call then requests the frame that
  follows the batch through the same capture pipeline, so an action and its
  observation are one call: `timeout_ms` bounds both waits (default twice
  `frame_timeout_ms`), and a frame that cannot be taken is reported as a missing
  observation that still names the applied frame.
- **Capture framing: raw in the library, image/png at the MCP edge.**
  `sdl_capture` and `sdl_input` are the two tools whose library result is not a
  content array: they write one result line
  `{"format":"rgba8888","width":W,"height":H,"byteLength":B}` (`sdl_input` adds
  `"text"`, the line naming the frame it was applied at) and then exactly `B`
  tightly packed RGBA bytes, with no newline and no base64. `region` stays in
  the library; PNG and base64 are supervisor policy, and the supervisor encodes
  the frame at exactly the dimensions it received, so frames are never
  resized. The supervisor reads that framing, validates the header and payload
  size, and returns the standard
  `{"type":"image","mimeType":"image/png","data":"<base64>"}` content block,
  which stays the client-facing contract. A supervisor in front of another
  supervisor relays that already-presented `content` result untouched; only a
  raw `format`/`width`/`height`/`byteLength` frame is encoded, and exactly once,
  at the hop nearest the application. Do not reintroduce a
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
  `capture.lastWidth` by `capture.lastHeight`; the library crops it and the
  supervisor encodes the result at those same dimensions. `sdl_input` takes the
  same object and parses it with the same helper, so both tools accept exactly
  the same region. Captures are never downscaled, so PNG pixels map 1:1 to app
  pixels and `region` is the only payload-size lever. `timeout_ms` is 1..60000.
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
  `SDL_EVENT_TEXT_INPUT` with the whole UTF-8 string, and the event's pointer is
  valid only until F+1; the library does not retain it and the app must not
  either. Do not reintroduce chunking. Text input itself is started
  just in time: synthetic text needs no keyboard focus and the queued
  `SDL_EVENT_TEXT_INPUT` reaches the app regardless, so the library starts text
  input for a text batch and restores it afterwards instead of failing when it
  was inactive.
- **Scroll is in notches.** `1.0` is one notch; integer wheel fields stay 0.
- **Warping needs no focus.** `SDL_WarpMouseInWindow` is best effort and a
  synthetic zero-delta motion accompanies the warp; Remote Desktop or headless
  sessions may ignore the OS warp while event-driven apps still see the move.
- **Requests are serialized.** One server thread handles one message at a time,
  so a blocking `sdl_input`/`sdl_capture` delays later requests, including
  `ping`, by up to its timeout (for `sdl_input`, the whole apply-plus-frame
  budget).
- **The supervisor's forward deadline follows the tool.** A forwarded call that
  carries `timeout_ms` gets that budget plus a grace; without one, `sdl_input`
  gets twice the default frame timeout because it waits for the apply and then
  for the frame, while every other call gets one frame timeout. One forward path
  serves all tools: a result with `content` is relayed verbatim, a result with
  `byteLength` is read, PNG-encoded, and answered as text plus one image block
  (the frame's own `text` when it has one, the capture summary otherwise). An
  application configured with a larger `frame_timeout_ms` should pass
  `timeout_ms` explicitly.
- **`sdl_terminate` only sets a flag.** The server thread stores the request and
  returns `termination requested`; it never calls SDL. `SDL3MCP_AppIterate`
  exchanges the flag and pushes exactly one zeroed `SDL_EVENT_QUIT`, restoring
  the flag to retry when `SDL_PushEvent` fails. It still returns
  `SDL_APP_CONTINUE` and leaves the app's own quit handling untouched.
- **The supervisor is one executable with three modes and one state machine.**
  Child mode (`--target`) speaks MCP on its own stdio and runs one target child
  at a time. Relay mode (`--listen`) speaks MCP on its own stdio and waits for an
  application to dial in. Spawner mode (`--connect --targets`) speaks MCP to the
  relay it dialed and starts the chosen application on demand. The advertised
  tool list is a function of the mode, never of whether an application is
  attached: child mode advertises `spawn` plus the application's four tools,
  relay mode `wait_session` plus those four, and spawner mode one
  `app_spawn_<alias>` per targets entry plus those four. `sdl_capture` always
  carries the supervisor's own definition (region, `timeout_ms`) because it owns
  presentation. The supervisor merges the attached peer's entries over that base
  by name: the peer wins on a collision except `sdl_capture`, and names only the
  peer advertises are appended. A bare application is therefore an identity; a
  spawner behind a relay contributes its `app_spawn_*` entries. A forwarded tool
  the peer did not advertise is answered with
  `the application does not provide <name>` instead of being sent. Relay mode
  starts in `WaitingSession` and becomes `PeerAttached` once a session attaches;
  spawner mode starts in `Idle` and also uses `PeerAttached`. `spawn` is
  synchronous: fork/exec, initialize, `tools/list`, then poll `sdl_info` until a
  window is bound or `--startup-timeout-ms` expires. A second `spawn` while not
  idle is an error, and the spawner starts one application at a time. A spawn,
  attach, or detach changes no advertised name, so it sends no
  `notifications/tools/list_changed`; a client cannot be left with a stale list
  after a spawn. A notification is sent only when the merged set really changes.
  A supervisor never treats a peer's announcement as the list itself: on
  `notifications/tools/list_changed` from an attached peer — a spawner behind a
  relay, or an application behind a spawner — it asks that peer for `tools/list`
  again, re-merges it, and republishes the result, so a chained view is observed
  by listing.
- **The terminate watchdog is supervisor policy, not library policy.**
  `sdl_terminate` is forwarded like any other running tool, but its timer starts
  before the forward and the supervisor waits for the child to exit. Clean exit
  returns the text `terminated`; on `--terminate-timeout-ms` (default 60000) the
  watchdog sends SIGTERM, waits about two seconds, then SIGKILL, and says the
  termination was forced. The child's exit handler, whether from the request, a
  crash, or the watchdog, restores `Idle`/`spawn` and sends the single
  `list_changed`. In relay and spawner modes the call is forwarded to the app
  unchanged; a relay holds no pid, so it cannot enforce the exit.
- **Supervisor shutdown never orphans the child.** stdin EOF, SIGINT/SIGTERM,
  and a normal return all run the same shutdown: SIGTERM, grace, SIGKILL, then
  join and reap. A relayed application is not a child, so it is left running. In
  spawner mode the relay connection closing ends the process the same way and
  takes the live child with it. Signals are blocked process-wide and consumed by
  a `sigwait` thread because the handler cannot take the state or writer mutexes.
- **The supervisor reuses the library plumbing.** CLI parsing, the line
  transport, endpoint parsing and dialing, and JSON-RPC dispatch come from
  `supervisor/cli.*`, `src/transport.*`, `src/configuration.*`,
  `src/socket_connect.*`, and `src/json_rpc.*`; the supervisor links
  `sdl3mcp_supervisor_objects` and the library's SDL-free half
  `sdl3mcp_wire_objects` rather than duplicating them. The library's sources are
  split into `sdl3mcp_wire_objects` (protocol, session, framing, transports,
  configuration; no SDL symbol and no `SDL3::SDL3` on the link line) and
  `sdl3mcp_app_objects` (input, replay, capture plumbing, info; the only half
  that links SDL), and `SDL3MCP` keeps both. The proxy never links the app half
  or `SDL3::SDL3`, so a wire unit that starts calling SDL breaks the proxy link
  instead of silently dragging SDL into the relay. It never calls SDL and is
  POSIX-only (`fork`/`exec`, pipes, sockets, signals), matching the platform
  stance above.
- **fd ownership.** `NONE` leaves stdin/stdout untouched. `STDIO` duplicates the
  originals for MCP, redirects the host stdin to `/dev/null` and the host stdout
  to stderr, and restores both at shutdown. `CONNECT` dials one socket and
  duplicates nothing, so the host keeps its own stdin/stdout. stderr stays
  separate and is where diagnostics go; the host must not write stdout before
  `SDL3MCP_AppInit` in stdio mode.
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
  link the object libraries `sdl3mcp_wire_objects` and `sdl3mcp_app_objects`
  rather than the shared object. The sources are compiled once; do not add a
  second static copy for the tests or drop the hidden visibility to make them
  link.
- **The transports and stdio isolation are POSIX-only; Windows is a TODO.**
  `poll`/`read`/`write`, `dup`/`dup2`, `/dev/null`, blocking `SIGPIPE` on the
  server thread, and the socket dial/listen/accept path (`getaddrinfo`, `socket`,
  `connect`, `listen`, `accept` in `src/socket_connect.cpp` and
  `supervisor/socket_peer.cpp`) are the shipped paths. `CMakeLists.txt` rejects
  a `WIN32` configure with a `FATAL_ERROR`, and `src/transport.cpp` and
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
