# AGENTS.md

SDL3MCP is a library that embeds an MCP server in any SDL3 application: public
C API, C++20 internals, a stdio server and a dialed endpoint on desktop, a
dialed WebSocket in the browser, synthetic input, and PNG frame capture.
`README.md` is the design contract and the implementation follows it: the
transports, input replay, frame pipeline, and all four MCP tools are in `src/`,
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
  presets. All four pass `-DSDL3MCP_BUILD_CLI=ON`, so every preset builds
  `sdl3mcp` and registers the supervisor tests; the `asan` suite runs
  with leak detection on (the real-example supervisor test scopes it off for
  itself and its SDL child) and `tsan` filters to `queue_tests`, `server_tests`,
  and `supervisor_tests`. The direct `cmake -S . -B build` flow stays the
  documented path and no CI workflow runs the presets.
- The unit suite is doctest. `SDL3MCP_BUILD_TESTS` defaults to on for a
  top-level configure, and a missing doctest is a configure error, never a
  silent zero-test run; a parent that vendors doctest through
  `add_subdirectory` already defines `doctest::doctest` and needs no package.
  `SDL3MCP_BUILD_TESTS` also works from a parent tree: Catalyst's desktop
  configurations build the library suite and run it from their own `ctest`.
  The supervisor half — `supervisor_png_tests`, `supervisor_tests`,
  `relay_tests`, `spawner_tests`, `supervisor_real_app` — is registered only
  under `SDL3MCP_BUILD_CLI`, which a parent build leaves off, so those still
  need a standalone configure.
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
- `SDL3MCP_BUILD_CLI=ON` builds `build/sdl3mcp`, one executable with three
  peer modes over one peer seam and a fourth that launches a web run.
  `--target <executable>` is the child supervisor: it speaks MCP on its own
  stdio and runs one target child at a time over private pipes.
  `--listen=HOST:PORT` is a relay: it speaks MCP on its own
  stdio and proxies to whichever application dials in, one session at a time.
  `--connect=HOST:PORT --targets=PATH [--listen=HOST:PORT]` is a spawner: it
  dials a relay, advertises one `app_spawn_<alias>` per targets entry, and starts
  the chosen application with `--mcp=HOST:PORT` so it dials back to a listener
  the spawner owns. `--web-root=DIR [--web-entry=NAME] [--web-port=PORT]
  [--bridge-port=PORT] [--connect=HOST:PORT] [--print-plan]` is a launcher: it
  composes, starts and supervises the page server, the WebSocket bridge and an
  isolated browser, owns no MCP session of its own and cannot be combined with
  `--target`, `--listen` or `--targets`. `--print-plan` prints the plan it would
  run, including the profile path a launch would seed, and starts nothing and
  creates no profile. The three peer modes vary only how the data-path `Peer`
  (`supervisor/peer.hpp`) is created: `stdio_peer.*` for the client's own pipes,
  `child_peer.*` for a `--target` child, and `socket_peer.*` for an accepted
  application connection (`--listen`) or the dialed relay (`--connect`). The
  proxy loop never knows which one it holds. Sources live in `supervisor/` (CLI,
  child process, peers, proxy, targets file, PNG/base64, the web plan in
  `web_app.*` and the web launch in `web_runner.*`) and its tests are
  `supervisor_png_tests` (the moved presentation code),
  `supervisor_tests` (fake MCP child over real pipes),
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
  to no server. Only the optional `sdl3mcp` relay binds a port. The enum is
  `SDL3MCP_TRANSPORT_NONE`, `SDL3MCP_TRANSPORT_STDIO`,
  `SDL3MCP_TRANSPORT_CONNECT` and `SDL3MCP_TRANSPORT_WEBSOCKET`, with
  `SDL3MCP_TRANSPORT_WEBSOCKET` appended after `CONNECT` so the two existing
  values keep their numbers. Desktop builds accept
  `NONE`, `STDIO` and `CONNECT`; an Emscripten build accepts only `NONE` and
  `SDL3MCP_TRANSPORT_WEBSOCKET`, because a page has neither a stdio server nor a
  second thread. There, `--mcp=off` and `--mcp=HOST:PORT` are the only values:
  no `stdio` value, no `--mcp-help`, no `--mcp-version`, and neither
  `usageText()` nor `versionText()` is compiled, since no command line can reach
  them.
- **One parser, two configuration sources.** Desktop reads argv; the browser
  reads `window.location.search`, takes the `mcp` parameter, percent-decodes it
  and hands the same `--mcp=<value>` argument to
  `Configuration::parseCommandLine`, so one grammar and one validator serve
  both platforms. A page without the parameter starts no server. The `ws://`
  scheme is added by the peer when it dials, never by configuration, so the
  endpoint grammar stays `HOST:PORT` (or `[V6]:PORT`) on both platforms and
  `connect_host` never carries a scheme.
- **One protocol implementation, two write paths.** `Responder` is the only code
  that parses a line, asks the session, and writes an answer; it is handed a
  `ByteSink`, which names exactly two operations, `writeLine` and `writeBytes`.
  The desktop `Transport` (an fd pair) and the browser `WebSocketPeer` both
  implement it, so the protocol is written once and no code that answers a
  request names a file descriptor.
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
  observation that still names the applied frame. A taken frame is named too:
  the answer says which frame carries the pixels, and because the capture is
  requested only after the F+1 completion that frame is always rendered after
  the actions were delivered (one acknowledgement frame and at least one capture
  frame later), so an unchanged frame means the actions had no effect. The
  browser keeps the F+1 rule but not the frame: a batch there ends with its own
  answer, and a frame is a separate `sdl_capture` (see the one-thread bullet).
- **Capture framing: raw in the library, image/png at the MCP edge.**
  On desktop `sdl_capture` and `sdl_input` are the two tools whose library
  result is not a content array: they write one result line
  `{"format":"rgba8888","width":W,"height":H,"byteLength":B,"frame":F}`
  (`sdl_input` adds `"text"`, the line naming the applied and attached frames;
  `F` is the attached frame) and then exactly `B`
  tightly packed RGBA bytes, with no newline and no base64. Only `sdl_capture`
  does that on the browser, because a batch there attaches no frame.
  `region` stays in the library; PNG and base64 are supervisor policy, and the
  supervisor encodes the frame at exactly the dimensions it received, so frames
  are never resized. The supervisor reads that framing, validates the header
  and payload size, and returns the standard
  `{"type":"image","mimeType":"image/png","data":"<base64>"}` content block,
  which stays the client-facing contract. A supervisor in front of another
  supervisor relays that already-presented `content` result untouched; only a
  raw `format`/`width`/`height`/`byteLength` frame is encoded, and exactly once,
  at the hop nearest the application. Do not reintroduce a
  custom client, a non-standard content type, or PNG encoding in the library.
- **Frame contract.** `SDL3MCP_WriteFrame(data, size, width, height)` accepts
  only tightly packed RGBA8888 with `size == width * height * 4`, top-left
  origin, no row padding and no pitch parameter. It stamps the frame with
  `frameCounter + 1`, the index of the frame whose pixels it holds, and a crop
  preserves that stamp. That index travels in the raw header as `frame`, so the
  supervisor's summary and `sdl_input`'s text can name the attached frame. The
  raw header also carries `format`/`width`/`height`/`byteLength`, and nothing
  about the JSON-RPC envelope changes. The app supplies the
  dimensions, so any render target is valid and the library never reads the
  window size to validate a frame. The app converts and packs when a readback
  surface is padded. The server thread crops the optional region and writes the
  raw framing; on the browser the app thread crops it when the deferred capture
  settles. PNG encoding and base64 happen only in the supervisor. Drops
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
- **Requests are serialized on desktop.** One server thread handles one message
  at a time, so a blocking `sdl_input`/`sdl_capture` delays later requests,
  including `ping`, by up to its timeout (for `sdl_input`, the whole
  apply-plus-frame budget).
- **The browser session is one thread, so it answers later.** Emscripten has no
  `Server` and no second thread: `WebSocketPeer` dials with
  `emscripten_websocket_new`, its callbacks copy each frame into a byte queue,
  and `pump()` hands the complete lines to the `Responder`, whose answers leave
  through `emscripten_websocket_send_binary`. Callbacks, `pump()` and every
  write share the page's main thread, which is why the queue needs no lock.
  `SDL3MCP_AppIterate` pumps at the top of the frame, before the frame counter,
  so a request registered there is answered by a frame the loop has not produced
  yet; it then completes a deferred call after the frame work, so a capture
  carries the frame the app answered with. `sdl_capture` and `sdl_input` never
  wait: they register the request (`CaptureState::begin`, `InputQueue::post`)
  and return a result marked deferred, which `Responder` leaves unanswered. A
  later frame settles it and writes the answer through `Responder::answer`, with
  `frame_timeout_ms` as the deadline, and one deferred call at a time so a
  capture and a batch never race for the same frame. The reason is structural
  and it is the point of the design: there is one thread, so waiting for the app
  to render or apply would be waiting for itself. Nothing on this path may
  block — not the pump, not a handler, not a write.
- **What is compiled where.** `src/server.cpp`, `src/transport.cpp`,
  `src/stdio_isolation.cpp` and `src/socket_connect.cpp` are desktop-only;
  `src/websocket_peer.cpp` takes their place on Emscripten. The wire object
  library holds whichever arm the build can compile, and the protocol above it
  is shared. The entry points gate on the endpoint this build has:
  `SDL3MCP_FrameRequested` asks the desktop `Server`, or on the browser the
  `Responder` and a live peer, so a peer that closed cannot answer a frame.
- **The supervisor's forward deadline follows the tool.** A forwarded call that
  carries `timeout_ms` gets that budget plus a grace; without one, `sdl_input`
  gets twice the default frame timeout because it waits for the apply and then
  for the frame, while every other call gets one frame timeout. One forward path
  serves all tools: a result with `content` is relayed verbatim, a result with
  `byteLength` is read, PNG-encoded, and answered as text plus one image block
  (the frame's own `text` when it has one, the capture summary otherwise). An
  application configured with a larger `frame_timeout_ms` should pass
  `timeout_ms` explicitly.
- **`sdl_terminate` only sets a flag.** The handler stores the request and
  returns `termination requested`; it never calls SDL. On desktop that handler
  runs on the server thread; on the browser the same handler runs on the app
  thread during the pump. `SDL3MCP_AppIterate` exchanges the flag and pushes
  exactly one zeroed `SDL_EVENT_QUIT`, restoring
  the flag to retry when `SDL_PushEvent` fails. It still returns
  `SDL_APP_CONTINUE` and leaves the app's own quit handling untouched.
- **The supervisor is one executable with three peer modes and one state
  machine.** Child mode (`--target`) speaks MCP on its own stdio and runs one
  target child at a time. Relay mode (`--listen`) speaks MCP on its own stdio and
  waits for an application to dial in. Spawner mode (`--connect --targets`)
  speaks MCP to the relay it dialed and starts the chosen application on demand.
  The web mode is the exception: it has no MCP session and no state machine of
  its own, it only launches and supervises the three processes of a web run (see
  the web bullets below). The advertised tool list is a function of the peer
  mode, never of whether an application is attached: child mode advertises
  `spawn` plus the application's four tools,
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
- **The web spawner launches three processes and owns their lifetime.**
  `--web-root=DIR [--web-entry=NAME] [--web-port=PORT] [--bridge-port=PORT]
  [--connect=HOST:PORT] [--print-plan]` (see `supervisor/cli.hpp`) composes the
  run from `web_app.*` and supervises it in `web_runner.*`. `--web-entry`
  defaults to `catalyst.html`, the page port to 18080 and the bridge port to
  18081, and `--connect` names the relay the bridge dials, defaulting to
  `127.0.0.1:7777`. Web mode owns the whole launch and cannot be combined with
  `--target`, `--listen` or `--targets`. The plan, in launch order, is:
  `python3 -m http.server PAGE_PORT --bind 127.0.0.1 --directory ROOT`,
  `websocat --binary ws-l:127.0.0.1:BRIDGE_PORT tcp:RELAY`, and Firefox on
  `http://127.0.0.1:PAGE_PORT/ENTRY?mcp=127.0.0.1:BRIDGE_PORT`, which is exactly
  the query the page's configuration reads. `--print-plan` prints `browser=`,
  `profile=` and one `command:` line per child and starts nothing; the same
  `makeLaunchPlan` composes both, so what a review prints is what a launch runs.
  It seeds no profile: `plannedProfilePath` reports the `sdl3mcp-web-XXXXXX`
  template a launch would fill, because only the run that starts children owns a
  directory to remove.
  Starting follows the plan: page server and bridge first, then up to a
  one-second wait for both loopback ports to accept a connection, then the
  browser. A child that exits during that wait fails the run and is named; an
  expired wait only warns and starts the browser against a server that is still
  coming up.
- **A web launch checks its inputs before it forks anything.** `websocat`
  (install it with `cargo install websocat`) and `python3` must be on `PATH`,
  and the page root must be a directory that holds the entry, so a missing tool
  is one sentence instead of a half-launched stack holding a bound port. The
  browser is resolved by `resolveBrowser`: `$BROWSER` when it names `firefox` or
  `firefox-esr`, otherwise `firefox` then `firefox-esr` on `PATH`, and no
  fallback at all, because any other browser would open the page in the user's
  own session instead of the isolated one.
- **The browser runs in a throwaway profile, and that is not optional.** The
  browser is started with `--profile` pointing at a fresh
  `sdl3mcp-web-XXXXXX` directory under `$XDG_RUNTIME_DIR` (or `/tmp`) together
  with `--no-remote --new-instance`, so the run can neither inherit nor join an
  already running browser. `seedProfile` writes the `user.js` that carries
  `dom.allow_scripts_to_close_windows`, which is what lets the page close its
  own window when the application shuts down, plus the preferences that keep a
  fresh profile from stopping on first-run prompts. That preference can only be
  set in a profile we own, never in the user's settings; the directory is
  removed when the run ends.
- **A web run ends in one chain, and the application starts it.**
  `sdl_terminate` reaches the app through the relay and the bridge, the app's
  loop handles the `SDL_EVENT_QUIT` it produces, and `SDL3MCP_AppShutdown` runs
  `window.close()`; the browser then exits with status 0, which the launcher
  reads as a clean end — the page closed itself, so nothing failed. It stops the
  rest with SIGTERM to their process groups, waits one grace period, escalates
  to SIGKILL for anything still alive, removes the throwaway profile, and exits
  0. A browser that exits non-zero, and a page server or bridge that exits at
  any point, fail the run with a message naming the child, and exit 1 after the
  same stop and removal; a tool's death is never the intended end. When Firefox
  refuses to close the window, the run does not end on its own; a stop request
  then takes the browser down through that same grace-then-SIGKILL path. The
  preference above is what makes the normal case a page that closes itself.
  SIGINT and SIGTERM take the same stop path with a clean outcome.
- **fd ownership.** `NONE` leaves stdin/stdout untouched. `STDIO` duplicates the
  originals for MCP, redirects the host stdin to `/dev/null` and the host stdout
  to stderr, and restores both at shutdown. `CONNECT` dials one socket and
  duplicates nothing, so the host keeps its own stdin/stdout. `WEBSOCKET` is the
  browser's `CONNECT`: it dials the page's WebSocket and duplicates nothing,
  because a page has no stdin, stdout or descriptor to isolate. stderr stays
  separate and is where diagnostics go; the host must not write stdout before
  `SDL3MCP_AppInit` in stdio mode.
- **Threading.** The desktop server thread never calls SDL: geometry is
  snapshotted on the app thread, and frames and input cross through
  queues/copies. The browser build has no server thread at all — its transport
  callbacks, the queue copy and the pump all run on the app thread, which is why
  nothing on that path may block and why a tool call that needs the app is
  answered by a later frame.
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
- **The fd transports, stdio isolation and socket dial are desktop-only;
  Windows is a TODO.** A browser has no descriptor and no second thread, so the
  Emscripten arm replaces all of them with `src/websocket_peer.cpp` and answers
  from the frame loop. `poll`/`read`/`write`, `dup`/`dup2`, `/dev/null`,
  blocking `SIGPIPE` on the server thread, and the socket dial/listen/accept
  path (`getaddrinfo`, `socket`, `connect`, `listen`, `accept` in
  `src/socket_connect.cpp` and `supervisor/socket_peer.cpp`) are the shipped
  desktop paths. `CMakeLists.txt` rejects a `WIN32` configure with a
  `FATAL_ERROR`, and `src/transport.cpp` and
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
