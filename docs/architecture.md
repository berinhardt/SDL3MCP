# Architecture

SDL3MCP's internals: the seams and decisions behind the contract. `README.md` is
the contract — the CLI grammar, the MCP schemas, framing, the loop order, the
public targets, the browser arm and the limits. `STYLE.md` fixes how the code is
written; `CONTRACT.md` is the frozen v0 archive, not the current contract. Where
a fact is contract, this doc names the README section instead of restating it.

## File layout

```text
include/SDL3MCP.h                the complete public C surface: SDL, the types
include/SDL3MCP_types.h          the SDL-free configuration and release macros
include/SDL3MCP_version.h.in     the generated release macros' template

src/common/                      the SDL-free core: everything the library and
                                 the Handler both compile
  Configuration.h                --mcp grammar and configuration interface
  Configuration.cpp              configuration value and validation
  Configuration-Endpoint.cpp     endpoint and page-query grammar
  Configuration-Arguments.cpp    --mcp argument application
  Configuration-Usage.cpp        usage and version text
  Configuration-Limits.h         shared decimal parser and field bounds
  JsonRpc.h/.cpp                 one parsed request/answer
  Posix.h                        the shared descriptor helpers
  Result.h                       Result<T, E> for both halves
  Session.h/.cpp                 one peer: initialize, ping, tools/list, calls
  Stream.h/.cpp                  Stream + StreamBase (line/payload buffer)
  SocketStream.h/.cpp            native dial
  SocketStream-Websockets.cpp    Emscripten dial (same header)
  SocketStream-WSEvents.cpp      browser callbacks and byte queue
  SocketStream-Websockets.h      the browser arm's PIMPL state and close codes
  Tool.h                         name, description, schema, invoke
  ToolCatalog.h/.cpp             map<name, Tool>
  ToolDefinitions.h/.cpp         the four application tools' metadata
  ToolResult.h/.cpp              content or raw payload, self-encoded

src/lib/                         the library's SDL half
  Api.cpp                        the C ABI over Runtime
  Runtime.h/.cpp                 process-wide state; start/iterate/shutdown
  Pipeline.h/.cpp                the app-thread input/capture pipeline
  TextInputScope.h/.cpp          text input for the lifetime of one batch
  Input.h                        action model, validation and chord interface
  Input.cpp                      model validation
  Input-Modifiers.cpp            modifier names and bits
  Input-Chord.cpp                key and chord resolution
  InputArguments.h/.cpp          JSON -> actions, the entry point
  InputProperties.h/.cpp         closed-schema property readers
  InputActions.h/.cpp            action dispatch and text/release parsers
  InputKeyboard.h/.cpp           key action parser
  InputPointer.h/.cpp            mouse and scroll action parsers
  KeyMap.h/.cpp                  key names and chords
  Replay.h/.cpp                  held state and event replay
  Mouse.h/.cpp                   pointer normalization
  AppInput.h/.cpp                batch -> events
  Frame.h/.cpp                   validate, crop, frame order
  Capture.h/.cpp                 capture statistics and regions
  Info.h/.cpp                    sdl_info value and serializer

src/handler/                     the Handler binary
  Main.cpp                       the Handler entry point
  HandlerCli.h/.cpp              Handler command line and usage
  HandlerCli-Endpoint.h/.cpp     the --listen/--connect address arguments
  HandlerCli-Usage.cpp           the usage block main() prints
  Handler.h                      the one Handler class: all loop state
  Handler-Lifecycle.cpp          construction, client/listener startup, shutdown
  Handler-Loop.cpp               the poll loop and its dispatch
  Handler-Catalog.h              the Handler's tool text
  Handler-Catalog.cpp            tool sources and the base tools
  Handler-Merge.cpp              catalog merge and tools/list publication
  Handler-Attach.cpp             listener accept and app attach/change/detach
  Handler-Wait.cpp               wait_session and its waiters
  Handler-Forward.cpp            forwards and the application's answers
  Handler-Present.cpp            a raw frame as a PNG image block
  Handler-Deadline.cpp           the next poll deadline
  Handler-Spawn.cpp              target spawn and the exec channel
  Handler-Reaper.cpp             child reaping and pending-spawn completion
  Handler-Web.cpp                web spawn and the run's service
  Handler-DryRun.cpp             the --dry-run argv print and shell quoting
  StdioStream.h/.cpp             the Handler's client-facing stream
  ToolArguments.h                the shared timeout_ms helpers
  AppPeer.h/.cpp                 the application session and its lifecycle
  AppPeer-Protocol.cpp           the handshake, tool list and event dispatch
  AppPeer-Protocol.h             the protocol constants both units share
  Listener.h/.cpp                the application listener
  Targets.h/.cpp                 --targets sources
  Web-Apps.h/.cpp                --web sources and the alias dictionary
  Web-Plan.h                     the web launch options and plan declarations
  Web-Plan.cpp                   plan composition
  Web-Plan-Preflight.cpp         preflight, browser lookup, ports
  Web-Plan-Profile.cpp           profile seed, lock and prepare
  Web-Plan-Sweep.cpp             abandoned-profile sweep and tree removal
  Web-Run.h                      WebRun and its events
  Web-Run.cpp                    the run's lifetime and stop
  Web-Run-Child.cpp              child launch, pidfd watch, readiness probe
  Web-Run-Service.cpp            poll set, exit recording, ready deadline
  Png.h/.cpp                     PNG + base64 presentation
  StbImageWrite.cpp              the one stb implementation unit

tests/                           doctest suites, check_readme.py, consumers
```

A unit lives in the directory of its half: `src/common/` is the SDL-free core
both consumers compile, `src/lib/` is the library's SDL half, and
`src/handler/` is the binary. Each directory is an include root by bare name
(`src/common`, `src/lib`, `src/handler`); no target takes `src/` as a catch-all,
so a unit that reaches across halves names the directory it reaches into.
`src/common/` is where a header both halves need lives — `Result.h`, `Posix.h`,
`Tool.h` — because it is the only directory the Handler can reach without SDL.

C++ file names are CamelCase with a hyphen between compound words (`STYLE.md`
§Files states the rule and its reason).

C++ file names are CamelCase with a hyphen between compound words (`STYLE.md`
§Files states the rule and its reason).

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
  browser `SocketStream-Websockets`, and the Handler's `StdioStream` all
  implement the same seam, so no code that answers a request names a file
  descriptor.
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
adds, and the WebSocket lifecycle. The arm is `src/common/SocketStream-Websockets.cpp`
(the dial, the writes and pump) with `src/common/SocketStream-WSEvents.cpp` (the
callbacks and the byte queue they fill), the same `Stream` seam as the desktop
`SocketStream` (see *Build composition*), so no code above the stream names a
file descriptor.

Two things are easy to misread there:

- **A window is needed for capture and info, not to run.** Nothing in the app
  half requires a bound window to pump the session: without one `sdl_capture`
  refuses with `no window bound`, and `sdl_info` omits `videoDriver` and the
  window block. The app-path suite therefore creates a real window with SDL's
  dummy driver rather than faking one, so `SDL3MCP_SetWindow`'s measurement and
  the window block of `sdl_info` are exercised too.
- **The WebSocket is on the main thread because there is only one, not because
  of a field.** Emscripten ignores `createOnMainThread` — its own WebSocket
  library marks the field a TODO and creates every connection on the main
  thread. Under Node there is one thread at all, so the callbacks and `pump()`
  share it and the queue needs no lock.

The Node suites cover the app half, not the browser. `node_web_app_tests` runs
the real archive with the dummy video driver, and a test-only `--post-js` file
supplies `window.location.search` and `window.close()` — the only two browser
names `src/lib/Api.cpp` reads. The Emscripten video driver (canvas, WebGL, DOM input,
the theme listener), the browser's WebSocket, a real page's query and close
semantics, and the Handler's web stack stay with the manual `--web` run; README
§Tests lists what each covers.

## The Handler

README owns the Handler contract: §The Handler for *the Handler is one Server
shape with an optional upstream*; §Tools and merging for *the universal tool
list and error* and *merging is by name*; §Forwarding and timeouts for *forward
deadlines follow the call*; §Spawning for *spawn-and-forget for applications*.
The internals behind them:

- **The Handler is single-threaded.** One `poll` loop over the client stream,
  the application stream, the listener, the signalfd and the web stack's pidfds.
  Writes are bounded and never block. There is no acceptor, monitor or sigwait
  thread. A pending write is polled through `Stream::outputFd()`, which is the
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

## Layering

Layering is a design constraint, not a style rule: `STYLE.md` says what code
looks like, and this section says which objects may be compiled together.

- **`SDL3MCP::COMMON` is the intersection of the objects the library and the
  Handler both compile.** Those objects are `Configuration`, `JsonRpc`,
  `Session`, `Stream`, `ToolCatalog`, `ToolDefinitions`, `ToolResult`, and the
  transport arm the build can compile — `SocketStream` natively,
  `SocketStream-Websockets` on Emscripten. `StdioStream` is Handler-only, because
  the library never writes to stdio. A unit that both consumers compile belongs
  to `COMMON`; a unit that only one compiles does not. It is a real static
  library built from the common object library, not an interface shim, so the
  intersection is visible as a target rather than a convention.
- **`COMMON` compiles no SDL.** No file compiled into `COMMON` includes SDL,
  directly or through a header of its own. The configuration type is what makes
  the Handler side workable: it names no SDL type, so a unit that only builds
  configuration needs no SDL header, and the Handler's build can go without
  SDL's include path entirely.
- **The constraint enforces itself.** With no SDL include path on the Handler, a
  stray SDL include in a common file fails the Handler build rather than passing
  review and failing later, so no check script has to restate the rule.

## Build composition

README owns the public build surface: §Targets for *the targets*, *the library
is static only and publishes no SDL* and *the consumer link line*, §Requirements
for the runtime dependencies and *Linux only*, §The Emscripten build for the two
Node suites. The internals:

- **What is compiled where.** `src/common/SocketStream.cpp` and
  `src/handler/StdioStream.cpp` are desktop-only;
  `src/common/SocketStream-Websockets.cpp` and
  `src/common/SocketStream-WSEvents.cpp` take their place on Emscripten. The
  common object library holds the dial arm the build can compile and the
  protocol above it; `src/handler/StdioStream.cpp` is compiled into
  `sdl3mcp_handler_objects`, so the library archive holds no stdio transport.
  `src/handler/StdioStream.cpp` is the Handler's client-facing stream; the
  library never uses it.
- **Dependencies stay SDL3-only at runtime.** `nlohmann/json` resolves in three
  steps — an existing target, then its package, then a pinned `v3.12.0` download
  — and is private and header-only; there is no vendored copy. `stb_image_write.h`
  is vendored but used only by the Handler: one private implementation unit, and
  the library archive must contain no stb symbol.
- **SDL is compiled in, never published.** The app half links `SDL3::SDL3` to
  compile, and the Handler compiles with no SDL include path at all. The
  `SDL3MCP` target therefore names no SDL target in its interface: on a static
  library even a `PRIVATE` dependency is exported as `$<LINK_ONLY:...>`, so the
  link would still cross to every consumer. The app names SDL itself, and the
  Emscripten consumer adds `-lwebsocket.js`.
- **Build notes.** doctest belongs to both test groups and resolves in three
  steps — an existing target, then its package, then a pinned `v2.5.3` download —
  so a missing package is never a silent zero-test run. The tests are split by
  dependency, and the two groups are derived rather than options:
  `HANDLER_TESTS` is on when `SDL3MCP_BUILD_TESTS` and `SDL3MCP_BUILD_CLI` are,
  and links no SDL; `LIBRARY_TESTS` is on when `SDL3MCP_BUILD_TESTS` and
  `SDL3MCP_BUILD_LIBRARY` are, and links the library or the app half.
  `handler_only_check` configures, builds and tests the Handler half with
  `CMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE`, so the SDL-free build is proven rather
  than assumed; its nested configure is the fixture's own switch, not a public
  option.
  `readme_check` is registered only when Python 3 is found. `targets_check` runs
  only when SDL3 came from `find_package`, because a parent that vendors SDL
  leaves `SDL3_DIR` unset. There is no `tsan` preset: the
  library and the Handler are single-threaded, so there is nothing for it to
  find.

## ABI and linkage

README owns the public surface: §The library API for the six entry points, the
C11 contract and the exception boundary (also `STYLE.md` §Errors), and §Targets
for the static library and the consumer's SDL link line. The internals:

- **No linkage macro.** The library is static only and Windows is a configure
  error, so the public declarations carry neither an import/export attribute nor
  a calling convention: a static archive needs none, and `SDLCALL` is empty on
  Linux, so the ABI is unchanged. The target publishes no SDL linkage macro
  either: the consumer links SDL itself.
