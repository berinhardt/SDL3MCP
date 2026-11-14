# Changelog

All notable changes to SDL3MCP. The format follows Keep a Changelog; versions are
`MAJOR.MINOR.PATCH`.

## 1.0.0

The rewrite. The library is now single-threaded and dial-only, and the Handler
replaces the v0 supervisor as the client-facing server.

### Added

- `--targets=PATH` and `--web=PATH` as app sources. Each alias contributes an
  `app_spawn_<alias>` tool; `--web` starts a page server, a `websocat` bridge and
  an isolated Firefox in a throwaway profile.
- `--dry-run=ALIAS`, which prints the argv a target spawn would run, or the web
  commands/page URL/bridge/profile a web spawn would use, and starts nothing.
- `--dump-tools`, which prints the catalog `tools/list` advertises as JSON and
  exits. It needs no listener.
- `SDL3MCP::SERVER` (the `sdl3mcp` Handler) and `SDL3MCP::COMMON` (the SDL-free
  core) beside `SDL3MCP::SDL3MCP`.
- `sdl3mcp_app(<alias> <target>)`, which writes
  `${CMAKE_BINARY_DIR}/sdl3mcp-targets.json` for the Handler.
- A `--web` run that owns its three processes and its throwaway profile, and a
  browser arm that closes the page on `SDL3MCP_AppShutdown`.

### Changed

- **The library is single-threaded.** `SDL3MCP_AppIterate` is the server loop;
  there is no server thread and no cross-thread snapshot. Every tool handler runs
  on the app thread.
- **`sdl_capture` answers at F+1 like `sdl_input`.** Both register a request and
  return with the next frame the app writes.
- **The library dials only.** `--mcp=off` and `--mcp=HOST:PORT` are the only
  values. A failed dial fails `SDL3MCP_AppInit`; there is no fallback.
- **Captures are raw in the library and PNG at the MCP edge.** `sdl_capture` and
  `sdl_input` return the raw framing plus RGBA bytes; the Handler encodes PNG and
  base64 once, at the hop nearest the application.
- **`sdl_input` names the delivered frame.** The result text is
  `<N> actions applied at frame F; frame F+1 attached, rendered after the actions
  were delivered`.
- **The Handler is one Server shape.** Every Handler is a Server; `--connect`
  makes it a Relay. The advertised list is `wait_session` + the four application
  tools + `app_spawn_<alias>`, independent of whether an app is attached.
- **A spawned application is spawn-and-forget.** The Handler does not signal,
  wait for, or reap it. The web stack is the exception and is Handler-owned.
- **The version is 1.0.0** in the project, `SDL3MCP_VERSION_STRING`,
  `serverInfo.version` and `--mcp-version`.
- C++ sources are CamelCase and follow `STYLE.md`.

### Removed

- **`SDL3MCP_Transport` and `SDL3MCP_Configuration.transport`.** A session exists
  exactly when `connect_host` names an endpoint.
- **`STDIO`, `StdioIsolation`, and library stdio fd ownership.** The library
  never owns the host's stdin/stdout. `--mcp=stdio` and bare `--mcp` are gone.
- **`frame_timeout_ms`, `--mcp-timeout-ms`, the library tools' `timeout_ms`, and
  `sdl_info.limits.frameTimeoutMs`.** The only library timeout is
  `connect_timeout_ms`; the Handler's `--timeout-ms` and tool `timeout_ms` are
  the forward budget.
- **`--target` and `--web-root`.** `--targets` and `--web` are the app sources.
- **`--print-plan`.** `--dry-run=ALIAS` replaces it.
- **`--startup-timeout-ms` and `--terminate-timeout-ms`.** The Handler's single
  `--timeout-ms` covers forward, attach and spawn; `sdl_terminate` is forwarded
  with no watchdog.
- **The supervisor tree and the v0 sources**, including the server thread, the
  stdio isolation, the two-writer capture pipeline and their white-box suites.
  The v0 tree survives in `v0.1.0` and history.
- **The `tsan` preset.** There is no thread left to race.

## 0.1.0

The v0 release, preserved in `CONTRACT.md` and the `v0.1.0` tag.
