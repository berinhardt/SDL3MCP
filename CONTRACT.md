# SDL3MCP v0 Contract

**Status:** extracted 2026-11-12 from the v0 implementation and its documentation.
**Purpose:** the frozen oracle for the rewrite. Every architecture decision and
the final spec must preserve what this document marks **MUST**; everything marked
**FREE** is implementation detail the rewrite may replace.
**Sources:** `deps/SDL3MCP/README.md` (the v0 design contract),
`deps/SDL3MCP/include/SDL3MCP/SDL3MCP.h` (the public surface),
`deps/SDL3MCP/src/` (exact constants, strings, and framing where the README is
silent or stale), `deps/SDL3MCP/tests/` (behavioural oracle), and
`deps/SDL3MCP/AGENTS.md` (the decisions-not-to-regress summary). Inline citations
name the section or file; line numbers are from the v0 tree as read on the date
above.

**Normative words:** **MUST** / **MUST NOT** are observable contract. **SHOULD**
is a strong default that a deliberate decision may change. **FREE** is not
contract.

---

## Contents

1. [Vocabulary and naming](#1-vocabulary-and-naming)
2. [Public C API](#2-public-c-api)
3. [MCP protocol](#3-mcp-protocol)
4. [Tool catalog](#4-tool-catalog)
5. [Raw frame framing and presentation](#5-raw-frame-framing-and-presentation)
6. [Transports and configuration](#6-transports-and-configuration)
7. [Frame pipeline](#7-frame-pipeline)
8. [Input model](#8-input-model)
9. [Handler executable](#9-handler-executable)
10. [Web app source and Handler-owned web run](#10-web-app-source-and-handler-owned-web-run)
11. [Build, platform, and dependencies](#11-build-platform-and-dependencies)
12. [Settled rewrite deltas](#12-settled-rewrite-deltas)
13. [Open contract questions](#13-open-contract-questions)
14. [Source map](#14-source-map)

---

## 1. Vocabulary and naming

| Term | Meaning |
| --- | --- |
| **Library** | The linkable SDL3MCP C library: public header plus C++20 implementation. |
| **App** | The SDL3 application that links the library and owns its window, renderer, and event loop. |
| **Client** | The MCP client (an agent or another Handler) that speaks JSON-RPC to a session. |
| **Handler** | The `sdl3mcp` executable. v0 calls it the *supervisor*. It is not the library and not the app. |
| **Server mode** | A Handler without `--connect`: upstream is stdio, downstream is `--listen` plus app sources. v0 README calls the `--listen` role *relay mode*. |
| **Relay mode** | A Handler with `--connect`: upstream is TCP, downstream is `--listen` plus app sources. v0 README calls the spawner variant *spawner mode*. |
| **Transport** | How the library reaches its client: `NONE`, `STDIO`, `CONNECT`, `WEBSOCKET`. |
| **Session** | One MCP client session: `initialize`, `tools/list`, `tools/call`, `ping`. |
| **Tool** | One MCP tool definition plus its handler. |
| **Frame** | A rendered image: tightly packed RGBA8888, top-left origin, `width * height * 4` bytes. |
| **Capture** | A request for a fresh frame, answered by the app through `SDL3MCP_FrameRequested` / `SDL3MCP_WriteFrame`. |
| **Batch** | An ordered `sdl_input` action list, applied atomically in one `SDL3MCP_AppIterate`. |
| **F+1** | The frame after the one in which a batch was applied; the first frame in which the app has certainly consumed the events. |
| **Raw framing** | One JSON result line followed by exactly `byteLength` RGBA bytes, with no newline and no base64. |
| **Presentation** | Turning a raw frame into a client-facing `image/png` content block; Handler policy, never library policy. |
| **Targets file** | A JSON map from alias to an app argument vector, used by a relay to advertise `app_spawn_<alias>`. |
| **Web run** | The Handler-owned page server, `websocat` bridge, and Firefox launched by a `--web` entry. It is infrastructure, not the app. |

### 1.1 v0 names to rewrite names

The rewrite renames the executable's roles. The mapping is normative for
reading this contract, not a claim that the v0 prose is wrong.

| v0 README | Rewrite |
| --- | --- |
| supervisor / `sdl3mcp` | Handler |
| `--target <executable>` child supervisor | removed |
| `--listen` relay mode | Server (no `--connect`) |
| `--connect --targets` spawner mode | Relay (`--connect`) + app source |
| `--web-root` web spawner | `--web` app source + Handler-owned web run |

The binary name is settled: the rewrite keeps `sdl3mcp` and bumps the project
version to `1.0.0` (ticket 02).

### 1.2 Contract versus implementation

MUST preserve:

- the public C header's declarations, types, defaults, linkage, and lifecycle;
- every MCP name, schema, result shape, error code, and documented error string;
- the raw frame framing and the Handler's `image/png` presentation;
- the CLI grammar and the observable mode behaviour, except the settled deltas
  in §12;
- the fd-ownership and threading rules that a host can observe;
- the build options, target names, and platform stance in §11.

FREE to replace:

- file layout, class names, internal headers, object-library composition;
- the `Responder` / `Session` / `Peer` / `Transport` type names and their
  inheritance or composition;
- the exact JSON parser, queue implementation, keymap table, and replay plan;
- test structure, except the behaviour-level oracle in §11.7;
- the README's prose, once the contract and the new implementation agree.

---

## 2. Public C API

### 2.1 Header and linkage

`include/SDL3MCP/SDL3MCP.h` is the complete public surface. It MUST:

- compile as C11 and as C++20;
- include only `<SDL3/SDL.h>`, `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`, and
  `<SDL3MCP/SDL3MCP_version.h>`;
- expose C linkage through `extern "C"`;
- tag public symbols with `SDL_DECLSPEC` and `SDLCALL`, following SDL's own
  linkage; the shared build defines `DLL_EXPORT`, the static build defines
  `SDL_STATIC_LIB` privately; no private `SDL3MCP_DECLSPEC`,
  `SDL3MCP_BUILDING_LIBRARY`, or `SDL3MCP_STATIC` macro may appear;
- never throw across the boundary; C++ exceptions are caught, reported through
  `SDL_GetError()`, and converted to `SDL_APP_FAILURE` where the signature
  allows it.

`SDL3MCP_DefaultConfiguration()` is a `static inline` function in the header, not
an exported symbol.

### 2.2 Types and defaults

```c
typedef enum SDL3MCP_Transport
{
    SDL3MCP_TRANSPORT_NONE = 0,
    SDL3MCP_TRANSPORT_STDIO,
    SDL3MCP_TRANSPORT_CONNECT,
    SDL3MCP_TRANSPORT_WEBSOCKET
} SDL3MCP_Transport;

typedef struct SDL3MCP_Configuration
{
    SDL3MCP_Transport transport;
    uint32_t frame_timeout_ms;
    uint32_t max_frame_bytes;
    bool warp_cursor;
    int log_level;
    char connect_host[64];
    uint16_t connect_port;
} SDL3MCP_Configuration;
```

The four enum values MUST keep their numeric order: `NONE=0`, `STDIO=1`,
`CONNECT=2`, `WEBSOCKET=3`. `WEBSOCKET` was appended after `CONNECT` so the two
existing values keep their numbers (`AGENTS.md`, "The library never listens").

| Field | Default | Contract |
| --- | --- | --- |
| `transport` | `NONE` | `NONE` starts no server; `STDIO` starts the stdio server; `CONNECT` dials `connect_host:connect_port`; `WEBSOCKET` is the browser's `CONNECT`. |
| `frame_timeout_ms` | `2000` | Default `sdl_capture` timeout and the time `sdl_input` waits for its F+1 acknowledgement. An `sdl_input` call without `timeout_ms` gets twice this as its whole budget. |
| `max_frame_bytes` | `32 * 1024 * 1024` | Upper bound on a raw RGBA frame accepted by `SDL3MCP_WriteFrame`. 4K fits; 5K needs a larger value. |
| `warp_cursor` | `true` | Warp the OS cursor with `SDL_WarpMouseInWindow` on absolute moves and positioned `mouse_button` actions. |
| `log_level` | `-1` | Accepted and validated as `-1..4` (`0` off, `1` error, `2` warn, `3` info, `4` debug). v0 has no leveled logger; only usage and errors go to stderr. See open question 13.10. |
| `connect_host` | empty | `CONNECT`/`WEBSOCKET` only; at most 63 bytes before the terminator; never carries a scheme. |
| `connect_port` | `0` | `CONNECT`/`WEBSOCKET` only; 1–65535. |

The struct contains no pointers, is copied by `SDL3MCP_AppInit`, and is never
retained. `SDL3MCP_AppInit(NULL, argc, argv)` means "defaults plus whatever the
command line says". The command line never changes a field it does not mention.
Transport values are case-insensitive.

### 2.3 Entry point contracts

All six entry points are app-thread only. The C API has one lifecycle per
process: a second `SDL3MCP_AppInit` fails even after shutdown;
`SDL3MCP_AppShutdown` is idempotent.

#### `SDL3MCP_AppInit`

```c
SDL_AppResult SDL3MCP_AppInit(SDL3MCP_Configuration *config, int argc, char **argv);
```

MUST:

- be called once, from the app thread, before any other SDL3MCP function;
- read `argc`/`argv` without modifying them;
- start the server thread for `STDIO`, dial the configured endpoint and start it
  for `CONNECT`, and only record the configuration for `NONE`;
- return `SDL_APP_CONTINUE` on success, `SDL_APP_SUCCESS` for
  `--mcp-help`/`--mcp-version`, and `SDL_APP_FAILURE` for invalid arguments,
  broken stdin/stdout, an unreachable `CONNECT` endpoint, or a thread that
  cannot start;
- leave no state behind on failure; a later `SDL3MCP_AppShutdown` is still safe;
- fail a second call in the same process, even after shutdown.

#### `SDL3MCP_SetWindow`

```c
SDL_AppResult SDL3MCP_SetWindow(SDL_Window *window);
```

MUST:

- bind the window agents interact with; call after creating it, again on
  replacement, and with `NULL` to clear;
- use the window for geometry (`SDL_GetWindowSize` in points,
  `SDL_GetWindowSizeInPixels` in pixels, published once per `SDL3MCP_AppIterate`),
  event routing (synthetic events carry its `windowID`), text input, cursor
  warping, and relative-mouse-mode queries;
- return `SDL_APP_FAILURE` if `SDL3MCP_AppInit` was not called or the window has
  no drawable area yet; a window minimized later is fine and only limits what
  the app can read back;
- reset held keys, held buttons, and the virtual cursor on rebind.

#### `SDL3MCP_AppIterate`

```c
SDL_AppResult SDL3MCP_AppIterate(void);
```

MUST:

- be called once per frame, **last**: after drawing, after
  `SDL_RenderPresent`, and after answering `SDL3MCP_FrameRequested`;
- acknowledge the batch applied at frame F (which is why the matching
  `sdl_input` returns at F+1) before taking a new batch, so at most one batch is
  in flight;
- take at most one queued input batch and push its events onto SDL's queue;
  this is the only place input enters the app;
- publish current window geometry, held input, and frame counters for the server
  thread;
- never block and never wait for the network or a client;
- return `SDL_APP_CONTINUE` normally, and `SDL_APP_FAILURE` when the server
  thread died from a fatal transport error;
- be a no-op returning `SDL_APP_CONTINUE` when `transport == NONE`.

#### `SDL3MCP_AppShutdown`

```c
void SDL3MCP_AppShutdown(void);
```

MUST:

- stop the server thread, close the transport, restore the host's stdin/stdout,
  release any keys or buttons the library still holds, and free the frame buffer;
- be idempotent and safe after a failed `SDL3MCP_AppInit`;
- be bounded: the stdin reader wakes at least every 50 ms to check the stop flag,
  so shutdown does not hang on an idle client;
- be app-thread only;
- leave no restart path: one lifecycle per process.

#### `SDL3MCP_FrameRequested`

```c
bool SDL3MCP_FrameRequested(void);
```

MUST:

- return `true` while a capture is waiting for a frame;
- be cheap enough to call every frame (an atomic load, no locks);
- be called after drawing and before presenting; when true, the app reads back
  pixels and calls `SDL3MCP_WriteFrame` once;
- stay `true` until the frame is accepted or the capture times out, so a missed
  frame is retried on the next iteration;
- be always `false` when no server is running or no window is bound.

#### `SDL3MCP_WriteFrame`

```c
void SDL3MCP_WriteFrame(const uint8_t *data, size_t size, int width, int height);
```

MUST:

- accept only tightly packed RGBA8888, top-left origin, row-major, stride exactly
  `width * 4`, bytes in R, G, B, A order;
- accept only `size == width * height * 4`, with positive dimensions and
  `size <= max_frame_bytes`;
- stamp the frame with `frameCounter + 1`, the index of the frame whose pixels it
  holds; a crop preserves that stamp;
- copy the pixels before returning, so the app can reuse or free its buffer;
- be called only when `SDL3MCP_FrameRequested()` is true; calls with no pending
  capture are ignored;
- be app-thread only, because it reads the pending capture state;
- return nothing: a drop leaves the request pending and surfaces in the timeout
  text and `sdl_info`, not in the render loop.

There is no pitch or stride parameter. `width` and `height` are the frame's own
dimensions, so any render target is valid; the library never measures the window
to validate a frame.

### 2.4 Threading and fd ownership

| Thread | May touch | Must not touch |
| --- | --- | --- |
| App thread | SDL, the window, the renderer, all `SDL3MCP_*` functions. | — |
| Server thread (desktop) | the transport's fds, JSON, the copied frame buffer, the input queue. | any SDL function, including `SDL_GetWindowSize`. |

Rules:

1. One owner per piece of state. Window geometry is published by the app thread
   into a snapshot; the server thread only reads it.
2. Input travels one way. Tool calls append immutable batches to a queue;
   `SDL3MCP_AppIterate` drains and applies them on the app thread.
3. Frames travel one way. `SDL3MCP_WriteFrame` copies pixels under a mutex and
   signals a condition variable; the crop happens after the copy; the Handler's
   encoding happens in a different process.
4. No blocking on the app thread. `SDL3MCP_AppIterate` and
   `SDL3MCP_WriteFrame` never wait for the server, the client, or a lock held
   during encoding.
5. Shutdown joins. `SDL3MCP_AppShutdown` sets the stop flag, waits for the reader
   (bounded by its 50 ms poll interval), joins the thread, then releases held
   input, restores stdin/stdout, and frees frames.

The library assumes one app thread. Calling `SDL3MCP_SetWindow`,
`SDL3MCP_AppIterate`, `SDL3MCP_WriteFrame`, or `SDL3MCP_AppShutdown` from a
different thread is unsupported.

fd ownership:

| Transport | stdin/stdout | MCP stream |
| --- | --- | --- |
| `NONE` | untouched | none |
| `STDIO` | duplicates the originals for MCP; host stdin → `/dev/null`; host stdout → stderr; stderr untouched; restored at shutdown | duplicated stdin/stdout |
| `CONNECT` | untouched | one dialed socket |
| `WEBSOCKET` | untouched (a page has none) | one dialed WebSocket |

The host MUST NOT write to stdout before `SDL3MCP_AppInit` in stdio mode: at that
point stdout is still the client's pipe. The host MUST NOT read stdin after a
stdio `AppInit`: it is `/dev/null`. Diagnostics belong on stderr.

### 2.5 Error handling

- The C API never throws. C++ exceptions stop at the boundary, are logged, and
  become `SDL_APP_FAILURE` where the signature allows it.
- On failure, `SDL_GetError()` explains what happened.
- `SDL3MCP_AppIterate` returns `SDL_APP_FAILURE` when the server thread died from
  a fatal transport error; the app should propagate that failure.
- A client disconnect is not an app failure: the server sees EOF, stops cleanly,
  and `SDL3MCP_AppIterate` keeps returning `SDL_APP_CONTINUE`.

---

## 3. MCP protocol

### 3.1 Framing

- UTF-8 JSON-RPC 2.0, one message per line, no batches.
- The server targets MCP protocol revision `2025-11-25` and always answers
  `initialize` with that revision, whatever the client asked for
  (`src/session.cpp:21`).
- Advertised capability: `tools` only. No resources, prompts, sampling, or
  logging.
- `serverInfo` is `{"name": "sdl3mcp", "version": "0.1.0"}` in v0; the version is
  `SDL3MCP_VERSION_STRING` from the generated version header
  (`src/session.cpp:23,110-113`; `CMakeLists.txt:3`).
- Inbound lines are capped at 1 MiB; a longer line is a transport error and ends
  the session.
- Every outbound message is flushed immediately.
- The raw-frame exception is the only non-line payload: a result line followed
  by exactly `byteLength` bytes, no newline, no base64 (§5).

### 3.2 JSON-RPC rules

- `initialize` MUST come first and only once. `tools/list` and `tools/call`
  before it fail with `-32600` and the message
  `tools are unavailable before initialize`. A second `initialize` fails with
  `-32600` and `initialize was already called`.
- `initialize` requires a string `protocolVersion`; otherwise `-32602` and
  `initialize requires a string protocolVersion`.
- `ping` is answered at any time.
- Notifications get no response and change no state.
- Requests are handled one at a time on the single server thread. A blocking
  tool call delays every later request, including `ping`, by up to its timeout;
  nothing is cancelled by the delay.
- A JSON array is rejected as a batch with `-32600` and
  `Batch requests are not supported`.
- Invalid JSON is `-32700` with `Parse error: invalid JSON`; blank input is
  `-32700` with `Parse error: empty input`.
- A non-object message is `-32600` with `Request must be a JSON object`.
- A request id MUST be a string or a number; any other id is `-32600` with
  `Invalid request id` and a `null` id.
- `jsonrpc` MUST be `"2.0"`; otherwise `-32600` with
  `Missing or unsupported jsonrpc version`.
- `method` MUST be a string; otherwise `-32600` with `Method must be a string`.
- `params`, when present, MUST be an object; otherwise `-32600` with
  `Params must be a JSON object`.
- An unknown method is `-32601` with `Method not found: <method>`.
- `tools/call` requires a string `name` (`-32602`,
  `tools/call requires a string tool name`), a known tool (`-32602`,
  `Unknown tool: <name>`), and an object `arguments` when present (`-32602`,
  `tools/call arguments must be a JSON object`).
- Tool failures use `isError: true` with a text explanation. JSON-RPC errors are
  reserved for protocol-level problems.

Response shapes:

```json
{"jsonrpc":"2.0","id":1,"result":{...}}
{"jsonrpc":"2.0","id":1,"error":{"code":-32600,"message":"..."}}
```

`id` is echoed exactly; protocol errors that cannot echo a valid id use `null`.

### 3.3 Example session

The v0 README's example session is stale for `sdl_input`: it shows a text-only
content result, but the library's `sdl_input` result is raw framing (§5). The
correct library exchange is:

```text
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"agent","version":"1.0"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"sdl3mcp","version":"0.1.0"}}}
→ {"jsonrpc":"2.0","method":"notifications/initialized"}
→ {"jsonrpc":"2.0","id":2,"method":"tools/list"}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[...]}}
→ {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"sdl_input","arguments":{"actions":[{"type":"key","key":"right","action":"down"}]}}}
← {"jsonrpc":"2.0","id":3,"result":{"format":"rgba8888","width":960,"height":540,"byteLength":2073600,"frame":14,"text":"1 actions applied at frame 12; frame 14 attached, rendered after the actions were delivered"}}
<2073600 raw RGBA8888 bytes; no newline, no base64>
```

Through the Handler, the same call ends as a text block plus one `image/png`
block (§5.3).

---

## 4. Tool catalog

The library advertises exactly four tools: `sdl_info`, `sdl_input`,
`sdl_capture`, `sdl_terminate`. The Handler may add `wait_session`,
`app_spawn_<alias>`, or (v0 child mode) `spawn`, and always replaces
`sdl_capture` with its own presentation definition.

Annotations:

| Tool | `readOnlyHint` | `idempotentHint` | `openWorldHint` | `destructiveHint` |
| --- | --- | --- | --- | --- |
| `sdl_info` | `true` | `true` | `false` | — |
| `sdl_input` | `false` | `false` | `false` | — |
| `sdl_capture` | `true` | `false` | `false` | — |
| `sdl_terminate` | `false` | `false` | `false` | `true` |

Every tool description MUST carry a parseable `Example:` JSON object naming each
argument its schema accepts; `tests/check_readme.py` enforces this for the
catalog and the README.

### 4.1 `sdl_info`

No arguments. Returns a pretty-printed JSON text block with these fields:

```json
{
  "sdlVersion": "3.2.10",
  "videoDriver": "wayland",
  "window": {
    "title": "SDL3MCP demo",
    "width": 960,
    "height": 540,
    "pixelWidth": 1920,
    "pixelHeight": 1080,
    "pixelDensity": 2.0,
    "relativeMouseMode": false
  },
  "input": { "heldKeys": ["right"], "heldButtons": ["left"] },
  "capture": {
    "pending": false,
    "count": 3,
    "dropped": 0,
    "lastWidth": 1920,
    "lastHeight": 1080,
    "lastBytes": 8294400,
    "lastMs": 27,
    "lastError": null
  },
  "limits": { "frameTimeoutMs": 2000, "maxFrameBytes": 33554432 },
  "frame": 128,
  "appliedFrame": 127
}
```

MUST:

- `sdlVersion` is `major.minor.micro` of the linked SDL.
- `videoDriver` is the current SDL video driver, or `null` until a window is
  bound.
- `window` is `null` when no window is bound; otherwise sizes are in points
  (`width`, `height`) and drawable pixels (`pixelWidth`, `pixelHeight`).
- `heldKeys` are canonical lowercase scancode names in press order; a key held
  with `down` is listed until released. `heldButtons` use the same names as the
  `sdl_input` button enum.
- `capture.pending` is true while a frame request waits; `count` is successful
  frames handed to a client (a `sdl_capture`, or the frame `sdl_input`
  returned); `dropped` counts rejected `SDL3MCP_WriteFrame` attempts.
- `lastWidth`/`lastHeight` describe the frame the library returned, after any
  region crop, and are the dimensions of the PNG the Handler encodes.
  `lastBytes` is the raw RGBA payload size (`width * height * 4`). `lastMs`
  covers the wait for the frame plus the crop. `lastError` is `null` when the
  last request succeeded (or none ran), otherwise the last drop or capture error
  string.
- `limits` repeats the effective `frame_timeout_ms` and `max_frame_bytes`.
- `frame` is the library's frame counter; `appliedFrame` is the last frame at
  which an input batch was applied (0 when none was).
- With no window bound, `window` is `null` and the tool still succeeds.

### 4.2 `sdl_input`

Schema (library and Handler; the Handler forwards it unchanged except for
presentation):

```json
{
  "name": "sdl_input",
  "description": "Queue a batch of synthetic input actions; the call always returns the frame that follows the batch: text naming both the frame the actions were applied at and the frame attached, plus that frame's pixels, which the supervisor presents as an image/png part and a direct library client receives as raw RGBA. The batch is applied atomically at the next frame and the call returns after the following frame, once the application has consumed the generated events. The attached frame is always rendered after the actions were delivered - one acknowledgement frame and at least one capture frame later - so an unchanged frame means the actions had no effect, such as a click that missed a widget, and sdl_info carries the geometry for the next attempt. Arguments: actions (required; 1-256 action objects), optional region, and optional timeout_ms. Each action object is one of six types: key {key name or chord, action=down|up|press (default press), modifiers=[ctrl|shift|alt|gui] (at most four)}; text {text, 1-1024 UTF-8 bytes}; mouse_move {x,y in window points or dx,dy relative, never both}; mouse_button {button=left|middle|right|x1|x2, action=down|up|click (default click), clicks=1-3 (default 1), optional x,y window points to move first}; scroll {y in notches (1.0 is one notch, positive scrolls down), x default 0}; release_all {no extra fields}. The optional region crops the returned frame in pixels of the full frame with a top-left origin, exactly like sdl_capture; sdl_input coordinates are window points, so use pixelDensity from sdl_info to convert. timeout_ms bounds the whole call, apply and capture together, and defaults to twice the frame timeout. Example: {\"actions\":[{\"type\":\"key\",\"key\":\"ctrl+s\",\"action\":\"press\"},{\"type\":\"mouse_button\",\"button\":\"left\",\"x\":120,\"y\":80}]}",
  "inputSchema": {
    "type": "object",
    "properties": {
      "actions": {
        "type": "array",
        "minItems": 1,
        "maxItems": 256,
        "items": {
          "oneOf": [
            {
              "type": "object",
              "description": "Press, hold, or release one key, optionally with modifiers.",
              "properties": {
                "type": { "const": "key" },
                "key": {
                  "type": "string",
                  "description": "Key name, or a modifier+modifier+key chord for press, for example \"ctrl+s\"."
                },
                "action": { "enum": ["down", "up", "press"], "default": "press" },
                "modifiers": {
                  "type": "array",
                  "maxItems": 4,
                  "items": { "enum": ["ctrl", "shift", "alt", "gui"] }
                }
              },
              "required": ["type", "key"],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Type text as SDL text input.",
              "properties": {
                "type": { "const": "text" },
                "text": { "type": "string", "minLength": 1, "maxLength": 1024 }
              },
              "required": ["type", "text"],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Move the cursor to absolute window points (x, y) or by relative deltas (dx, dy).",
              "properties": {
                "type": { "const": "mouse_move" },
                "x": { "type": "number" },
                "y": { "type": "number" },
                "dx": { "type": "number" },
                "dy": { "type": "number" }
              },
              "required": ["type"],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Press, release, or click a mouse button at the cursor, or at optional window points (x, y) that move the cursor there first.",
              "properties": {
                "type": { "const": "mouse_button" },
                "button": { "enum": ["left", "middle", "right", "x1", "x2"] },
                "action": { "enum": ["down", "up", "click"], "default": "click" },
                "x": { "type": "number" },
                "y": { "type": "number" },
                "clicks": { "type": "integer", "minimum": 1, "maximum": 3, "default": 1 }
              },
              "required": ["type", "button"],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Scroll the wheel; 1.0 is one notch and positive y scrolls down.",
              "properties": {
                "type": { "const": "scroll" },
                "x": { "type": "number", "default": 0 },
                "y": { "type": "number" }
              },
              "required": ["type", "y"],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Release every key and mouse button the library holds.",
              "properties": {
                "type": { "const": "release_all" }
              },
              "required": ["type"],
              "additionalProperties": false
            }
          ]
        }
      },
      "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 },
      "region": {
        "type": "object",
        "description": "Crop rectangle in pixels of the full frame, top-left origin.",
        "properties": {
          "x": { "type": "integer", "minimum": 0 },
          "y": { "type": "integer", "minimum": 0 },
          "width": { "type": "integer", "minimum": 1 },
          "height": { "type": "integer", "minimum": 1 }
        },
        "required": ["x", "y", "width", "height"],
        "additionalProperties": false
      }
    },
    "required": ["actions"],
    "additionalProperties": false
  },
  "annotations": {
    "readOnlyHint": false,
    "idempotentHint": false,
    "openWorldHint": false
  }
}
```

Behaviour:

- 1–256 actions, ordered, validated as a whole, queued as one unit, and applied
  in a single `SDL3MCP_AppIterate`, so no frame boundary can appear inside a
  gesture.
- At most one batch is in flight.
- The result is raw framing: a result line with `format`, `width`, `height`,
  `byteLength`, `frame`, and `text`, followed by exactly `byteLength` bytes. The
  `text` format is
  `<N> actions applied at frame F; frame A attached, rendered after the actions were delivered`.
  `F` is the applied frame; `A` is the attached frame. `A` is always rendered
  after the actions were delivered — one acknowledgement frame and at least one
  capture frame later — and the gap is not fixed. An unchanged frame means the
  actions had no effect.
- `timeout_ms` is 1–60000, defaults to twice `frame_timeout_ms`, and bounds the
  whole call: the F+1 acknowledgement plus the frame wait.
- `region` uses the same parser and rules as `sdl_capture`; the framed dimensions
  are the region's. It is in pixels of the full frame, while action coordinates
  are window points.
- Structural problems fail the tool call before queueing, naming the offending
  index (for example `actions[2]: x and y must be provided together`).
- Key names and chords resolve at apply time on the app thread. An unknown key,
  malformed chord, unknown chord modifier, or SDL refusing to start text input
  fails the call at F+1 as an `isError` result.
- A `down` on a key that only exists with a modifier (such as `!`) is refused at
  apply time; use `press`.
- An input batch the app rejected, or a key it could not resolve, returns an
  `isError` content result with no frame.
- When the input applies but no frame arrives in the remaining budget, the result
  is an `isError` content result naming the applied frame and why the frame is
  missing; the input is never reported as failed.
- Browser: `sdl_input` never waits. It posts the batch and returns a deferred
  result; the settled batch begins the acknowledgement capture, and the frame
  that follows answers the call. A capture already pending refuses that second
  stage, and the input call answers with the refusal instead of dropping the
  other request (§7.7).

### 4.3 `sdl_capture` (library)

Schema:

```json
{
  "name": "sdl_capture",
  "description": "Ask the application for a fresh frame through SDL3MCP_FrameRequested() and return its raw pixels. Arguments: optional timeout_ms (1-60000, default frame_timeout_ms) and optional region. The result is one JSON-RPC line {\"format\":\"rgba8888\",\"width\":W,\"height\":H,\"byteLength\":B,\"frame\":F}, immediately followed by exactly B tightly packed RGBA bytes with no newline and no base64. F is the app frame whose pixels these are; the supervisor names it in the image summary. region is a crop rectangle in pixels of the full frame with a top-left origin and must fit inside capture.lastWidth x capture.lastHeight from sdl_info. sdl_input coordinates are window points, not frame pixels: sdl_info reports pixelWidth, pixelHeight and pixelDensity so a caller can convert between the two. A capture requested before the application has rendered is answered by the first frame the library trusts, not by the first frame, and the request waits rather than failing: the first rendered frame carries no draw data and would show only the clear colour, and kFirstServableFrame (src/capture.hpp, applied by servableFrame in src/capture.cpp) is the single place that decides that bound. Example: {\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360},\"timeout_ms\":2000}",
  "inputSchema": {
    "type": "object",
    "properties": {
      "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 },
      "region": {
        "type": "object",
        "description": "Crop rectangle in pixels of the full frame, top-left origin.",
        "properties": {
          "x": { "type": "integer", "minimum": 0 },
          "y": { "type": "integer", "minimum": 0 },
          "width": { "type": "integer", "minimum": 1 },
          "height": { "type": "integer", "minimum": 1 }
        },
        "required": ["x", "y", "width", "height"],
        "additionalProperties": false
      }
    },
    "additionalProperties": false
  },
  "annotations": {
    "readOnlyHint": true,
    "idempotentHint": false,
    "openWorldHint": false
  }
}
```

Behaviour:

- `region` is a crop in **captured-frame pixels** — the
  `capture.lastWidth` × `capture.lastHeight` from `sdl_info` — top-left origin.
  All four fields are required, `x`/`y` at least 0, `width`/`height` at least 1,
  and the rectangle must fit inside the frame. The library crops; the Handler
  encodes the result at those same dimensions. Captures are never resized.
- `timeout_ms` is 1–60000 and defaults to `frame_timeout_ms`.
- Any other property is rejected before a frame is requested.
- A successful call is one result line followed by exactly `byteLength` raw
  bytes: `format` always `"rgba8888"`, `byteLength == width * height * 4`, and
  exactly that many bytes before the next JSON-RPC message. `width`/`height`
  are the region's when one was given, otherwise the app-supplied frame's.
- The library's `sdl_capture` result has no `text`; `sdl_input` adds one.
- On timeout the result is an ordinary content error, not a frame:
  `no frame arrived within <N> ms. The app must call SDL3MCP_WriteFrame() when SDL3MCP_FrameRequested() returns true, with tightly packed RGBA8888, its width, and its height.`
  followed by ` Last drop: <reason>` when there was a drop.
- A capture fails immediately if no window is bound, if the region does not fit,
  or if the app never answers in time. A timeout is a normal content result with
  `isError: true`.
- Only one capture is pending at a time; tool calls are serialized on the server
  thread, so a slow capture delays the next tool call.
- A capture requested before the app has rendered its first frames waits; it is
  never answered by the first rendered frame. The first servable frame is
  `kFirstServableFrame = 1` completed frame (`src/capture.hpp:51`,
  `src/capture.cpp:8-11`). This is observable behaviour: the request waits
  rather than failing, and the first frame it can use is the one after the first
  completed frame.

### 4.4 `sdl_terminate`

Schema:

```json
{
  "name": "sdl_terminate",
  "description": "Ask the application to exit cleanly: the next SDL3MCP_AppIterate() pushes one SDL_EVENT_QUIT so the app's own event loop can shut down. Takes no arguments. Example: {}",
  "inputSchema": {
    "type": "object",
    "properties": {},
    "additionalProperties": false
  },
  "annotations": {
    "readOnlyHint": false,
    "destructiveHint": true,
    "idempotentHint": false,
    "openWorldHint": false
  }
}
```

Behaviour:

- No arguments. Sets a flag and returns the text `termination requested` with
  `isError: false`. It never calls SDL and never ends the process.
- On the next `SDL3MCP_AppIterate`, after the usual batch and snapshot work, the
  library exchanges the flag and pushes exactly one zeroed `SDL_EVENT_QUIT` with
  `SDL_PushEvent`. If the push fails, the flag is restored and the next iterate
  retries. Either way `SDL3MCP_AppIterate` still returns `SDL_APP_CONTINUE`, so
  the app's own quit handling runs unchanged. A second iterate with no new
  request pushes nothing.
- On desktop the handler runs on the server thread; on the browser it runs on
  the app thread during the pump.

---

## 5. Raw frame framing and presentation

### 5.1 Library result

`sdl_capture` and `sdl_input` are the two tools whose library result is not a
content array. They write:

```json
{"format":"rgba8888","width":W,"height":H,"byteLength":B,"frame":F}
```

followed by exactly `B` tightly packed RGBA bytes, with no newline and no
base64. `sdl_input` adds `"text"`, the line naming the applied and attached
frames; `sdl_capture` never has one. `F` is the attached frame.

`frame` is part of the header in `src/responder.cpp:76-79`. Two README examples
(lines 2053 and 1566) omit it; those examples are stale and MUST NOT be used as
the contract. The v0 `sdl_input` README example (lines 1974-1977) includes it
and is correct.

### 5.2 Handler presentation

The Handler advertises its own `sdl_capture` definition in every mode, because
presentation belongs to the Handler. It MUST:

- read the child's result line and validate `format == "rgba8888"`, positive
  dimensions, and `byteLength == width * height * 4`;
- reject a frame larger than 64 MiB before allocating the payload;
- treat a mismatched header or a short payload as a tool error;
- encode the frame as PNG at exactly the dimensions received, never resized;
- base64-encode it and return the standard content block:

```json
{"content":[
  {"type":"text","text":"<summary or the library's text>"},
  {"type":"image","mimeType":"image/png","data":"<base64>"}
],"isError":false}
```

- for `sdl_capture`, summarise the size, the region when one was given, the PNG
  size, and the frame, for example
  `960x540 PNG at 0,0, 41234 bytes; frame 31 attached`;
- for `sdl_input`, keep the library's text, for example
  `1 actions applied at frame 12; frame 14 attached, rendered after the actions were delivered`;
- pass a JSON-RPC error object or a text error result from the child through
  unchanged;
- when the peer is itself a Handler, relay an already-presented `content` result
  untouched; only a raw `format`/`width`/`height`/`byteLength` frame is encoded,
  and exactly once, at the hop nearest the app.

The Handler's `sdl_capture` schema:

```json
{
  "name": "sdl_capture",
  "description": "Ask the application for a fresh frame and return it as a PNG image block at the app's native pixel size. Arguments: optional timeout_ms (1-60000, default the app's frame timeout) and optional region. region crops pixels from the captured frame with a top-left origin and must fit inside capture.lastWidth x capture.lastHeight from sdl_info. sdl_input coordinates are window points, while region is in pixels of the full frame; sdl_info reports pixelWidth, pixelHeight and pixelDensity so a caller can convert between them. Captures are never resized, so region is the only way to keep the payload small. The text part names the frame attached, so a caller can tell a fresh frame from a stale one. Example: {\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360},\"timeout_ms\":2000}",
  "inputSchema": {
    "type": "object",
    "properties": {
      "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 },
      "region": {
        "type": "object",
        "description": "Crop rectangle in pixels of the full frame, top-left origin.",
        "properties": {
          "x": { "type": "integer", "minimum": 0 },
          "y": { "type": "integer", "minimum": 0 },
          "width": { "type": "integer", "minimum": 1 },
          "height": { "type": "integer", "minimum": 1 }
        },
        "required": ["x", "y", "width", "height"],
        "additionalProperties": false
      }
    },
    "additionalProperties": false
  },
  "annotations": {
    "readOnlyHint": true,
    "idempotentHint": false,
    "openWorldHint": false
  }
}
```

---

## 6. Transports and configuration

### 6.1 App command line

`SDL3MCP_AppInit` reads only arguments beginning with `--mcp`; everything else is
left untouched for the app.

| Argument | Result |
| --- | --- |
| *(none)* | Use `cfg.transport`, default `NONE`. |
| `--mcp` | Same as `--mcp=stdio`. |
| `--mcp=stdio` | Start the stdio server. |
| `--mcp=HOST:PORT` | Dial an MCP endpoint and serve over that socket (`[V6]:PORT` accepted). |
| `--mcp=off` | Start nothing; overrides a configuration that enabled stdio. |

- Transport values are case-insensitive.
- A dialed endpoint is one connection with no retry. If the host cannot be
  resolved or reached, `SDL3MCP_AppInit` returns `SDL_APP_FAILURE` and
  `SDL_GetError()` names the endpoint. It never falls back to stdio or to no
  server.
- Any other value prints usage to stderr and fails the same way.

Tuning flags, all optional; value flags use `=`:

| Flag | Effect |
| --- | --- |
| `--mcp-timeout-ms=N` | Override `frame_timeout_ms` (1–60000). |
| `--mcp-max-frame-bytes=N` | Override `max_frame_bytes` (1 MiB–1 GiB). |
| `--mcp-no-warp-cursor` | Set `warp_cursor = false`. |
| `--mcp-log-level=off\|error\|warn\|info\|debug` | Override `log_level`. |
| `--mcp-help` | Print usage to stderr, return `SDL_APP_SUCCESS`. |
| `--mcp-version` | Print the library version to stderr, return `SDL_APP_SUCCESS`. |

`--mcp-version` prints `SDL3MCP 0.1.0` in v0 (`src/configuration.cpp:385-388`;
`CMakeLists.txt:3`). Unknown `--mcp*` arguments are errors, not ignored. Usage
and version output always go to stderr.

### 6.2 Desktop versus Emscripten

- Desktop builds accept `NONE`, `STDIO`, and `CONNECT`.
- Emscripten builds accept only `NONE` and `WEBSOCKET`, because a page has
  neither a stdio server nor a second thread.
- On Emscripten, `--mcp=off` and `--mcp=HOST:PORT` are the only values: no
  `stdio` value, no `--mcp-help`, no `--mcp-version`, and neither `usageText()`
  nor `versionText()` is compiled.
- The browser reads `window.location.search`, takes the `mcp` parameter,
  percent-decodes it, and hands the same `--mcp=<value>` argument to the shared
  configuration parser. One grammar and one validator serve both platforms.
- A page without the parameter starts no server.
- The `ws://` scheme is added by the peer when it dials, never by configuration;
  `connect_host` never carries a scheme.

### 6.3 The library never listens

The default transport is `NONE`. The library opens no listening socket, no port,
and no authentication. `CONNECT` dials exactly the configured endpoint and
listens on nothing. Only the Handler binds a port. A failed dial is a loud
startup failure, not a silent fallback.

---

## 7. Frame pipeline

### 7.1 Pull-based capture

1. The client calls `sdl_capture`, or `sdl_input` once its batch has been
   acknowledged.
2. The server validates the arguments, checks that a window is bound, marks a
   request pending, and waits up to `timeout_ms`.
3. On the next frame, the app's `SDL3MCP_FrameRequested()` returns true.
4. The app renders, reads its own pixels, and calls `SDL3MCP_WriteFrame`.
5. `SDL3MCP_WriteFrame` validates the byte count against the width and height the
   app supplies, copies it, clears the pending request, and wakes the server
   thread.
6. The server crops the optional region and answers with the raw framing (§5.1).
7. The app continues; nothing blocked.

### 7.2 Rules

- The returned frame is one written **after** the request arrived; stale frames
  are never served.
- A capture requested before the app has rendered is not answered by the first
  rendered frame; it stays pending and is answered by the first servable frame
  (`kFirstServableFrame = 1` completed frame). The request waits rather than
  failing; it is never cancelled or rejected for arriving early.
- Only one capture is pending at a time. Tool calls are serialized on the server
  thread, so a slow capture delays the next call until it completes or times out.
  `sdl_input` uses the same pipeline for the frame it returns.
- A frame is dropped when `size` is not exactly `width * height * 4` for the
  supplied dimensions, when the data pointer is null, when a dimension is not
  positive, or when the frame exceeds `max_frame_bytes`. A drop leaves the
  request pending so the app can retry; the last reason is appended to the
  timeout message as `Last drop: ...` and is visible in `sdl_info` as
  `capture.lastError`, with `capture.dropped` counting the attempts.
- The library does only the crop on the server thread: no resize and no image
  encoding happen in the app process.
- The Handler encodes the cropped frame unchanged. It has no resizing option:
  image pixels map 1:1 to app pixels, so coordinates measured in the returned
  PNG are app coordinates. `region` is the only lever for payload size.

### 7.3 Region

`region` is `{x, y, width, height}` in pixels of the captured frame, top-left
origin, all four required. `x`/`y` at least 0, `width`/`height` at least 1, and
the rectangle must fit inside `capture.lastWidth` × `capture.lastHeight`.
`cropFrame` rejects anything that would read outside the frame instead of
clamping. `sdl_input` takes the same object and parses it with the same helper,
so both tools accept exactly the same region. Captures are never downscaled;
`region` is the only payload-size lever.

### 7.4 Timeouts and errors

- `sdl_capture` default timeout: `frame_timeout_ms`.
- `sdl_input` default timeout: twice `frame_timeout_ms`, bounding apply plus
  capture.
- Both are clamped to the published 1–60000 range.
- A capture timeout is an `isError` content result, not a JSON-RPC error.
- `sdl_input` distinguishes: input rejected (error, no frame), input applied but
  no frame (error naming the applied frame, input not reported as failed), and
  structural error before queueing.

### 7.5 Browser deferred path

Emscripten has no `Server` and no second thread. `WebSocketPeer` dials with
`emscripten_websocket_new`; its callbacks copy each frame into a byte queue;
`pump()` hands complete lines to the `Responder`, whose answers leave through
`emscripten_websocket_send_binary`. Callbacks, `pump()`, and every write share
the page's main thread, so the queue needs no lock.

- `SDL3MCP_AppIterate` pumps at the top of the frame, before the frame counter,
  so a request registered there is answered by a frame the loop has not produced
  yet; it then completes a deferred call after the frame work, so a capture
  carries the frame the app answered with.
- `sdl_capture` and `sdl_input` never wait: they register the request and return
  a result marked deferred, which `Responder` leaves unanswered. A later frame
  settles it and writes the answer through `Responder::answer`, with
  `frame_timeout_ms` as the deadline. One deferred call at a time, so a capture
  and a batch never race for the same frame.
- `sdl_input` spends two turns: the settled batch begins the acknowledgement
  capture, and the frame that follows answers the call, with the budget
  restarting at the settle. A capture already pending refuses that second stage
  and the input call answers with the refusal instead of dropping the other
  request.
- Nothing on this path may block — not the pump, not a handler, not a write.

---

## 8. Input model

### 8.1 Batch

`sdl_input` takes an ordered array of 1–256 actions. The batch is validated as a
whole, queued as one unit, and applied in a single `SDL3MCP_AppIterate`, so no
frame boundary can appear inside a gesture.

### 8.2 Actions

| Action | Fields |
| --- | --- |
| `key` | `key` (name or chord), `action` = `down`/`up`/`press` (default `press`), optional `modifiers`. |
| `text` | `text` (UTF-8, 1–1024 bytes). |
| `mouse_move` | `x`,`y` (window points) **or** `dx`,`dy` (relative), never both and never half a pair. |
| `mouse_button` | `button` = `left`/`middle`/`right`/`x1`/`x2`, `action` = `down`/`up`/`click`, optional `x`,`y` (moves the cursor there first), `clicks` 1–3. |
| `scroll` | `x` (default 0), `y` in wheel notches. |
| `release_all` | Releases every key and button the library holds. |

There is no `repeat` field. A tap is a `press`; to repeat it, send another press.
Auto-repeat is a repeated `down` without an intervening `up`, and that event's
`repeat` flag is set. Modifiers are valid only on `press`.

### 8.3 Validation and error timing

- Structural problems fail before anything is queued: unknown action type or
  property, missing required field, bad `mouse_move` shape, `click` count outside
  1–3, modifiers on `down`/`up`, more than four modifiers, unknown modifier name,
  empty or over-long key/text, or more than 256 actions. The message names the
  offending index, for example
  `actions[2]: x and y must be provided together`.
- Key names and chords resolve at apply time on the app thread, because a single
  character depends on the current keyboard layout. An unknown key, malformed
  chord, unknown chord modifier, or SDL refusing to start text input fails the
  `sdl_input` call at F+1 as an `isError` result.
- A `down` on a key that only exists with a modifier (such as `!`) is refused at
  apply time; use `press`, which supplies the modifier itself.

### 8.4 Keys

- Names are matched case-insensitively against SDL's scancode names, limited to
  64 bytes. `-` and `_` are word separators, so `left-ctrl`, `left_ctrl`, and
  `left ctrl` are the same key; SDL's own names keep their punctuation (`-`,
  `keypad +`).
- Aliases cover common spellings: `enter`, `esc`, `del`, `ins`, `pgup`, `pgdn`,
  `ctrl`, `control`, `shift`, `alt`, `option`, `gui`, `cmd`, `command`, `super`,
  `win`, `windows`, and `keypad plus`/`minus`/`multiply`/`divide`/`period`/
  `comma`/`equals`.
- Single printable characters are accepted and resolved against the active
  layout at apply time, including characters SDL has no scancode name for.
- Chords use `+`: `ctrl+shift+s`, `alt+f4`. An optional `modifiers` array (at
  most four entries) is merged into the chord. Modifiers are only valid on
  `press`; a held `down` carries no modifier mask.
- `press` is `down` immediately followed by `up`. On an already-held key the
  key-down carries `repeat`, and the following up still releases the key, because
  `press` always ends released.
- A key sent with `down` stays down across frames until an `up`, a
  `release_all`, or `SDL3MCP_AppShutdown`. An `up` for a key the library is not
  holding is an error.

### 8.5 Text

`text` sends one `SDL_EVENT_TEXT_INPUT` carrying the whole UTF-8 string, not key
events. The schema's `maxLength` is 1024 and the implementation counts UTF-8
bytes. Text requires only a bound window: when text input is not active on that
window, the library starts it for the batch and restores the previous state once
the events are queued. Only a window SDL refuses to start text input on still
produces the `text input is not active on the bound window` error.

The event's `text` pointer refers to the queued batch, which the library keeps
alive until frame F+1. The app must consume it during that window and must not
retain the pointer.

### 8.6 Mouse, scroll, modifiers

- Absolute coordinates are in window points, the same space as
  `SDL_EVENT_MOUSE_MOTION.x/y`, and are clamped to the window.
- The library tracks a virtual cursor. `xrel`/`yrel` are derived from the move,
  except when `warp_cursor` is on and the platform's warp event already carries
  the delta; then the synthetic event leaves its delta at zero so delta
  accumulators do not count the move twice.
- With `warp_cursor = true` (default), an absolute move also warps the OS cursor
  with `SDL_WarpMouseInWindow`, so `SDL_GetMouseState` and hit testing follow the
  synthetic motion. No window focus is required. A synthetic zero-delta motion
  event always accompanies the warp, so event-driven apps see the move even when
  the platform ignores the warp.
- In relative mouse mode, warping is skipped and absolute moves are converted to
  deltas from the virtual cursor. `dx`/`dy` pass through as `xrel`/`yrel`.
- `click` is a full down/up pair with the requested `clicks` count. Pressing a
  held button, or releasing an unheld one, fails the action.
- A `mouse_button` with `x`/`y` performs the same absolute move first, in the
  same action: one synthetic motion at the clamped target, the warp under
  `warp_cursor`, then the button events. Without `x`/`y` it emits only button
  events and never warps.
- Buttons and wheel use `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` and
  `SDL_EVENT_MOUSE_WHEEL` on the bound window.
- `scroll` sends one `SDL_EVENT_MOUSE_WHEEL` at the virtual cursor position.
  `1.0` is one notch and positive `y` scrolls down; `x` defaults to 0. Integer
  wheel fields stay 0; values pass through unchanged.
- The library maintains a virtual modifier mask, sets it on every key event, and
  mirrors it with `SDL_SetModState`, so `SDL_GetModState` agrees. Releasing a
  modifier or `release_all` restores the mask.

### 8.7 Keyboard-state caveat

Injected input is event-based. SDL maintains `SDL_GetKeyboardState` from its own
backend key handlers, not from events pushed onto the queue, and SDL3 has no
public per-key setter. Apps that handle `SDL_EVENT_KEY_DOWN`/`UP` see
everything; apps that only poll `SDL_GetKeyboardState` for movement do not move
in response to synthetic keys. The mouse is different: `warp_cursor` makes
`SDL_GetMouseState` follow injected motion, so polling mouse code mostly works.
This is a documented limitation, not a bug to work around with SDL internals.

---

## 9. Handler executable

### 9.1 v0 topology (for reference)

| v0 mode | CLI | MCP endpoint | Downstream peer | Own tools |
| --- | --- | --- | --- | --- |
| child supervisor | `--target <executable> [args...]` | own stdio | one child over private pipes | `spawn` + four app tools |
| relay | `--listen=HOST:PORT` | own stdio | one app that dials in | `wait_session` + four app tools |
| spawner | `--connect=HOST:PORT --targets=PATH [--listen=HOST:PORT]` | dials a relay | one spawned app over a private listener | `app_spawn_<alias>` per target + four app tools |
| web spawner | `--web-root=DIR ...` | none | launches page server, websocat, browser | none |

The client-facing endpoint stays the same across app restarts. The advertised
tool set is a function of the mode, never of whether an app is attached.

### 9.2 Rewrite topology (settled)

The Handler has one shape with an optional upstream:

- **Upstream face:** stdio when `--connect` is absent (a Server); TCP when
  `--connect=HOST:PORT` is present (a Relay). The Handler is MCP server on this
  face in both cases: it answers `initialize`, `ping`, and `tools/list`.
- **Downstream face:** one listener (`--listen=HOST:PORT`) plus zero or more app
  sources (`--targets=PATH`, `--web=PATH`). The listener accepts a spawned app,
  a standalone library, or a `websocat` bridge; the Handler is MCP client to it.
- **All Relays are also Servers.** `--connect` only adds the upstream link; the
  listener and app sources are identical in both cases.
- **App sources** contribute `app_spawn_<alias>` tools. A `--targets` entry
  spawns the configured app; a `--web` entry starts a Handler-owned web run
  (page server, bridge, Firefox).
- `--listen` defaults to `127.0.0.1:0` when an app source is present and is
  required otherwise. A Handler needs at least one downstream source:
  `--listen`, `--targets`, or `--web`. `--connect` with no downstream source is
  a startup error.

`--target` is removed. There is no child-supervisor mode and no `spawn` tool.
Run 1 uses a Server plus an app launched separately with `--mcp=HOST:PORT`.

### 9.3 Shared Handler contract

- `initialize`, `ping`, and `tools/list` are always answered by the Handler.
- A forwarded `tools/call` whose name the peer did not advertise is answered
  with `the application does not provide <name>` instead of being sent.
- The advertised list is a function of topology, never of whether an app is
  attached. A spawn, attach, or detach that changes no advertised name sends no
  `notifications/tools/list_changed`.
- A notification is sent only when the merged set really changes.
- The Handler never treats a peer's announcement as the list itself: on
  `notifications/tools/list_changed` from an attached peer, it asks that peer for
  `tools/list` again, re-merges it, and republishes the result.
- The Handler merges the attached peer's entries over its base by name: the peer
  wins on a collision except `sdl_capture`, which always keeps the Handler's PNG
  definition. Names only the peer advertises are appended. A bare app is
  therefore an identity.
- A forwarded call that carries `timeout_ms` gets that budget plus a grace.
  Without one, `sdl_input` gets twice the default frame timeout (it waits for the
  apply and then the frame); every other call gets one frame timeout.
- One forward path serves all tools: a result with `content` is relayed
  verbatim; a result with `byteLength` is read, PNG-encoded, and answered as
  text plus one image block (the frame's own `text` when it has one, the capture
  summary otherwise).
- The base list is `wait_session` plus the four app tools; app sources append
  `app_spawn_<alias>`. Before an app attaches, every app call fails with
  `no application session is attached; call wait_session first`. This replaces
  v0's spawner-only
  `no application is attached; call one of the app_spawn_* tools`.
- `wait_session` does not chain: it waits for the Handler's own downstream peer,
  not for a relay's downstream app.
- `sdl_terminate` is forwarded like any other running tool. The Handler does not
  wait for the exit or force it; there is no watchdog.
- Spawn-and-forget: the Handler starts an app, waits for its handshake, and then
  never signals, waits for, or reaps it. `SIGCHLD` is ignored or `SA_NOCLDWAIT`
  is set; the Handler tracks the session, not the process. On Handler shutdown
  the app keeps running.
- One Handler `--timeout-ms` (default 10000) is the default for upstream
  dial/attach, downstream attach, `wait_session`, and `app_spawn_*` when the call
  omits `timeout_ms`.
- The Handler never calls SDL. It links the library's SDL-free wire half and
  must not link `SDL3::SDL3`.

### 9.4 Server details (no `--connect`)

A Server is the Handler without `--connect`; its upstream is stdio. The listener
and app-source behaviour is shared with Relay.

- `--listen=HOST:PORT` may be given once. `[V6]:PORT` is accepted; port 0 asks
  the kernel for an ephemeral port. The bound endpoint is printed to stderr as
  `sdl3mcp: listening on HOST:PORT`.
- A bind failure is a startup error and the process exits non-zero.
- `wait_session`:
  - optional `timeout_ms` 1–60000; without it the call uses the Handler
    `--timeout-ms` (v0 waited indefinitely; this is a deliberate delta);
  - an expired timeout is a normal `isError: true` content result, not a
    JSON-RPC error;
  - returns `an application session attached`, or
    `an application session is already attached` when one is up;
  - requests are serialized, so a parked `wait_session` delays `ping`.
- Before a session attaches, every forwarded `tools/call` fails with
  `no application session is attached; call wait_session first`.
- Attach completes a private MCP handshake (`initialize`, the initialized
  notification, and `tools/list`), then merges the peer's entries.
- Exactly one application session is accepted at a time. A second connection is
  closed as soon as it is accepted, not queued, and never handed to the proxy.
- When the attached peer's socket closes, the Handler drops the peer's merged
  entries and keeps the client's stdio endpoint open for the next app. The
  hang-up is probed on every loop pass.
- A forwarded call is bounded: when the peer stops answering, the Handler
  detaches it, returns a normal error content block naming the tool, and is back
  in `wait_session`.
- `sdl_capture` keeps the Handler presentation contract while attached; every
  other call, including `sdl_terminate`, is forwarded verbatim.
- A Server may also have `--targets`/`--web`; then it advertises `app_spawn_*`
  alongside `wait_session` and the four app tools.
- Client stdin EOF, SIGINT, SIGTERM, and a normal return close the listener and
  the attached session and exit. A spawned or dial-in app is not killed;
  spawn-and-forget means it keeps running.

### 9.5 Relay details (`--connect`)

A Relay is a Server with `--connect`; its upstream is TCP. The listener and
app-source behaviour is shared with Server.

- `--connect=HOST:PORT` is required and given once. A failed dial is fatal: the
  process prints `--connect:` with the endpoint and exits non-zero.
- `--targets=PATH` and `--web=PATH` are app sources. `--targets` is repeatable
  and accepts a JSON file or a directory of `*.json` files; `--web` is repeatable
  and accepts a JSON file mapping alias → web root. At least one downstream
  source (`--listen`, `--targets`, or `--web`) is required.
- `--listen=HOST:PORT` is the downstream app listener. It defaults to
  `127.0.0.1:0` when an app source is present; otherwise it is required. With an
  explicit `--listen`, a bind failure is a startup error; it never silently
  falls back to an ephemeral port.
- `tools/list` is `wait_session` plus the four app tools plus one
  `app_spawn_<alias>` per targets/web entry, in every state. The set does not
  change when an app is spawned or exits, so no
  `notifications/tools/list_changed` is sent around a spawn.
- A web `app_spawn_<alias>` starts the Handler-owned page server, `websocat`
  bridge, and Firefox, waits for the browser app to connect, and kills the page
  server and bridge when Firefox exits; see §10.
- Before an app is attached, an app `tools/call` fails with
  `no application session is attached; call wait_session first`.
- One app runs at a time. Calling another `app_spawn_<alias>` while one is
  attached fails with `an application is already attached; one spawn at a time`.
- `app_spawn_<alias>` has an optional `timeout_ms` 1–60000, defaulting to the
  Handler `--timeout-ms`, covering bind, exec, dial-back, and handshake.
  Outcomes:
  1. success: `spawned <alias> (pid N)`, tool list unchanged, app calls forward;
  2. app died first: `isError` result names the exit status or `exec` failure and
     the pid, tool list unchanged;
  3. timeout: error names the timeout and the pid; the Handler does not kill the
     child (spawn-and-forget), so a failed spawn may leave a process behind.
- A bind or `exec` failure is reported like outcome 2, with the reason in the
  error text. After any failure the Handler is idle and a later
  `app_spawn_<alias>` may be tried.
- `sdl_terminate` is forwarded like any other app tool; the Handler does not wait
  for the exit or force it. When the app exits, the Handler returns to idle with
  the same list, so nothing is announced. A forwarded call to an app that stopped
  answering is bounded.
- The Relay lives exactly as long as its upstream connection: when the upstream
  closes the socket, the run loop ends and the Handler exits. It does not stop or
  reap the app; spawn-and-forget means the app keeps running. SIGINT and SIGTERM
  take the same path. The spawned app does not inherit the upstream connection or
  the listener, so the upstream sees the socket close as soon as the Handler
  dies.

### 9.6 Targets file and `sdl3mcp_app`

Each source is a file or a directory:

- a regular file is a JSON object, read as one source;
- a directory contributes every `*.json` regular file at its top level, read in
  sorted filename order; other files are ignored; a directory without any
  `*.json` file is an error.

All sources accumulate into one map. An alias may be defined by only one source;
seeing it twice is a startup error naming both paths.

In a file, each key is an alias and each value is the app's argument vector,
executable first:

```json
{
  "demo": ["./build/examples/minimal"],
  "probe": ["/opt/probe/probe", "--fast", "--no-audio"]
}
```

The spawner appends `--mcp=HOST:PORT` as the last argument, naming the listener it
bound, so an entry must not already carry an MCP argument.

Validation happens at startup; a failure is fatal before any socket or child
exists and names the offending path, file, or entry:

- every path must exist and every file must open, inside a directory included;
- each file must parse as JSON;
- each file's top level must be a non-empty JSON object;
- every key must match `[a-z0-9_]+` and must not begin with the reserved
  `app_spawn_` prefix;
- every value must be a non-empty array of strings;
- no element may contain `--mcp` anywhere;
- the accumulated map must not be empty;
- an alias may not appear in two sources.

`--web` is an app source with the same alias rules as `--targets`, but each
entry names a web root instead of an app argv. The entry page is always
`index.html`; the Handler owns `python3`, `websocat`, Firefox, and the throwaway
profile (see §10).

`sdl3mcp_app(<alias> <target> [ARGS <arg>...])` records an executable target and
writes the targets file at generate time:

- alias required, `[a-z0-9_]+`, `app_spawn_` prefix rejected;
- target must exist; an alias may be registered once per configure;
- the first element is `$<TARGET_FILE:<target>>`;
- the file lands at `${CMAKE_BINARY_DIR}/sdl3mcp-targets.json`;
- registration appends to a global property; the write is deferred to the end of
  the top-level directory; no registration means no file;
- extra `ARGS` become trailing arguments, escaped for JSON; the `--mcp` rule
  still applies.

### 9.7 Handler shutdown and signals

- stdin EOF, SIGINT/SIGTERM, and a normal return close the listener and the
  attached session and exit. The Handler does not signal, wait for, or reap any
  app; spawn-and-forget means a spawned or dial-in app keeps running. `SIGCHLD`
  is ignored or `SA_NOCLDWAIT` is set, so the kernel reaps.
- Signals are blocked process-wide and consumed by a `sigwait` thread because the
  handler cannot take the state or writer mutexes.
- There is no terminate watchdog. `sdl_terminate` is forwarded; the app decides.
  A bounded forward recovers a hung session.
- In Relay mode, the upstream connection closing ends the Handler; the downstream
  app keeps running.

### 9.8 Handler CLI grammar

v0 (for reference):

```text
sdl3mcp [--startup-timeout-ms N] [--terminate-timeout-ms N] (
  --target <executable> [app args...] |
  --listen=HOST:PORT |
  --connect=HOST:PORT --targets=PATH [--targets=PATH ...] [--listen=HOST:PORT] |
  --web-root=DIR [--web-entry=NAME] [--web-port=PORT] [--bridge-port=PORT]
    [--connect=HOST:PORT] [--print-plan]
)
```

Rewrite (settled deltas):

```text
sdl3mcp [--timeout-ms N] [--dry-run=ALIAS] (
  [--listen=HOST:PORT]
  [--connect=HOST:PORT]
  [--targets=PATH ...]
  [--web=PATH ...]
)
```

- At least one downstream source is required: `--listen`, `--targets`, or
  `--web`.
- `--listen` defaults to `127.0.0.1:0` when `--targets` or `--web` is present;
  otherwise it is required.
- `--connect` selects Relay (upstream TCP); without it the Handler is a Server
  (upstream stdio).
- `--target` is removed. `--startup-timeout-ms` and `--terminate-timeout-ms` are
  replaced by `--timeout-ms` (default 10000).
- `--web-root` is removed; `--web` is an app-source path (alias → web root,
  entry `index.html`).
- `--dry-run=ALIAS` prints the command(s) the `app_spawn_<alias>` tool would run
  (for a web entry, the three commands, page URL, bridge endpoint, and profile
  path) and starts nothing. It replaces v0's `--print-plan` and covers both app
  sources.
- The binary stays `sdl3mcp`; the project version becomes `1.0.0`.

---

## 10. Web app source and Handler-owned web run

### 10.1 `--web` dict

`--web=PATH` is an app source. PATH is a JSON file whose top level is a
non-empty object mapping alias → web root:

```json
{"demo": "build/web", "other": "/srv/web"}
```

- The entry page is always `index.html` inside that root.
- A relative root resolves against the current working directory; absolute paths
  and `..` are accepted as-is. The Handler does not sandbox the root.
- `--web` is repeatable. Each path is a JSON file; there is no directory form.
- Aliases use the same `[a-z0-9_]+` rules as `--targets`, may not start with
  `app_spawn_`, and share one alias map with `--targets`. A collision is a
  startup error naming both paths.
- Each alias contributes an `app_spawn_<alias>` tool.

### 10.2 What a web spawn does

An `app_spawn_<alias>` from a web entry:

1. preflights `python3`, `websocat`, Firefox, the root, and `index.html`;
2. creates a throwaway Firefox profile, seeds `user.js`, and locks it;
3. picks ephemeral page and bridge ports and prints
   `page=http://127.0.0.1:PAGE_PORT/index.html` and
   `bridge=127.0.0.1:BRIDGE_PORT` to stderr;
4. starts, in launch order:
   - `python3 -m http.server PAGE_PORT --bind 127.0.0.1 --directory ROOT`;
   - `websocat --binary ws-l:127.0.0.1:BRIDGE_PORT tcp:HANDLER_ENDPOINT`;
   - `FIREFOX --no-remote --new-instance --profile PROFILE --width 1280 --height 800 http://127.0.0.1:PAGE_PORT/index.html?mcp=127.0.0.1:BRIDGE_PORT`;
5. waits for the browser app to connect to the Handler's listener and handshake,
   or for the spawn timeout;
6. returns `spawned <alias> (browser pid N)` using Firefox's pid.

`HANDLER_ENDPOINT` is the Handler's own listener (`--listen`), which the bridge
dials. The page reads `?mcp=...` and the browser app dials the bridge.

### 10.3 Cleanup

- When Firefox exits, cleanly or not, the Handler kills the page server and the
  bridge and removes the throwaway profile. A clean Firefox exit is the normal
  end of a web run; a non-zero exit is reported on stderr.
- If the page server or bridge exits at any point, the run fails: the Handler
  kills the remaining web processes, removes the profile, and reports the child.
- On spawn timeout, the Handler kills all three and removes the profile.
- On Handler shutdown, the Handler kills all three and removes the profile.
- The web stack is the one thing the Handler owns: it is infrastructure, not the
  app. Spawn-and-forget applies to the app session, not to these three
  processes.
- A SIGKILL of the Handler can leave the web stack behind; the profile lock and
  `sweepAbandonedProfiles` reclaim the profile on the next launch.

### 10.4 `--dry-run=ALIAS`

`--dry-run=ALIAS` replaces v0's `--print-plan` and covers both app sources. It
prints what the `app_spawn_<alias>` tool would run and starts nothing:

- for a `--targets` entry, the app argv, including the endpoint the Handler
  would append;
- for a `--web` entry, the three commands, the page URL, the bridge endpoint,
  and the profile path.

It does not bind a port, create a profile, or start a process. An unknown alias
is an error. When the listener is ephemeral, a target dry run prints the literal
`HOST:PORT` placeholder for the endpoint it would append.

### 10.5 Prerequisites and isolation

The web run keeps the v0 requirements and isolation:

- `websocat` on `PATH` (install with `cargo install websocat`);
- `python3` on `PATH`;
- the root must be a directory containing `index.html`;
- a Firefox binary: `$BROWSER` when it names `firefox` or `firefox-esr`,
  otherwise `firefox` then `firefox-esr` on `PATH`. No fallback browser.
- The browser runs with `--no-remote --new-instance` and a fresh
  `sdl3mcp-web-XXXXXX` profile under `$XDG_RUNTIME_DIR` (or `/tmp`). The
  profile's `user.js` carries `dom.allow_scripts_to_close_windows` and the
  first-run suppression preferences. The run holds an exclusive `flock` on
  `.lock`; `sweepAbandonedProfiles` reclaims abandoned profiles on the next
  launch.

### 10.6 Page query

The spawned URL carries `?mcp=127.0.0.1:BRIDGE_PORT`. The page reads the `mcp`
parameter from `window.location.search`, percent-decodes it, and hands the same
`--mcp=<value>` argument to the shared configuration parser. A page opened
without the parameter starts no MCP server.

---

## 11. Build, platform, and dependencies

### 11.1 Requirements

- SDL 3.2 or newer.
- A C11 compiler for the public header; the implementation is C++20.
- CMake 3.21 or newer.
- A pthreads-capable POSIX platform for the desktop transports, fd isolation,
  and server thread. Windows is a TODO.
- `nlohmann/json.hpp` for the protocol, `stb_image_write.h` for the Handler's
  PNG encoding.

### 11.2 CMake options

| Option | Default | Meaning |
| --- | --- | --- |
| `SDL3MCP_BUILD_SHARED` | `OFF` | Build a shared library instead of a static one. |
| `SDL3MCP_BUILD_EXAMPLES` | `OFF` | Build `minimal` from `examples/minimal/main.c`. |
| `SDL3MCP_BUILD_CLI` | `OFF` | Build the `sdl3mcp` Handler executable. |
| `SDL3MCP_BUILD_TESTS` | `ON` if top-level | Build unit tests and register them with CTest. |
| `SDL3MCP_USE_SYSTEM_JSON` | `ON` | Try `find_package(nlohmann_json CONFIG QUIET)` before the vendored header. |

`CMakePresets.json` adds `debug`, `shared`, `asan`, and `tsan` presets; all four
set `SDL3MCP_BUILD_CLI=ON`. The direct `cmake -S . -B build` flow remains the
documented path.

### 11.3 Targets and object-library split

- `SDL3MCP` is the library, static by default, aliased as
  `SDL3MCP::SDL3MCP`; installed builds export the alias and install
  `<SDL3MCP/SDL3MCP.h>`.
- `sdl3mcp` is the Handler executable.
- `minimal` is the example.
- `sdl3mcp_wire_objects` holds protocol, session, framing, transports, and
  configuration; it references no SDL symbol.
- `sdl3mcp_app_objects` holds input, replay, capture plumbing, and info; it is
  the only half that links `SDL3::SDL3`.
- `SDL3MCP` keeps both halves. The Handler links
  `sdl3mcp_supervisor_objects` and `sdl3mcp_wire_objects` only; no SDL target or
  symbol reaches the relay binary.
- A shared build exports only the six public C entry points; C++ internals stay
  hidden (`CXX_VISIBILITY_PRESET hidden`). Unit tests that include private
  `src/` headers link the object libraries, not the shared object.

### 11.4 Dependencies

- `nlohmann/json.hpp` is resolved system-first (`SDL3MCP_USE_SYSTEM_JSON`) with
  the vendored `third_party/nlohmann` header as fallback. It is exported as the
  `nlohmann_json::nlohmann_json` interface target.
- `stb_image_write.h` is vendored in `third_party/stb` and used only by the
  Handler: a private include and one implementation unit in `supervisor/`,
  compiled into `sdl3mcp_supervisor_objects`. The library archive must not
  contain an stb symbol. There is no `SDL3MCP_USE_SYSTEM_STB` option.
- No other runtime dependencies. The public header includes only SDL3 and the C
  standard headers.

### 11.5 Platform stance

- v0 supports Linux and macOS through the POSIX transport: `poll` for idle and
  shutdown wake-up, `read`/`write` on duplicated descriptors, `dup`/`dup2` for fd
  isolation, `/dev/null` for the host-descriptor redirect, `SIGPIPE` blocked on
  the server thread.
- Windows is a TODO, not implemented: a `WIN32` configure fails with a
  `FATAL_ERROR`. The missing pieces are binary stdio, a Win32 reader wake-up,
  `_dup`/`_dup2` with `NUL`, broken-pipe handling, and test guards.
- The rewrite's scope is Linux plus Emscripten; macOS and Windows are out of
  scope (§12). The v0 macOS claim is therefore a deliberate scope reduction, not
  a silent regression.

### 11.6 Tests

The v0 suite is doctest, registered with CTest:

`header_c`, `header_cpp`, `configuration_tests`, `keymap_tests`, `input_tests`,
`input_json_tests`, `replay_tests`, `mouse_tests`, `queue_tests`,
`app_input_tests`, `input_tool_tests`, `capture_tests`, `capture_tool_tests`,
`terminate_tests`, `json_rpc_tests`, `session_tests`, `frame_tests`,
`info_tests`, `server_tests`, `responder_tests`, and six `lifecycle_*`
scenarios.

With `SDL3MCP_BUILD_CLI=ON`: `supervisor_png_tests`, `supervisor_tests`,
`relay_tests`, `spawner_tests`, and `supervisor_real_app` (the last only when
examples are built). `connect_tests` covers the app's dialed transport. When
Python 3 is available, `readme_check` runs `tests/check_readme.py`.

`tests/check_readme.py` compiles the README's two complete C examples, parses
every JSON block, and checks both `sdl_capture` schemas, the raw-framing prose,
the region's captured-frame dimensions, the four-argument `SDL3MCP_WriteFrame`
signature, the run command, the Handler states, and the loop order. It also runs
`sdl3mcp_test_supervisor --dump-tools` and requires every catalog and README tool
description to carry a parseable `Example:` JSON object naming each argument its
schema accepts.

### 11.7 Test policy

- Behaviour-level tests are the rewrite's oracle and MUST pass before the old
  tree is deleted: JSON-RPC, session, tools, configuration, keymap, input
  parsing, replay, queue, frame validation/crop, info, lifecycle, server,
  responder, relay, spawner, targets, web plan, and the real-app round trip.
- White-box tests that include `src/` headers may be rewritten with the new
  internals; their behaviour must remain covered by the frozen set.
- The three acceptance runs (§12.2) sit above both as the final gate.

---

## 12. Settled rewrite deltas

These are decisions already made for the rewrite. They override the v0 contract
where they conflict; everything else in this document remains MUST.

### 12.1 Naming and CLI

- The executable is the **Handler**, not the supervisor. The binary stays
  `sdl3mcp`; the project version becomes `1.0.0`.
- `--target` and the child-supervisor mode are removed, along with the `spawn`
  tool. Run 1 launches the app separately with `--mcp=HOST:PORT`.
- Every Handler is a Server; a Relay is a Server with `--connect`. The upstream
  face is stdio without `--connect` and TCP with it. The downstream face is
  `--listen` plus zero or more app sources.
- `--targets` and `--web` are app sources. Both read JSON and contribute
  `app_spawn_<alias>`. `--web` maps alias → web root; the entry is always
  `index.html`; the Handler owns the page server, bridge, and Firefox. `--listen`
  defaults to `127.0.0.1:0` when an app source is present.
- Every Handler advertises `wait_session` plus the four app tools; app sources
  append `app_spawn_*`. The no-app error is universal:
  `no application session is attached; call wait_session first`.
- Spawn-and-forget replaces v0's no-orphan rule for apps. The Handler does not
  signal, wait for, or reap an app; each app owns its cleanup. The web stack is
  the exception: the Handler owns and kills the page server, bridge, and Firefox.
- `--dry-run=ALIAS` replaces v0's `--print-plan` and prints what a target or web
  spawn would run without starting anything.
- One Handler `--timeout-ms` (default 10000) replaces `--startup-timeout-ms` and
  `--terminate-timeout-ms` and is the default for `wait_session` and
  `app_spawn_*` when the call omits `timeout_ms`. The library has no frame or
  capture timeout; its only timeout is the `CONNECT` dial.
- The input and capture pipeline is one app-thread pipeline; a capture is a
  no-op input. A request registered at F is served by the frame at F+1; one
  frame per frame; multiple requests share it. `InputQueue`/`CaptureState` and
  their mutex/condvar are removed. `sdl_info` reads the app state directly.
- The library tools' `timeout_ms` and `sdl_info.limits.frameTimeoutMs` are
  removed; the Handler's `timeout_ms` is the forward budget. The browser
  `deferToolCall`/`pumpPeer` path is removed.
- `--target` and `--web-root` are removed without aliases; the `1.0.0` break is
  documented.
- The library is single-threaded on desktop and browser. `SDL3MCP_AppIterate`
  pumps the stream, processes all available lines, and completes deferred calls;
  there is no server thread. `sdl_capture` and `sdl_input` both respond at F+1;
  `sdl_info`/`sdl_terminate` answer inline.
- The transport enum is removed. `Runtime` holds an optional `Session`; an empty
  `connect_host` means no MCP. `SDL3MCP_Transport` and
  `SDL3MCP_Configuration.transport` are removed; the platform selects the
  `SocketStream` implementation (native or Emscripten). `STDIO`, bare
  `--mcp`, `StdioIsolation`, and stdio fd ownership are removed; `--mcp=off` and
  `--mcp=HOST:PORT` are the only values. The library never listens. The Handler
  keeps client-facing stdio.
- A browser WebSocket close or error sets the same quit flag as
  `sdl_terminate`; the next `AppIterate` pushes one `SDL_EVENT_QUIT`. The app's
  own quit handling runs; `AppShutdown` closes the stream and the page.
- File names are CamelCase (`SocketStream.h/cpp`, `WebSocketStream.cpp`, ...);
  a `STYLE.md` guide is a deliverable. There is one `SocketStream` class with a
  native `SocketStream.cpp` and an Emscripten `WebSocketStream.cpp`; `StdioStream`
  is separate. The test matrix adds an optional Node.js/Emscripten doctest suite
  for the WebSocket class.
- The CMake public targets are `SDL3MCP::SDL3MCP` (library),
  `SDL3MCP::SERVER` (Handler), and `SDL3MCP::COMMON` (the SDL-free core both
  use). The shared build exports only the C API; tests link the object libraries.
- The `Stream` mirrors the Emscripten WebSocket API: callbacks plus
  `sendText`/`sendBinary`/`close`, with a desktop `pump`; `Session` implements
  the stream handler and the completion delegate.
- The Handler is a single-threaded Linux `poll` loop (`signalfd`/`pidfd`); no
  acceptor, monitor, or `sigwait` threads. Writes are bounded and never block.
- Exact v0 compatibility otherwise: same JSON schemas (except the library's
  removed `timeout_ms`/`frameTimeoutMs` fields and the removed transport
  enum/fields), same result shapes, same documented error strings (except the
  universal no-app error). The threading and fd rules are superseded by the
  single-threaded/no-stdio bullets above.

### 12.2 Acceptance seam

The rewrite's definition of done is three manual Catalyst runs, each exercising
every MCP tool the topology advertises:

1. `Handler (no --connect) <- App` (Linux).
2. `Handler (no --connect) <- Handler (--connect) <- App` (Linux).
3. `Handler (no --connect) <- Handler (--connect) <- websocat <- webapp`
   (Emscripten, via the `--web` app source).

Run 3's left side is the `--web` app source: the Handler launches the page
server, `websocat`, and Firefox itself, and kills the stack when Firefox exits.

### 12.3 Scope

- Windows and macOS are out of scope.
- The Handler is Linux-only (`poll`/`signalfd`/`pidfd`); the library is Linux
  desktop + Emscripten.
- No new MCP tools or product features.
- No performance work.
- No third-party dependency changes without a named blocker.
- v0's no-orphan supervisor rule; spawn-and-forget means each app owns its
  cleanup.
- Catalyst itself is not rewritten; it is the acceptance app.

### 12.4 Migration

Big-bang rewrite on a branch. The old tree is deleted only after the three
acceptance runs pass. The final artifact is a build-ready spec; the Manager
implements it.

Branch `feature/sdl3mcp-rewrite` from `master`; tag `v0.1.0`; Catalyst pin bump
on `feature/sdl3mcp-rewrite-pin` from `develop`. Green = `ctest`
debug/shared/asan + `readme_check` + optional Node WebSocket suite + the three
acceptance runs. Rollback is the tag/base commit and the old pin.

---

## 13. Open contract questions

These are contradictions, stale passages, and resolutions the rewrite must
carry forward. Items marked **Resolved** are settled; the rest are open and map
to a ticket or a Handler/architecture decision.

### 13.1 README enum omits `WEBSOCKET`

The README's "Configuration" code block (lines 590-594) shows only `NONE`,
`STDIO`, `CONNECT`. The header and `AGENTS.md` include
`SDL3MCP_TRANSPORT_WEBSOCKET` as value 3. The header is authoritative; the
README block is stale.

### 13.2 Raw header examples omit `frame`

README lines 1566 and 2053 show the raw header without `"frame"`. The actual
`Responder` includes it (`src/responder.cpp:76-79`), `AGENTS.md` says it travels
in the header, and the `sdl_input` example includes it. The contract requires
`frame` in every raw header. The two examples are stale.

### 13.3 Example session shows `sdl_input` as text-only

README lines 2120-2126 show `sdl_input` returning a content block with text only.
The `sdl_input` section (lines 1970-1977) and the implementation return raw
framing plus a frame. The example session is stale; the raw framing is the
contract.

### 13.4 macOS claim versus rewrite scope

README "Platforms" says Linux and macOS are supported. The rewrite scope excludes
macOS. This is a deliberate reduction; the spec must say so and the README must
be corrected. The library's POSIX code may still compile on macOS, but macOS is
not an acceptance target.

### 13.5 v0/rewrite terminology inversion

v0 calls `--listen` "relay mode" and `--connect --targets` "spawner mode". The
rewrite has one Server shape: `--connect` makes it a Relay, and `--targets`/
`--web` are app sources. The glossary and README rewrite must map v0 relay →
rewrite Server and v0 spawner → rewrite Relay + app source, or readers will
invert the topology.

### 13.6 Handler binary and tool names

Resolved: the executable stays `sdl3mcp` and the project version becomes
`1.0.0`. The stderr prefixes (`sdl3mcp: listening on ...`,
`sdl3mcp: cannot refresh ...`) stay unless a later decision changes them.

### 13.7 Relay + server composition

Resolved by ticket 02: every Handler is a Server; `--connect` adds the upstream
link. `--listen` is the downstream app listener in every Handler; `--targets`
and `--web` are app sources. One app session at a time; the base list is
`wait_session` + four app tools + `app_spawn_*`; the no-app error is
`no application session is attached; call wait_session first`. `wait_session`
does not chain.

### 13.8 Web launcher entry point

Resolved by ticket 03: `--web=dict.json` maps alias → web root, entry always
`index.html`; the Handler owns the three web processes; `--dry-run=ALIAS`
replaces `--print-plan` and covers targets too.

### 13.9 Protocol revision and server version

v0 answers `2025-11-25` and reports `serverInfo.version` `0.1.0`. The rewrite
bumps the project version to `1.0.0`; decide whether `serverInfo.version`
reports `1.0.0` and whether the protocol revision stays `2025-11-25`.

### 13.10 `log_level`

`log_level` is accepted and validated (-1..4) but v0 has no leveled logger; only
usage and errors go to stderr. The rewrite may implement logging, but that changes
observable stderr. Decide whether to preserve the v0 behaviour or make logging a
feature (which would be out of scope for exact compatibility).

### 13.11 Which error strings are contract

The README quotes some messages; the source has more (for example
`cannot write <method> to the application`,
`the application closed its connection during <method>`,
`the application returned an invalid <name> result`). The universal no-app
error is settled by ticket 02:
`no application session is attached; call wait_session first`. The rewrite must
decide which other strings clients may match on and freeze those; the rest can
be reworded. Recommendation: freeze the messages the README quotes and the
tool-result texts that name a frame or a topology state; treat internal
transport diagnostics as non-contract.

### 13.12 Two `sdl_capture` definitions

The library advertises a raw-pixel `sdl_capture`; the Handler advertises a
PNG-presenting `sdl_capture` with the same name. The contract distinguishes them
by endpoint: a direct library client sees the raw schema, a Handler client sees
the PNG schema. The rewrite must keep the Handler's definition stable and must
not let a chained Handler re-encode an already-presented result.

### 13.13 `kFirstServableFrame`

The README deliberately does not restate the value; the source sets
`kFirstServableFrame = 1` (`src/capture.hpp:51`). This is observable: a capture
requested before the first completed frame waits for the next servable frame.
The rewrite must preserve the wait and the "first rendered frame is not served"
rule. Whether the constant stays 1 is a behaviour decision; changing it changes
when an early capture is answered.

### 13.14 `sdl3mcp_app` and targets generation

The CMake function and the targets-file format are part of the spawn/relay
contract. The rewrite must keep `sdl3mcp_app`, the `[a-z0-9_]+` alias rule, the
reserved `app_spawn_` prefix, the `${CMAKE_BINARY_DIR}/sdl3mcp-targets.json`
location, and the deferred write, unless a ticket decides otherwise. `--web`
uses the same alias rules; its dict is a hand-written or generated alias → root
map, and no web variant of `sdl3mcp_app` is required unless a later ticket adds
one.

### 13.15 Client configuration examples

`opencode.json` at the repository root points a local client at
`build/deps/SDL3MCP/sdl3mcp --listen=127.0.0.1:7777`. The README also shows
`--target` and `--listen` examples. The rewrite must update these examples when
`--target` is removed; they are documentation, not runtime contract, but they
are how users discover the modes.

---

## 14. Source map

| Contract area | Primary source |
| --- | --- |
| C API declarations and types | `include/SDL3MCP/SDL3MCP.h`; README "Public API" |
| Lifecycle, threading, fd ownership | README "Threading and lifetime", "File descriptors", "Security and process model"; `AGENTS.md` "fd ownership", "Threading" |
| JSON-RPC and session | `src/json_rpc.cpp`, `src/session.cpp`; README "MCP surface" |
| Tool schemas | `src/tools.cpp`; README "MCP surface" |
| Raw framing and presentation | `src/responder.cpp`; README "Capture through the supervisor"; `AGENTS.md` "Capture framing" |
| Frame validation and crop | `src/frame.cpp`, `src/frame.hpp`; README "Frame pipeline" |
| Capture state and first servable frame | `src/capture.cpp`, `src/capture.hpp`; README "Frame pipeline" |
| Input parsing and replay | `src/input*.cpp`, `src/keymap.cpp`, `src/replay.cpp`, `src/mouse.cpp`, `src/app_input.cpp`, `src/queue.cpp`; README "Input model" |
| Configuration and app CLI | `src/configuration.cpp`; README "Configuration", "Command line" |
| Server thread and transports | `src/server.cpp`, `src/transport.cpp`, `src/stdio_isolation.cpp`, `src/socket_connect.cpp`; README "How it works", "File descriptors" |
| Browser path | `src/websocket_peer.cpp`; `AGENTS.md` "The browser session is one thread" |
| Handler modes and forwarding | `supervisor/supervisor.cpp`, `supervisor/peer.hpp`, `supervisor/cli.cpp`; README "Supervisor", "Relay", "Spawner" |
| Targets file and CMake helper | `supervisor/targets.cpp`, `CMakeLists.txt`; README "Targets sources", "Generating the targets file" |
| Web launcher | `supervisor/web_app.cpp`, `supervisor/web_runner.cpp`, `supervisor/main.cpp`; README "Web spawner" |
| Build and dependencies | `CMakeLists.txt`, `CMakePresets.json`; README "Build", "Requirements", "Platforms" |
| Tests | `tests/`; README "Development" |
| Version | `CMakeLists.txt`, `include/SDL3MCP/SDL3MCP_version.h.in`; `src/configuration.cpp` |

---

*End of contract. This document is the oracle for the rewrite; update it only
when a ticket resolves a contract question, and record the change in the map's
Decisions so far.*
