# SDL3MCP Style Guide

The rules the 1.0 tree follows. `README.md` fixes the public behavior; this file
fixes how the code is written. Where they disagree, `README.md` wins; where this
file has no rule, match the surrounding code. `CONTRACT.md` is the v0 archive and
is not a current source of truth.

## Files

- CamelCase names with `.h` and `.cpp` extensions: `SocketStream.h`,
  `SocketStream.cpp`. The public headers keep their names, `include/SDL3MCP.h`
  (the entry points) and `include/SDL3MCP_types.h` (the SDL-free types).
- One primary type per file where practical: a header declares the type its
  matching `.cpp` defines, and the two share a basename.
- Platform arms of one type share a header and take the platform suffix:
  `SocketStream.cpp` is the native implementation and `WebSocketStream.cpp` is
  the Emscripten one.
- `include/` is the flat public surface and exposes the C API only; it never
  includes a private `src/` header. `SDL3MCP_types.h` includes neither SDL nor
  the entry points, so a unit can take the types without an SDL include path.

## C++ names

- Types and classes: PascalCase (`ToolCatalog`, `ToolResult`).
- Functions, methods, and fields: camelCase (`writeFrame`, `frameCounter`).
- Private data members: trailing underscore (`frameCounter_`).
- Namespaces: `sdl3mcp`.
- Helpers no other translation unit needs: anonymous namespace in the `.cpp`,
  never a declaration in a header.
- Wire names (JSON fields, CLI flags) are snake_case (`max_frame_bytes`); C++
  names never are.

## Formatting

- `.clang-format` is the authority: Google base, 3-space indent, no column limit,
  left-aligned pointers and references.
- Run it on the files a change touches; do not reformat untouched code in the
  same change.
- Keep include groups meaningful rather than hand-sorting them; the formatter
  orders them within a group.

## Headers

- Every header is self-contained: it compiles alone and includes what it uses.
- A `.cpp` includes its own header first, before any other include.
- No `using namespace` in a header; qualify names or alias inside the `.cpp`.
- Include the minimum: forward-declare in a header when a definition is not
  needed, and include the definition in the `.cpp`.

## Errors

- Errors are values, not exceptions. Internal code returns a typed
  `Result<T, E>` (`ProtocolError`, `TransportError`); no exception crosses the C
  ABI, and the C entry points report failure through `SDL_APP_FAILURE` and
  `SDL_GetError`.
- Never swallow an error: propagate it, or report it with the reason.
- Do not validate caller input with `assert`; return the error instead.

## Comments

- Comments explain why, not what. Delete a comment that restates the code.
- No dead code and no commented-out code; version control remembers.
- A public declaration documents its contract (ownership, lifetime, threading),
  not its implementation.

## Tests

- doctest, one executable per family, behavior-first: assert through the public
  seam the change names, not private helpers.
- The library and the Handler are single-threaded, so a suite needs no locking;
  a test that uses a thread (the test MCP server, the page stack) owns it
  explicitly.
- Deterministic: no network, no wall-clock dependence, and no sleep used as
  synchronization.
- A bug fix carries the regression test that would have caught it.
- **Payload floor.** A transport test carries at least one payload larger than
  the OS buffer it travels through (64 KiB on Linux pipes), so the answer has to
  be written in parts and read back whole; a suite whose largest message fits in
  one write has not exercised the transport. The exemplar is `tests/handler_tests.cpp`,
  `a response larger than the pipe buffer reaches the client`: its `noise = true`
  parameter on `serveRawFrame` produces incompressible pixels, so the PNG stays
  above the buffer instead of compressing under it.
- **Drained child output.** A harness that spawns a process gives the child a
  file or a draining thread for its stderr, so the child can always finish a
  write; a captured pipe left undrained while the harness blocks on stdout is a
  deadlock, not a failure report. The worked example is `tests/handler_tests.cpp`'s
  `TestProcess`: it gives the child a stderr pipe, and `readStderrLine` drains it,
  polling the descriptor and buffering partial lines across reads.

## CMake

- The public targets are `SDL3MCP::SDL3MCP` (the library), `SDL3MCP::SERVER` (the
  Handler), and `SDL3MCP::COMMON` (the SDL-free core both use). The object
  libraries `sdl3mcp_common_objects`, `sdl3mcp_app_objects`, and
  `sdl3mcp_server_objects` stay internal.
- The shared build exports only the C surface; tests link the object libraries
  rather than the shared object.
- No new third-party dependency without a named blocker. Runtime dependencies
  stay SDL3-only.
