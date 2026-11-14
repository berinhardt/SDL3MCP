# Architecture

SDL3MCP's internals: the seams and decisions behind the contract. `README.md` is
the contract — the CLI grammar, the MCP schemas, framing, the loop order, the
public targets, the browser arm and the limits. `STYLE.md` fixes how the code is
written; `CONTRACT.md` is the frozen v0 archive, not the current contract. Where
a fact is contract, this doc names the README section instead of restating it.

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
src/handler/Main.cpp             the Handler entry point
src/handler/HandlerCli.h/.cpp    Handler command line and usage
src/handler/Handler.h/.cpp       the poll loop, catalog, forwards, spawn, web
src/handler/AppPeer.h/.cpp       application handshake and forwarding
src/handler/Listener.h/.cpp      the application listener
src/handler/Targets.h/.cpp       --targets sources
src/handler/Web.h/.cpp           --web sources, web plan and run
src/handler/Png.h/.cpp           PNG + base64 presentation
src/handler/StbImageWrite.cpp    the one stb implementation unit
tests/                           doctest suites, check_readme.py, consumers
examples/minimal/main.c          a complete host
```

CamelCase is for C++ files; `examples/minimal/main.c` stays C.

## The wire protocol

README owns the library-side contract: §Running an application and §Platforms
and limits for *the library never listens*, *no transport enum, no library
timeout* and *one lifecycle per process*; §The browser for *one parser, two
configuration sources*; §The frame loop for *`SDL3MCP_AppIterate` is the last
call* and *the frame contract* (the `SDL3MCP_WriteFrame` signature and its
drops); §Tools for *F+1 for both blocking tools* and *`sdl_terminate` only sets a
flag*; §Capture framing for *raw in the library, image/png at the MCP edge* and
*capture region*; §Input actions for *event-based input*, *no `repeat` field*,
*key names resolve at apply time*, *text is one event* and *scroll is in
notches*. The internals behind them:

- **One protocol implementation.** `Session` parses a line, asks the catalog,
  and writes an answer through a `Stream`. The desktop `SocketStream`, the
  browser `WebSocketStream`, and the Handler's `StdioStream` all implement the
  same seam, so no code that answers a request names a file descriptor.
- **Single-threaded library.** `SDL3MCP_AppIterate` is the server loop: pump the
  stream, dispatch every available line, apply input, answer frame requests,
  complete deferred calls, flush. Nothing blocks, and every tool handler reads
  live SDL state because it runs on the app thread.
- **Frame stamping.** `SDL3MCP_WriteFrame` stamps the frame with
  `frameCounter + 1`, and a crop preserves that stamp. The app supplies the
  dimensions, so any render target is valid.

## The browser arm

README §The browser owns the contract: the `?mcp=HOST:PORT` page query and its
percent-decoding, the one configuration parser, the `ws://` scheme the peer
adds, and the WebSocket lifecycle. The arm itself is `src/WebSocketStream.cpp`,
the same `Stream` seam as the desktop `SocketStream` (see *Build composition*),
so no code above the stream names a file descriptor.

## The Handler

README owns the Handler contract: §The Handler for *the Handler is one Server
shape with an optional upstream*; §Tools and merging for *the universal tool
list and error* and *merging is by name*; §Forwarding and timeouts for *forward
deadlines follow the call*; §Spawning for *spawn-and-forget for applications*.
The internals behind them:

- **The Handler is single-threaded.** One `poll` loop over the client stream,
  the application stream, the listener, the signalfd and the web stack's pidfds.
  Writes are bounded and never block. There is no acceptor, monitor or sigwait
  thread. A pending write is polled through `Stream::writeFd()`, which is the
  socket for a socket and stdout for `StdioStream`; polling the read descriptor
  for `POLLOUT` stalls every response larger than the pipe buffer.
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
  that wrote stdout would corrupt the answers. The seeded profile turns on
  `devtools.console.stdout.content`, so the browser's `console.log` and
  `console.error` — the app's `SLP_*` output and any wasm abort — arrive on that
  stderr too.

## File descriptor ownership

- **fd ownership.** `CONNECT` dials one socket and duplicates nothing, so the
  host keeps its own stdin/stdout; `WEBSOCKET` is the browser's `CONNECT` and
  duplicates nothing. The library has no stdio transport and never owns the
  host's descriptors. The Handler's `StdioStream` owns the client-facing
  stdin/stdout for its own process. stderr stays separate and is where
  diagnostics go.

## Build composition

README owns the public build surface: §Targets for *the targets* and *a shared
build exports only the C surface*, §Requirements for the runtime dependencies
and *Linux only*, §The Emscripten build for the WebSocket suite. The internals:

- **What is compiled where.** `src/SocketStream.cpp` and `src/StdioStream.cpp`
  are desktop-only; `src/WebSocketStream.cpp` takes their place on Emscripten.
  The common object library holds the dial arm the build can compile and the
  protocol above it; `src/StdioStream.cpp` is compiled into
  `sdl3mcp_server_objects`, so the library archive holds no stdio transport.
  `src/StdioStream.cpp` is the Handler's client-facing stream; the library never
  uses it.
- **Dependencies stay SDL3-only at runtime.** `nlohmann/json` is system-first with
  the vendored header as the fallback, and is private and header-only.
  `stb_image_write.h` is vendored but used only by the Handler: one private
  implementation unit, and the library archive must contain no stb symbol.
- **Build notes.** doctest is required for the test suite; a missing doctest is
  a configure error, never a silent zero-test run. A parent that vendors doctest
  through `add_subdirectory` already defines `doctest::doctest` and needs no
  package. `readme_check` is registered only when Python 3 is found.
  `targets_check` runs only when SDL3 came from `find_package`, because a parent
  that vendors SDL leaves `SDL3_DIR` unset. There is no `tsan` preset: the
  library and the Handler are single-threaded, so there is nothing for it to
  find.

## ABI and linkage

README owns the public surface: §The library API for the six entry points, the
C11 contract and the exception boundary (also `STYLE.md` §Errors), and §Targets
for the shared build exporting only the C surface. The internals:

- **Linkage follows SDL.** Public symbols carry `SDL_DECLSPEC` and `SDLCALL`.
  The shared build defines `DLL_EXPORT`; the static build defines
  `SDL_STATIC_LIB`, including on the exported interface. Do not add a private
  dllimport/export macro.
