# SDL3MCP Style Guide

`sdl3mcp`'s single standards document. `README.md` fixes the public behavior;
this file fixes how the code is written. Where they disagree, `README.md` wins;
where this file has no rule, match the surrounding code. `CONTRACT.md` is the v0
archive and is not a current source of truth.

Every rule here is one of two kinds, and the section it sits in says which:

- **Mechanical rules** are enforced by `scripts/check.sh`. A violation fails the
  gate, so there is nothing to argue about in review.
- **Judgement rules** are owned by a reviewer. A check cannot decide whether a
  name, a split boundary, a comment or a helper's home is right, so these are
  prose and are applied with the surrounding code in view.

Layering is neither kind: it is a design constraint on what may be compiled
together, and `docs/architecture.md` owns it.

## Mechanical rules

### File size

- **300 non-comment lines fails; 150 warns.** A file over 300 non-comment lines
  fails `scripts/check.sh`, and the message names the file and the limit; a file
  over 150 non-comment lines warns without failing. 150 is the target, not the
  ceiling: a unit that goes past 150 is due a split on its next change, and one
  at 300 is not reviewable.
- **Scope: the product sources.** `include/` and `src/`, which carries the
  `src/common/`, `src/lib/` and `src/handler/` halves. `tests/` and
  `third_party/` are not in scope.
- **Tests are exempt on purpose.** A suite's length comes from the scenarios it
  covers, not from a responsibility its author chose. Cutting a test file to fit
  the rule would cut coverage, so no test file is split for size.
- **A soft-limit exception is named, not implied.** A file kept above 150 is a
  decision recorded in the ticket's resolution — one long protocol arm, a
  generated table — so the next reader knows the size was chosen rather than
  drifted into. Over 300 is not a decision: the file splits.

This is the only mechanical rule. A rule worth a check earns one; a preference a
check cannot decide stays in the judgement section instead of being weakened
into something a script can see.

## Judgement rules

A reviewer owns these: the rule constrains the choice, the author and the
reviewer make it. Cite the rule in review instead of arguing from taste.

### Files

- CamelCase names with `.h` and `.cpp` extensions: `SocketStream.h`,
  `SocketStream.cpp`. The public headers keep their names, `include/SDL3MCP.h`
  (the entry points) and `include/SDL3MCP_types.h` (the SDL-free types).
- **Compound names use a hyphen between the words**: `Handler-Loop.cpp`,
  `Web-Plan.cpp`, `Handler-Loop.h`. clang-format treats the file whose name
  matches the unit's basename as that unit's main header and keeps it first in
  the include block, but it recognises the pair only when the names match; a
  `HandlerLoop.cpp` would leave its own header unsorted among the other
  includes, so the unit's header would no longer lead. A unit that shares
  declarations with other units gets the matching hyphenated header; a unit with
  no header of its own still includes its class header first (`Handler.h` in
  every `Handler-*.cpp`).
- **Split by responsibility, not by size.** A unit over 150 lines is a candidate
  for a split, and the split gives each unit one responsibility: one class's
  method definitions divide by what they do (loop, catalog, forwards, spawn,
  web), a parser separates from its model, usage text separates from argument
  parsing. A unit that is long because one responsibility is long — a table, a
  single protocol arm — is not a split candidate; say so in the resolution and
  leave it whole while it stays under the hard limit.
- One primary type per file where practical: a header declares the type its
  matching `.cpp` defines, and the two share a basename.
- Platform arms of one type share a header and take the platform suffix:
  `SocketStream.cpp` is the native implementation,
  `SocketStream-Websockets.cpp` the Emscripten one, and
  `SocketStream-WSEvents.cpp` its events half. Both `.cpp` units strip to
  `SocketStream.h` through `IncludeIsMainRegex`, so it leads their includes; the
  Emscripten arm's own header, `SocketStream-Websockets.h`, carries that arm's
  shared state under the shared `SocketStream` type —
  `struct SocketStream::Native` and the close codes.
- `include/` is the flat public surface and exposes the C API only; it never
  includes a private `src/` header. `SDL3MCP_types.h` includes neither SDL nor
  the entry points, so a unit can take the types without an SDL include path.

### C++ names

- Types and classes: PascalCase (`ToolCatalog`, `ToolResult`).
- Functions, methods, and fields: camelCase (`writeFrame`, `frameCounter`).
- Private data members: trailing underscore (`frameCounter_`).
- Namespaces: `sdl3mcp`.
- Wire names (JSON fields, CLI flags) are snake_case (`max_frame_bytes`); C++
  names never are.

### Helper placement

A helper is a free function, a small struct or a constant table that exists for a
caller's convenience; it is not a collaborator and does not get a class.

- **One caller: the helper stays in the caller's unit.** Define it in that
  `.cpp`, in an anonymous namespace, and declare it in no header. Moving it out
  pre-emptively costs a header, an include and a name in the link, and the next
  reader has to leave the file to understand the caller. **The size cap outranks
  this rule.** When inlining a single-caller helper would push the unit past the
  300-line hard limit, the helper may be lifted into a matching hyphenated unit
  instead — `HandlerCli-Endpoint.h` beside `HandlerCli-Endpoint.cpp`, called
  from `HandlerCli.cpp` — because a limit the check enforces is not a preference
  a reviewer may weigh. That unit's header says in its comment that the cap chose
  the split, so the next reader does not wait for a second caller that is not
  coming.
- **More than one caller: it clusters by function.** A private unit named for
  what the helpers do holds them — `ToolArguments.h` for the argument readers
  (`readInteger`, `readTimeoutMs`), inline in the header because a header-only
  unit is enough while every helper stays inline. Never a generic support file:
  `Utils.h`, `Helpers.h` and `Common.h` say nothing about what belongs in them,
  so everything does, and the file grows into the largest unit in the tree with
  no owner to split it. A name for the function makes the boundary
  self-evident — a helper that does not fit the name does not go in the file.

### Formatting

- `.clang-format` is the authority: Google base, 3-space indent, no column limit,
  left-aligned pointers and references.
- Run it on the files a change touches; do not reformat untouched code in the
  same change.
- Let the formatter own include order: it groups and sorts them, and
  `IncludeIsMainRegex` puts a unit's class header first. A definition unit with
  no header of its own takes a name whose hyphenated suffix strips to its class
  header — `Handler-Loop.cpp` opens with `Handler.h` and `Web-Run-Child.cpp`
  with `Web-Run.h` — so the formatter detects that header. A unit whose name
  cannot map that way gets a matching header.

### Headers

- Every header is self-contained: it compiles alone and includes what it uses.
- A `.cpp` includes its own header first, before any other include.
- No `using namespace` in a header; qualify names or alias inside the `.cpp`.
- Include the minimum: forward-declare in a header when a definition is not
  needed, and include the definition in the `.cpp`.

### Errors

- Errors are values, not exceptions. Internal code returns a typed
  `Result<T, E>` (`ProtocolError`, `TransportError`); no exception crosses the C
  ABI, and the C entry points report failure through `SDL_APP_FAILURE` and
  `SDL_GetError`.
- Never swallow an error: propagate it, or report it with the reason.
- Do not validate caller input with `assert`; return the error instead.

### Comments

- Comments explain why, not what. Delete a comment that restates the code.
- A comment that stays earns its place by naming a reason a reader cannot see in
  the code: a constraint, a workaround, a protocol rule, an ordering that would
  otherwise look arbitrary.
- No dead code and no commented-out code; version control remembers.
- A public declaration documents its contract (ownership, lifetime, threading),
  not its implementation.

### Tests

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

### CMake

- The public targets are `SDL3MCP::SDL3MCP` (the static library),
  `SDL3MCP::SERVER` (the Handler), and `SDL3MCP::COMMON` (the SDL-free core
  both compile, as a real static library; `docs/architecture.md` §Layering owns
  what belongs in it). `SDL3MCP::SDL3MCP` and the installed package exist when
  the library half is built; a Handler-only build (`SDL3MCP_BUILD_LIBRARY=OFF`)
  keeps `SDL3MCP::SERVER` and `SDL3MCP::COMMON`. The object libraries
  `sdl3mcp_common_objects`, `sdl3mcp_lib_objects`, and
  `sdl3mcp_handler_objects` stay internal.
- The library is static only and publishes no SDL: no SDL target, include path,
  linkage macro or Emscripten flag travels with it, so the app names SDL itself.
  Tests link the common archive and the lib object library for private symbols,
  and the public target for the C surface.
- No new third-party dependency without a named blocker. Runtime dependencies
  stay SDL3-only.
