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

`README.md` is the single source of truth for the public contract; this file
covers the internals and the workflow. `CONTRACT.md` is the frozen v0 behaviour
record kept as an archive, not the current contract. `STYLE.md` fixes how the
code is written.

This is release 1.0.0. The v0 tree was deleted in the rewrite; it survives in
`v0.1.0` and history.

## Build, test, verify

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSDL3MCP_BUILD_EXAMPLES=ON -DSDL3MCP_BUILD_CLI=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`SDL3MCP_BUILD_CLI=ON` builds the Handler and registers its suites; without it
the library, tests and examples still build. The direct flow is the documented
path.

Presets wrap the same configurations:

```sh
cmake --preset debug  && cmake --build --preset debug  && ctest --preset debug
cmake --preset shared && cmake --build --preset shared && ctest --preset shared
cmake --preset asan   && cmake --build --preset asan   && ctest --preset asan
```

All three turn on the tests, the examples and the Handler. `asan` runs with leak
detection on; the dummy-video-driver suites scope it off for themselves because
SDL keeps process-wide allocations they cannot free. There is no `tsan` preset:
the library and the Handler are single-threaded, so there is nothing for it to
find.

- **doctest** is required for the test suite; a missing doctest is a configure
  error, never a silent zero-test run. A parent that vendors doctest through
  `add_subdirectory` already defines `doctest::doctest` and needs no package.
- **Transport tests and spawn harnesses** follow the payload-floor and
  drained-child-output rules in `STYLE.md`; a reviewer can cite them there.
- **Python 3** is required for `readme_check`, which CTest registers when it is
  found. The check compiles the README's two complete C examples, parses every
  JSON block, compares the documented schemas with the Handler's `--dump-tools`
  catalog, and checks the framing prose, the `sdl_input` text, the loop order and
  the run command. Update it when the contract changes; never weaken it.
- **Node.js** plus Emscripten are required only for
  `SDL3MCP_BUILD_NODE_TESTS=ON`, which builds the WebSocket suite and runs it
  under Node. The Emscripten configure compiles `src/WebSocketStream.cpp` in
  place of `src/SocketStream.cpp` and `src/StdioStream.cpp`.
- `targets_check` configures a parent project and asserts the three public
  target names; it runs only when SDL3 came from `find_package`, because a parent
  that vendors SDL leaves `SDL3_DIR` unset.

## File layout

```text
include/SDL3MCP/SDL3MCP.h        the complete public C surface
include/SDL3MCP/SDL3MCP_version.h.in
src/Api.cpp                      the C ABI over Runtime
src/Runtime.h/.cpp               process-wide state; start/iterate/shutdown
src/Configuration.h/.cpp         --mcp grammar, page query, endpoint parsing
src/JsonRpc.h/.cpp               one parsed request/answer
src/Session.h/.cpp               one peer: initialize, ping, tools/list, calls
src/Tool.h/.cpp                  name, description, schema, invoke
src/ToolCatalog.h/.cpp           map<name, Tool>
src/ToolDefinitions.h/.cpp       the four application tools' metadata
src/ToolResult.h/.cpp            content or raw payload, self-encoded
src/Stream.h/.cpp                Stream + StreamBase (line/payload buffer)
src/SocketStream.h/.cpp          native dial
src/WebSocketStream.cpp          Emscripten dial (same header)
src/StdioStream.h/.cpp           Handler's client-facing stream
src/Pipeline.h/.cpp              the app-thread input/capture pipeline
src/Input.h/.cpp                 action model and parsing
src/InputJson.h                  JSON -> actions
src/KeyMap.h/.cpp                key names and chords
src/Replay.h/.cpp                held state and event replay
src/Mouse.h/.cpp                 pointer normalization
src/AppInput.h/.cpp              batch -> events
src/Frame.h/.cpp                 validate, crop, frame order
src/Capture.h/.cpp               capture statistics and regions
src/Info.h/.cpp                  sdl_info value and serializer
server/Main.cpp                  the Handler entry point
server/Cli.h/.cpp                Handler command line and usage
server/Handler.h/.cpp            the poll loop, catalog, forwards, spawn, web
server/AppPeer.h/.cpp            application handshake and forwarding
server/Listener.h/.cpp           the application listener
server/Targets.h/.cpp            --targets sources
server/Web.h/.cpp                --web sources, web plan and run
server/Png.h/.cpp                PNG + base64 presentation
server/StbImageWrite.cpp         the one stb implementation unit
tests/                           doctest suites, check_readme.py, consumers
examples/minimal/main.c          a complete host
```

CamelCase is for C++ files; `examples/minimal/main.c` stays C.

## Decisions not to regress

- **The library never listens.** A Session exists exactly when `connect_host`
  names an endpoint. `--mcp=off` clears it and `--mcp=HOST:PORT` sets it; there is
  no bare `--mcp`, no stdio transport and no fallback to no session when a dial
  fails. Only the Handler binds a port.
- **No transport enum, no library timeout.** `SDL3MCP_Configuration` has no
  `transport` and no `frame_timeout_ms`. The only library bound is
  `connect_timeout_ms` (1..60000); the library never expires a capture or an
  input request. The Handler's `--timeout-ms` and tool `timeout_ms` are the
  forward budget, and `sdl_info.limits` carries only `maxFrameBytes`.
- **One parser, two configuration sources.** Desktop reads argv; the browser
  reads `window.location.search`, takes the `mcp` parameter, percent-decodes it
  and hands the same `--mcp=<value>` argument to `Configuration::parseCommandLine`.
  A page without the parameter starts no session. The `ws://` scheme is added by
  the Emscripten peer, never by configuration.
- **One protocol implementation.** `Session` parses a line, asks the catalog,
  and writes an answer through a `Stream`. The desktop `SocketStream`, the
  browser `WebSocketStream`, and the Handler's `StdioStream` all implement the
  same seam, so no code that answers a request names a file descriptor.
- **One lifecycle per process.** A second `SDL3MCP_AppInit` fails even after
  shutdown; `SDL3MCP_AppShutdown` is idempotent; a failed init still claims the
  lifecycle.
- **`SDL3MCP_AppIterate` is the last call of the frame**, after drawing, after
  `SDL_RenderPresent`, and after answering `SDL3MCP_FrameRequested`.
- **Single-threaded library.** `SDL3MCP_AppIterate` is the server loop: pump the
  stream, dispatch every available line, apply input, answer frame requests,
  complete deferred calls, flush. Nothing blocks, and every tool handler reads
  live SDL state because it runs on the app thread.
- **F+1 for both blocking tools.** A request registered at frame F is served by
  the frame rendered at F+1. `sdl_input` applies its batch at F, the app consumes
  the events during F+1, and the call returns during F+1 with that frame. Its
  text is `<N> actions applied at frame F; frame F+1 attached, rendered after the
  actions were delivered`. `sdl_capture` is a no-op input: it registers a request
  and shares the next frame with any input registered at the same time. An
  unchanged frame means the actions had no effect, not that the observation is
  stale.
- **Capture framing: raw in the library, image/png at the MCP edge.**
  `sdl_capture` and `sdl_input` write one result line
  `{"format":"rgba8888","width":W,"height":H,"byteLength":B,"frame":F}`
  (`sdl_input` adds `"text"`), then exactly `B` tightly packed RGBA bytes with no
  newline and no base64. `region` stays in the library. PNG and base64 are
  Handler policy, encoded once at the hop nearest the application; a Handler in
  front of another Handler relays an already-presented `content` result
  untouched. Frames are never resized.
- **Frame contract.** `SDL3MCP_WriteFrame(data, size, width, height)` accepts only
  tightly packed RGBA8888 with `size == width * height * 4`, top-left origin, no
  row padding, no pitch parameter. It stamps the frame with `frameCounter + 1`,
  and a crop preserves that stamp. The app supplies the dimensions, so any render
  target is valid. Drops (size mismatch, `max_frame_bytes`, null data,
  non-positive dimension) leave the request pending and surface in
  `capture.dropped`/`capture.lastError` and `sdl_info`.
- **Capture region.** `region` is `{x, y, width, height}` in pixels of the
  captured frame, top-left origin, all four required, and must fit inside
  `capture.lastWidth` by `capture.lastHeight`. `sdl_input` parses it with the
  same helper, so both tools accept exactly the same object.
- **Input is event-based.** Actions become `SDL_PushEvent` traffic applied on the
  app thread. `SDL_GetKeyboardState` cannot be updated because SDL3 exposes no
  per-key setter; that is a documented limitation.
- **No `repeat` field.** Taps are repeated `press` actions; auto-repeat is a
  repeated `down` without an intervening `up`, and that event's `repeat` flag is
  set. Modifiers are valid only on `press`.
- **Key names resolve at apply time.** Names are SDL scancodes and aliases with
  `-`/`_` as word separators; single characters go through
  `SDL_GetScancodeFromKey` on the app thread. Unknown key or chord errors are
  reported at F+1; structural errors fail before queueing.
- **Text is one event.** A `text` action pushes one `SDL_EVENT_TEXT_INPUT` with
  the whole UTF-8 string, and the event's pointer is valid only until F+1. Text
  input is started just in time and restored afterwards. Do not reintroduce
  chunking.
- **Scroll is in notches.** `1.0` is one notch; integer wheel fields stay 0.
- **`sdl_terminate` only sets a flag.** The handler stores the request and
  returns `termination requested`; `SDL3MCP_AppIterate` exchanges the flag and
  pushes exactly one zeroed `SDL_EVENT_QUIT`, restoring the flag when the push
  fails. It still returns `SDL_APP_CONTINUE` and leaves the app's own quit
  handling untouched.
- **The Handler is one Server shape with an optional upstream.** Server mode is
  stdio; `--connect` makes it a Relay. `--targets` and `--web` are app sources;
  `--listen` defaults to `127.0.0.1:0` when a source is present and is required
  otherwise. `--dry-run=ALIAS` prints what a spawn would run and starts nothing.
- **Universal tool list and error.** `wait_session` + the four app tools + one
  `app_spawn_<alias>` per source, independent of whether an application is
  attached. Before attach, app calls answer
  `no application session is attached; call wait_session first`. A forwarded tool
  the peer did not advertise answers `the application does not provide <name>`.
- **Merging is by name.** The Handler merges the attached peer's entries over
  its base; `sdl_capture` keeps the Handler's definition because it owns
  presentation. A spawn, attach or detach changes no advertised name, so no
  `list_changed` is sent for it; a notification is sent only when the merged set
  really changes.
- **The Handler is single-threaded.** One `poll` loop over the client stream,
  the application stream, the listener, the signalfd and the web stack's pidfds.
  Writes are bounded and never block. There is no acceptor, monitor or sigwait
  thread. A pending write is polled through `Stream::writeFd()`, which is the
  socket for a socket and stdout for `StdioStream`; polling the read descriptor
  for `POLLOUT` stalls every response larger than the pipe buffer.
- **Forward deadlines follow the call.** A call carrying `timeout_ms` gets that
  budget plus a 500 ms grace; without one, every forwarded call gets the
  Handler's `--timeout-ms` budget, and `wait_session`/`app_spawn_*` default to
  it. One forward path serves all tools: `content` is relayed verbatim, a raw
  frame is PNG-encoded and answered as text plus one image block.
- **Spawn-and-forget for applications.** The Handler does not signal, wait for,
  or act on a spawned application, and does not kill it on exit; each app owns
  its cleanup. The poll loop reaps every exited child so none becomes a zombie,
  but it ignores a spawned app's status. The web stack is the exception: the
  Handler owns the page server, bridge and browser, stops them, removes the
  throwaway profile, and reports the browser's non-zero exit status.
- **A web run is one lifetime.** `--web=PATH` maps aliases to page roots; the
  entry is always `index.html`. The run preflights `python3`, `websocat`, Firefox
  and the root before forking, picks ephemeral ports, seeds and locks a
  `sdl3mcp-web-XXXXXX` profile under `$XDG_RUNTIME_DIR` or `/tmp`, and starts the
  page server, the bridge, then the browser. A tool's death or a browser exit
  stops the rest with SIGTERM, one grace period and SIGKILL, removes the profile,
  and reports the child. The profile lock is what lets a later launch tell a live
  profile from an abandoned one; the sweep runs only in the launch path, never in
  a plan print. The three children get `/dev/null` on stdin and their stdout on
  the Handler's stderr, because the Handler's own stdin/stdout is the client's
  MCP channel in server mode: a child that read stdin would eat requests and one
  that wrote stdout would corrupt the answers.
- **What is compiled where.** `src/SocketStream.cpp` and `src/StdioStream.cpp`
  are desktop-only; `src/WebSocketStream.cpp` takes their place on Emscripten.
  The common object library holds the dial arm the build can compile and the
  protocol above it; `src/StdioStream.cpp` is compiled into
  `sdl3mcp_server_objects`, so the library archive holds no stdio transport.
  `src/StdioStream.cpp` is the Handler's client-facing stream; the library never
  uses it.
- **fd ownership.** `CONNECT` dials one socket and duplicates nothing, so the
  host keeps its own stdin/stdout; `WEBSOCKET` is the browser's `CONNECT` and
  duplicates nothing. The library has no stdio transport and never owns the
  host's descriptors. The Handler's `StdioStream` owns the client-facing
  stdin/stdout for its own process. stderr stays separate and is where
  diagnostics go.
- **Public surface.** The six entry points plus the header-inline
  `SDL3MCP_DefaultConfiguration()`. The C API is C11 and never throws; C++
  exceptions stop at the boundary and become `SDL_APP_FAILURE` plus
  `SDL_GetError`.
- **Linkage follows SDL.** Public symbols carry `SDL_DECLSPEC` and `SDLCALL`.
  The shared build defines `DLL_EXPORT`; the static build defines
  `SDL_STATIC_LIB`, including on the exported interface. Do not add a private
  dllimport/export macro.
- **A shared build exports only the C surface.** Every object library is compiled
  with `CXX_VISIBILITY_PRESET hidden` and `VISIBILITY_INLINES_HIDDEN`; only the
  C entry points are visible. Unit tests link the object libraries
  (`sdl3mcp_common_objects`, `sdl3mcp_app_objects`, `sdl3mcp_server_objects`),
  never a second static copy of the sources.
- **The targets.** `SDL3MCP::SDL3MCP` is the library and the only target the
  installed package exports; `SDL3MCP::SERVER` is the `sdl3mcp` executable and
  is defined exactly when `SDL3MCP_BUILD_CLI=ON` on a desktop configure (there
  is no Emscripten Handler); `SDL3MCP::COMMON` is the SDL-free core both
  compose. The object libraries stay internal.
- **Linux only.** The Handler uses POSIX `poll`, `signalfd`, `pidfd`,
  `fork`/`exec` and process groups. A `WIN32` configure stops with a
  `FATAL_ERROR`; the Windows TODO is binary stdio and a transport wake-up. Do not
  add untested Win32 code.
- **Dependencies stay SDL3-only at runtime.** `nlohmann/json` is system-first with
  the vendored header as the fallback, and is private and header-only.
  `stb_image_write.h` is vendored but used only by the Handler: one private
  implementation unit, and the library archive must contain no stb symbol.

## Working on the spec

- `README.md` is the source of truth for the API contract, CLI grammar, MCP
  schemas, framing and threading rules.
- Wire names are snake_case (`max_frame_bytes`); C++ names are camelCase; types
  are PascalCase; files are CamelCase.
- Change the implementation and the matching README section together, including
  the tool JSON schemas when a parameter changes.
- `tests/check_readme.py` enforces the two complete examples, the JSON blocks,
  the tool schemas, the `Example:` descriptions, the framing prose, the
  `sdl_input` text, the `SDL3MCP_WriteFrame` signature, the run command and the
  loop order. Update it when the contract changes rather than weakening it.
- `CHANGELOG.md` records breaking changes; add an entry for every user-visible
  break, not for internal refactors.

## Editing etiquette

- Minimal diffs. Do not reformat untouched code. `.clang-format` (Google base,
  3-space indent, no column limit, left-aligned pointers) applies to touched
  files; run it rather than restating it.
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

- Did this session avoid asking how to build or run? It should — the commands
  above.
- Did anything touch `third_party/`? It should not have.
- Is the diff minimal, with no wholesale reformatting?
- Were the library and the Handler kept free of threads?
- Did the shared build still export only the C API?
- Do `ctest` (debug, shared, asan) and `readme_check` pass?
