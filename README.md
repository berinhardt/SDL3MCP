# SDL3MCP

An MCP server you link into any SDL3 application.

SDL3MCP gives an agent two things every SDL3 app already has: **input** and a
**rendered frame**. Link the library, wire six calls into your existing main
callbacks, and the app speaks the Model Context Protocol over stdio. The agent
can press keys, type text, move and click the mouse, scroll, inspect the window,
and ask for a PNG of the current frame.

No renderer hooks, no window injection, no OS-level automation. Your app keeps
owning its event loop, window, and renderer; SDL3MCP only feeds synthetic SDL
events into the queue and hands the pixels you give it back to the agent.

> **Status:** v0 API draft. This document is the contract the implementation is
> being built against; the public header is `include/SDL3MCP/SDL3MCP.h`.

## Contents

- [What the agent can do](#what-the-agent-can-do)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Build](#build)
- [Quick start](#quick-start)
- [Running it](#running-it)
- [Configuration](#configuration)
- [Command line](#command-line)
- [Public API](#public-api)
- [Frame pipeline](#frame-pipeline)
- [Input model](#input-model)
- [MCP surface](#mcp-surface)
- [Threading and lifetime](#threading-and-lifetime)
- [Security and process model](#security-and-process-model)
- [Troubleshooting](#troubleshooting)
- [Limits and roadmap](#limits-and-roadmap)
- [Development](#development)
- [License](#license)

## What the agent can do

| Tool | Purpose |
| --- | --- |
| `sdl_info` | Window title, size in points and pixels, pixel density, video driver, SDL version, held input, capture statistics. |
| `sdl_input` | Send a batch of key, text, mouse-move, mouse-button, scroll, or release-all actions. |
| `sdl_capture` | Ask the app for a fresh frame and receive it as an `image/png` content block. |

Typical agent loop:

1. `sdl_info` to see what it is looking at.
2. `sdl_capture` for a baseline screenshot.
3. `sdl_input` to click, type, or hold a key.
4. `sdl_capture` again to see what changed.

## How it works

```
┌────────────────────────────── app process ───────────────────────────────┐
│                                                                          │
│   app thread (SDL)                       server thread                    │
│   ┌─────────────────────┐                ┌────────────────────────────┐   │
│   │ SDL_AppInit         │                │ stdio transport            │   │
│   │ SDL_AppIterate ─────┼─ geometry ───▶ │  JSON-RPC 2.0, one line    │   │
│   │  ├ apply input      │◀─ input batch ─┤  per message               │   │
│   │  ├ WriteFrame ──────┼─ RGBA copy ───▶│  sdl_input  → input queue  │   │
│   │ SDL_AppQuit         │◀─ frame req ───┤  sdl_capture → PNG + b64   │   │
│   └─────────────────────┘                └────────────────────────────┘   │
│             │                                            │                │
│     SDL event queue, renderer                     client's pipes          │
└──────────────────────────────────────────────────────────────────────────┘
```

- `SDL3MCP_AppInit` parses `--mcp*`, optionally starts one server thread, and
  returns. Nothing else changes until the first `SDL3MCP_AppIterate`.
- The server thread owns the transport and JSON. It never calls SDL.
- `SDL3MCP_AppIterate` is the meeting point on the app thread: it applies queued
  input to SDL's event queue and publishes the window geometry.
- `SDL3MCP_FrameRequested` / `SDL3MCP_WriteFrame` are a pull-based capture
  protocol. The app owns the renderer, so the library asks for pixels and the
  app answers; renderer type does not matter.
- `SDL3MCP_AppShutdown` joins the server thread and releases whatever input the
  library still holds.

A workflow is a single process lifetime: one `SDL3MCP_AppInit`, many
`SDL3MCP_AppIterate` calls, one `SDL3MCP_AppShutdown`.

### What it is not

- Not an OS-level input injector. It posts SDL events into your process, so it
  only affects apps that link it (and only the event-driven parts of them).
- Not a remote desktop. There is no network transport; the MCP client must be
  able to spawn the app and own its stdin/stdout.
- Not a screenshot API for other processes, and it cannot capture a window it
  was not given.
- It does not create, resize, or move windows, and it has no audio, gamepad, or
  touch tools yet.

## Requirements

- SDL 3.2 or newer.
- A C11 compiler for the public header; the implementation is C++20.
- CMake 3.21 or newer.
- `stb_image_write.h` and `nlohmann/json.hpp`. The build looks for system
  copies first and falls back to the vendored ones in `third_party/`.

No other runtime dependencies. The public header includes only `<SDL3/SDL.h>`
and the C standard headers.

## Build

```sh
cmake -S . -B build -DSDL3MCP_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build
```

Targets and options:

| Option | Default | Meaning |
| --- | --- | --- |
| `SDL3MCP_BUILD_SHARED` | `OFF` | Build a shared library instead of a static one. |
| `SDL3MCP_BUILD_EXAMPLES` | `OFF` | Build `examples/minimal`. |
| `SDL3MCP_BUILD_TESTS` | `ON` if top-level | Build the unit tests. |
| `SDL3MCP_USE_SYSTEM_STB` | `ON` | Try `find_path(STB_INCLUDE_DIR stb_image_write.h)` before vendoring. |
| `SDL3MCP_USE_SYSTEM_JSON` | `ON` | Try `find_package(nlohmann_json CONFIG QUIET)` before vendoring. |

`STB_IMAGE_WRITE_IMPLEMENTATION` is compiled exactly once, in
`src/stb_image_write_impl.cpp`, whether stb comes from the system or the
vendored copy.

### Embedding

```cmake
find_package(SDL3 CONFIG REQUIRED)

add_subdirectory(third_party/SDL3MCP)
target_link_libraries(my_game PRIVATE SDL3::SDL3 SDL3MCP::SDL3MCP)
```

With `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  SDL3MCP
  GIT_REPOSITORY https://example.invalid/SDL3MCP.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(SDL3MCP)

target_link_libraries(my_game PRIVATE SDL3MCP::SDL3MCP)
```

Installed builds export `SDL3MCP::SDL3MCP` and install the header as
`<SDL3MCP/SDL3MCP.h>`.

## Quick start

This is a complete, minimal app using SDL3's main callbacks:

```c
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3MCP/SDL3MCP.h>

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
} App;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    SDL3MCP_Configuration cfg = SDL3MCP_DefaultConfiguration();
    SDL_AppResult result = SDL3MCP_AppInit(&cfg, argc, argv);
    if (result != SDL_APP_CONTINUE) {
        return result; /* --mcp=off, --mcp-help, or a configuration error */
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return SDL_APP_FAILURE;
    }

    App *app = SDL_calloc(1, sizeof *app);
    if (!app) {
        return SDL_APP_FAILURE;
    }
    *appstate = app;

    if (!SDL_CreateWindowAndRenderer("SDL3MCP demo", 960, 540, 0,
                                     &app->window, &app->renderer)) {
        return SDL_APP_FAILURE;
    }

    return SDL3MCP_SetWindow(app->window);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    App *app = appstate;

    /* Apply agent input before polling, so this frame reacts to it. */
    SDL_AppResult result = SDL3MCP_AppIterate();
    if (result != SDL_APP_CONTINUE) {
        return result;
    }

    /* Draw something. */
    SDL_SetRenderDrawColor(app->renderer, 24, 24, 32, 255);
    SDL_RenderClear(app->renderer);
    SDL_SetRenderDrawColor(app->renderer, 240, 200, 80, 255);
    SDL_RenderDebugText(app->renderer, 24, 24, "link SDL3MCP, press keys, capture");
    /* ... */

    /* Answer a pending capture before presenting. */
    if (SDL3MCP_FrameRequested()) {
        SDL_Surface *frame = SDL_RenderReadPixels(app->renderer, NULL);
        if (frame) {
            SDL_Surface *rgba = frame;
            if (frame->format != SDL_PIXELFORMAT_RGBA8888) {
                rgba = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGBA8888);
                SDL_DestroySurface(frame);
            }
            if (rgba) {
                SDL3MCP_WriteFrame(rgba->pixels, (size_t)rgba->pitch * (size_t)rgba->h);
                SDL_DestroySurface(rgba);
            }
        }
    }

    SDL_RenderPresent(app->renderer);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    (void)appstate;
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE) {
        return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    App *app = appstate;
    (void)result;

    SDL3MCP_AppShutdown();
    if (app) {
        SDL_DestroyRenderer(app->renderer);
        SDL_DestroyWindow(app->window);
        SDL_free(app);
    }
    SDL_Quit();
}
```

Every synthetic event arrives through `SDL_AppEvent` like a real one, so an app
that already handles input needs no extra wiring.

### Manual loop

The same six calls work without main callbacks:

```c
int main(int argc, char **argv)
{
    SDL3MCP_Configuration cfg = SDL3MCP_DefaultConfiguration();
    if (SDL3MCP_AppInit(&cfg, argc, argv) != SDL_APP_CONTINUE) {
        return 1;
    }

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *window = SDL_CreateWindow("demo", 960, 540, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    SDL3MCP_SetWindow(window);

    bool running = true;
    while (running) {
        if (SDL3MCP_AppIterate() != SDL_APP_CONTINUE) {
            break;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        /* draw ... */
        if (SDL3MCP_FrameRequested()) {
            /* read back RGBA8888 and call SDL3MCP_WriteFrame */
        }
        SDL_RenderPresent(renderer);
    }

    SDL3MCP_AppShutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
```

## Running it

The default is **no MCP server**. Nothing starts unless the configuration
enables stdio or the app is launched with `--mcp`:

```sh
./demo --mcp
```

MCP over stdio means the client spawns the app and talks over its pipes. A
client configuration looks like:

```json
{
  "mcpServers": {
    "demo": {
      "command": "/path/to/demo",
      "args": ["--mcp", "--mcp-log-level=warn"]
    }
  }
}
```

Running `./demo --mcp` in a terminal is valid but silent: the terminal is not an
MCP client, and the server simply waits for a request on stdin.

## Configuration

```c
typedef enum SDL3MCP_Transport {
    SDL3MCP_TRANSPORT_NONE = 0,
    SDL3MCP_TRANSPORT_STDIO
} SDL3MCP_Transport;

typedef struct SDL3MCP_Configuration {
    SDL3MCP_Transport transport;
    uint32_t frame_timeout_ms;
    uint32_t max_frame_bytes;
    bool warp_cursor;
    int log_level;
} SDL3MCP_Configuration;
```

`SDL3MCP_DefaultConfiguration()` is a `static inline` function in the public
header, so it costs no symbol and works from both C and C++:

```c
SDL3MCP_Configuration cfg = SDL3MCP_DefaultConfiguration();
cfg.transport = SDL3MCP_TRANSPORT_STDIO; /* start without --mcp */
cfg.frame_timeout_ms = 3000;
cfg.warp_cursor = false;

SDL_AppResult result = SDL3MCP_AppInit(&cfg, argc, argv);
```

`SDL3MCP_AppInit(NULL, argc, argv)` means "pure defaults plus whatever the
command line says".

| Field | Default | Meaning |
| --- | --- | --- |
| `transport` | `SDL3MCP_TRANSPORT_NONE` | `NONE` starts no server; `STDIO` starts the stdio MCP server. |
| `frame_timeout_ms` | `2000` | Default `sdl_capture` timeout. |
| `max_frame_bytes` | `32 MiB` | Upper bound on a raw RGBA frame accepted by `SDL3MCP_WriteFrame`. 4K (`3840×2160×4`) fits; 5K and above need a larger value. |
| `warp_cursor` | `true` | Move the OS cursor with `SDL_WarpMouseInWindow` so `SDL_GetMouseState` agrees with injected motion. |
| `log_level` | `-1` | `-1` default (warn), `0` off, `1` error, `2` warn, `3` info, `4` debug. Logs go to stderr. |

The structure contains no pointers, is copied by `SDL3MCP_AppInit`, and is never
retained. Anything set in the structure can be overridden from the command line;
the command line never changes fields it does not mention.

## Command line

`SDL3MCP_AppInit` reads only arguments beginning with `--mcp`. Everything else
is left untouched for the app.

| Argument | Result |
| --- | --- |
| *(none)* | Use `cfg.transport`, which defaults to `NONE`. |
| `--mcp` | Same as `--mcp=stdio`. |
| `--mcp=stdio` | Start the stdio server. |
| `--mcp=off` | Start nothing. Useful to override a configuration that enabled stdio. |

Transport values are case-insensitive. Any other value prints usage to stderr
and makes `SDL3MCP_AppInit` return `SDL_APP_FAILURE`.

Tuning flags, all optional; flags that take a value use `=`:

| Flag | Effect |
| --- | --- |
| `--mcp-timeout-ms=N` | Override `frame_timeout_ms` (1–60000). |
| `--mcp-max-frame-bytes=N` | Override `max_frame_bytes` (1 MiB–1 GiB). |
| `--mcp-no-warp-cursor` | Set `warp_cursor = false`. |
| `--mcp-log-level=off\|error\|warn\|info\|debug` | Override `log_level`. |
| `--mcp-help` | Print usage to stderr, return `SDL_APP_SUCCESS`. |
| `--mcp-version` | Print the library version to stderr, return `SDL_APP_SUCCESS`. |

Unknown `--mcp*` arguments are errors rather than being ignored, so a typo in a
launcher fails loudly. Usage and version output always goes to **stderr**,
because stdout may already be the transport.

Propagate any non-`SDL_APP_CONTINUE` result out of `SDL_AppInit`; `--mcp-help`
and `--mcp-version` use `SDL_APP_SUCCESS` as a clean way to exit before a window
is opened.

## Public API

The complete public surface:

```c
SDL3MCP_Configuration SDL3MCP_DefaultConfiguration(void); /* header-inline */
SDL_AppResult SDL3MCP_AppInit(SDL3MCP_Configuration *config, int argc, char **argv);
SDL_AppResult SDL3MCP_SetWindow(SDL_Window *window);
SDL_AppResult SDL3MCP_AppIterate(void);
void          SDL3MCP_AppShutdown(void);
bool          SDL3MCP_FrameRequested(void);
void          SDL3MCP_WriteFrame(const uint8_t *data, size_t size);
```

All functions use C linkage. They never throw; the C++ implementation catches
everything at the boundary, logs it, and reports `SDL_APP_FAILURE` where the
signature allows it. On failure, `SDL_GetError()` explains what happened.

### `SDL3MCP_AppInit`

```c
SDL_AppResult SDL3MCP_AppInit(SDL3MCP_Configuration *config, int argc, char **argv);
```

Call once, from the app thread, before any other SDL3MCP function.

- `config == NULL` means defaults.
- `argc`/`argv` are read but never modified; pass the values SDL gave you.
- Starts the server thread when the resulting transport is `STDIO`; otherwise
  records the configuration and does nothing else.
- Returns `SDL_APP_CONTINUE` on success, `SDL_APP_SUCCESS` for
  `--mcp-help`/`--mcp-version`, and `SDL_APP_FAILURE` for invalid arguments, a
  broken stdin/stdout, or a thread that cannot start.
- On failure, no state is left behind; calling `SDL3MCP_AppShutdown` afterwards
  is still safe.
- Calling it a second time in the same process fails. One lifecycle per process.

### `SDL3MCP_SetWindow`

```c
SDL_AppResult SDL3MCP_SetWindow(SDL_Window *window);
```

Binds the window that agents interact with. Call it after creating the window,
and again if you replace the window. Pass `NULL` to clear the binding.

The window is used for three things:

- **geometry** — `SDL_GetWindowSize` (points) for mouse coordinates and
  `SDL_GetWindowSizeInPixels` for frame validation, published to the server
  thread once per `SDL3MCP_AppIterate`;
- **event routing** — synthetic events carry this window's `windowID`;
- **cursor and relative mode** — `SDL_WarpMouseInWindow` and
  `SDL_GetWindowRelativeMouseMode`.

App thread only. Returns `SDL_APP_FAILURE` if `SDL3MCP_AppInit` was not called
or the window has no drawable area yet; a window that is later minimized is fine
and only makes captures fail until it is restored.

### `SDL3MCP_AppIterate`

```c
SDL_AppResult SDL3MCP_AppIterate(void);
```

Call once per frame, ideally at the top, before `SDL_PollEvent`.

- Applies every queued input batch to SDL's event queue, in the order the tool
  calls arrived. This is the only place input enters the app.
- Publishes the current window geometry for the server thread.
- Releases held keys and buttons if the client disconnected since the last
  iteration.
- Never blocks and never waits for the network or for a client.
- Returns `SDL_APP_CONTINUE` normally and `SDL_APP_FAILURE` only when the server
  thread has died from a fatal transport error. The app should return that
  failure from its own iterate callback.

With `transport == NONE` it is a no-op that returns `SDL_APP_CONTINUE`.

### `SDL3MCP_AppShutdown`

```c
void SDL3MCP_AppShutdown(void);
```

Stops the server thread, closes the transport, releases any keys or buttons the
library still holds down, and frees the frame buffer. Call it from
`SDL_AppQuit`.

- Idempotent, and safe after a failed `SDL3MCP_AppInit`.
- Bounded: the stdin reader wakes at least every 50 ms to check the stop flag,
  so shutdown does not hang on an idle client.
- App thread only.
- There is no restart: the stdio session is one-shot, so expect one lifecycle
  per process. A second `SDL3MCP_AppInit` fails.

### `SDL3MCP_FrameRequested`

```c
bool SDL3MCP_FrameRequested(void);
```

Returns `true` while an `sdl_capture` call is waiting for a frame. Cheap enough
to call every frame: it is an atomic load, no locks.

- Call it after drawing and before presenting.
- If it is `true`, read back the pixels and call `SDL3MCP_WriteFrame` once.
- It stays `true` until the frame is accepted or the capture times out, so a
  missed frame is retried on the next iteration.
- Always `false` when no server is running.

### `SDL3MCP_WriteFrame`

```c
void SDL3MCP_WriteFrame(const uint8_t *data, size_t size);
```

Hands a rendered frame to a waiting capture.

- **Format:** tightly packed RGBA8888, top-left origin, row-major, stride exactly
  `width * 4`, bytes in the order R, G, B, A. On SDL 3.2+, `SDL_RenderReadPixels`
  returns a surface; convert it with `SDL_ConvertSurface(surface,
  SDL_PIXELFORMAT_RGBA8888)` and pass `surface->pixels`.
- **Size:** `width * height * 4`, where `width` and `height` are the bound
  window's `SDL_GetWindowSizeInPixels` at the moment of the call. A mismatch is
  dropped and logged; the capture eventually times out.
- **Guard:** `size <= max_frame_bytes`; larger frames are dropped.
- The pixels are copied before the function returns, so the app can reuse or
  free its buffer immediately. PNG encoding and base64 happen on the server
  thread, never on the render thread.
- Call it only when `SDL3MCP_FrameRequested()` is true. Calls with no pending
  capture are ignored.
- App thread only, because it queries the window.

The function returns nothing on purpose: a capture is a request that can fail,
and the error belongs to the client's `sdl_capture` call, not to the render
loop. Library-side failures are logged at warn level.

## Frame pipeline

Capture is pull-based because the library has no idea how the app draws. It
cannot read a swapchain from another thread, and it does not want to know
whether the app uses SDL_Renderer, OpenGL, Vulkan, Metal, or software. So it
asks:

1. The client calls `sdl_capture`.
2. The server marks a request pending and waits up to `frame_timeout_ms`.
3. On the next frame, the app's `SDL3MCP_FrameRequested()` returns true.
4. The app renders, reads its own pixels, and calls `SDL3MCP_WriteFrame`.
5. The library copies the buffer, clears the pending request, and wakes the
   server thread.
6. The server thread optionally downscales, encodes PNG, base64-encodes it, and
   answers the tool call.
7. The app continues its loop; nothing blocked.

Rules that fall out of this design:

- The returned frame is one written **after** the request arrived; stale frames
  are never served.
- Only one capture is pending at a time. Tool calls are serialized on the server
  thread, so a slow capture delays the next tool call until it completes or
  times out. Agents that need to act "between" two frames should call
  `sdl_input`, then `sdl_capture`.
- `sdl_capture` fails with a clear error if no window is bound, the window has
  zero drawable pixels, the frame exceeds `max_frame_bytes`, or the app never
  answers in time.
- PNG encoding is the expensive part (tens of milliseconds at 1080p) and happens
  on the server thread while the app keeps rendering.
- The optional `max_width` parameter box-filters the image down before encoding.
  This is the main lever for payload size; 1280 or 1600 wide is usually enough
  for an agent to read UI text.
- Base64 adds about 33% to the PNG, and the whole image travels as one JSON
  line. A 1280×720 PNG is typically 50–150 KB, or ~70–200 KB on the wire.

## Input model

`sdl_input` takes an ordered array of actions. The batch is validated as a
whole, then queued as one unit and applied in a single `SDL3MCP_AppIterate`, so
no frame boundary can appear in the middle of a gesture.

| Action | Fields |
| --- | --- |
| `key` | `key` (name or chord), `action` = `down`/`up`/`press` (default `press`), `modifiers`, `repeat`. |
| `text` | `text` (UTF-8). |
| `mouse_move` | `x`,`y` (window points) **or** `dx`,`dy` (relative). |
| `mouse_button` | `button` = `left`/`middle`/`right`/`x1`/`x2`, `action` = `down`/`up`/`click`, optional `x`,`y`, `clicks`. |
| `scroll` | `x`, `y` in wheel units (120 per notch). |
| `release_all` | Releases every key and button the library holds. |

### Keys

- Names are matched case-insensitively against SDL's scancode names, with `-`
  and `_` treated as spaces. `a`, `space`, `return`, `escape`, `left`, `f1`,
  `left ctrl`, `keypad plus` all work.
- Single printable characters are accepted.
- Chords are written with `+`: `ctrl+shift+s`, `alt+f4`. An optional `modifiers`
  array is merged into the chord.
- `press` is `down` immediately followed by `up`; `repeat` repeats the pair.
- A key sent with `down` stays down across frames until an `up`, a
  `release_all`, the client disconnects, or `SDL3MCP_AppShutdown`. That is how
  an agent holds a movement key.

### Text

`text` sends `SDL_EVENT_TEXT_INPUT`, not key events. It is meant for text fields
and consoles; games that read key codes should use `key` actions. SDL's text
event holds 32 bytes including the terminator, so the library splits longer
strings at UTF-8 boundaries and sends one event per chunk. Text requires a bound
window for its `windowID`.

### Mouse

- Absolute coordinates are in **window points**, the same coordinate space as
  `SDL_EVENT_MOUSE_MOTION.x/y`, and are clamped to the window.
- The library tracks a virtual cursor. `xrel`/`yrel` are derived from the move,
  except when `warp_cursor` is on and the platform's warp event already carries
  the delta; in that case the synthetic event leaves its delta at zero so delta
  accumulators do not count the move twice.
- With `warp_cursor = true` (the default), the OS cursor is also warped with
  `SDL_WarpMouseInWindow`, which keeps `SDL_GetMouseState` and hit testing
  consistent. Warping needs window focus and is best effort.
- In relative mouse mode, warping is skipped and absolute moves are converted to
  deltas from the virtual cursor. `dx`/`dy` are passed through as `xrel`/`yrel`.
- `click` is a full down/up pair with the requested `clicks` count.
- Buttons and wheel use `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` and
  `SDL_EVENT_MOUSE_WHEEL` on the bound window.

### Modifiers and state

The library maintains a virtual modifier mask, sets it on every key event, and
mirrors it with `SDL_SetModState`, so `SDL_GetModState` agrees with the synthetic
input. Releasing a modifier or using `release_all` restores the mask.

### The keyboard-state caveat

Injected input is **event-based**. SDL maintains `SDL_GetKeyboardState` from its
own backend key handlers, not from events pushed onto the queue, and SDL3 has no
public setter for per-key state. Consequences:

- Apps that handle `SDL_EVENT_KEY_DOWN`/`SDL_EVENT_KEY_UP` see everything.
- Apps that only poll `SDL_GetKeyboardState` for movement will **not** move in
  response to synthetic keys.
- The mouse is different: `warp_cursor` makes `SDL_GetMouseState` follow
  injected motion, so polling mouse code mostly works.

If your app is a keyboard-state poller, handle the key events as well (or drive
movement from your own event handler) to make it agent-controllable. There is no
portable way for this library to update SDL's private key array.

## MCP surface

SDL3MCP speaks MCP over the standard stdio transport: UTF-8 JSON-RPC 2.0, one
message per line, no batches. The server targets protocol revision
`2025-11-25` (the handshake-based revision) and negotiates the version the
client sends.

- Advertised capability: `tools` only. No resources, prompts, sampling, or
  logging.
- `initialize`, `notifications/initialized`, `ping`, `tools/list`, and
  `tools/call` are supported.
- Tool calls are handled one at a time, in order.
- Inbound lines are capped at 1 MiB; malformed input closes the session.
- Every outbound message is flushed immediately.
- Tool failures use `isError: true` with a text explanation; JSON-RPC errors are
  reserved for protocol-level problems (`-32700`, `-32601`, `-32602`).

### `sdl_info`

No arguments. Returns a pretty-printed JSON text block:

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
  "input": { "heldKeys": ["right"], "heldButtons": [] },
  "capture": { "pending": false, "count": 3, "lastWidth": 1920,
               "lastHeight": 1080, "lastBytes": 84213, "lastMs": 27 },
  "limits": { "frameTimeoutMs": 2000, "maxFrameBytes": 33554432 }
}
```

When no window is bound, `window` is `null` and the tool still succeeds, which
makes it a useful first call after startup.

### `sdl_input`

```json
{
  "name": "sdl_input",
  "description": "Queue a batch of synthetic input actions. The batch is applied atomically at the next frame. Input is delivered as SDL events; follow with sdl_capture to observe the result.",
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
              "properties": {
                "type": { "const": "key" },
                "key": { "type": "string" },
                "action": { "enum": ["down", "up", "press"], "default": "press" },
                "modifiers": {
                  "type": "array",
                  "items": { "enum": ["ctrl", "shift", "alt", "gui"] }
                },
                "repeat": { "type": "integer", "minimum": 1, "maximum": 32, "default": 1 }
              },
              "required": ["type", "key"],
              "additionalProperties": false
            },
            {
              "type": "object",
              "properties": {
                "type": { "const": "text" },
                "text": { "type": "string", "minLength": 1, "maxLength": 1024 }
              },
              "required": ["type", "text"],
              "additionalProperties": false
            },
            {
              "type": "object",
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
              "properties": { "type": { "const": "release_all" } },
              "required": ["type"],
              "additionalProperties": false
            }
          ]
        }
      }
    },
    "required": ["actions"],
    "additionalProperties": false
  }
}
```

`mouse_move` needs either `x` and `y`, or `dx` and `dy`. Invalid combinations
are rejected before anything is queued, and the error names the offending
action index. The result is a text summary such as `3 actions queued`.

### `sdl_capture`

```json
{
  "name": "sdl_capture",
  "description": "Ask the application for a fresh frame and return it as PNG. Requires the app to answer SDL3MCP_FrameRequested(). Use max_width to keep the payload small.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "max_width": { "type": "integer", "minimum": 16, "maximum": 16384 },
      "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 }
    },
    "additionalProperties": false
  }
}
```

The result has two content blocks: a short text summary and the image.

```json
{
  "content": [
    { "type": "text", "text": "1280x720 PNG, 91.4 KB, 34 ms" },
    { "type": "image", "mimeType": "image/png", "data": "iVBORw0KGgoAAAANSUhEUg..." }
  ],
  "isError": false
}
```

On timeout the result is an error, not an image:

```json
{
  "content": [
    { "type": "text", "text": "no frame arrived within 2000 ms. The app must call SDL3MCP_WriteFrame() when SDL3MCP_FrameRequested() returns true, with tightly packed RGBA8888 at SDL_GetWindowSizeInPixels()." }
  ],
  "isError": true
}
```

Tool annotations: `sdl_info` and `sdl_capture` are `readOnlyHint: true`;
`sdl_input` is not read-only and is `openWorldHint: false`. `sdl_info` is also
idempotent; `sdl_input` and `sdl_capture` are not, because input changes app
state and capture consumes a frame.

### Example session

```
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"agent","version":"1.0"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"sdl3mcp","version":"0.1.0"}}}
→ {"jsonrpc":"2.0","method":"notifications/initialized"}
→ {"jsonrpc":"2.0","id":2,"method":"tools/list"}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[ ... sdl_info, sdl_input, sdl_capture ... ]}}
→ {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"sdl_input","arguments":{"actions":[{"type":"key","key":"right","action":"down"}]}}}
← {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"1 action queued"}],"isError":false}}
→ {"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"sdl_capture","arguments":{"max_width":1280}}}
← {"jsonrpc":"2.0","id":4,"result":{"content":[{"type":"text","text":"1280x720 PNG, 91.4 KB, 34 ms"},{"type":"image","mimeType":"image/png","data":"iVBORw0KGgo..."}],"isError":false}}
→ {"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"sdl_input","arguments":{"actions":[{"type":"key","key":"right","action":"up"}]}}}
← {"jsonrpc":"2.0","id":5,"result":{"content":[{"type":"text","text":"1 action queued"}],"isError":false}}
```

## Threading and lifetime

| Thread | May touch | Must not touch |
| --- | --- | --- |
| App thread | SDL, the window, the renderer, all `SDL3MCP_*` functions. | — |
| Server thread | stdin/stdout, JSON, the copied frame buffer, the input queue. | Any SDL function, including `SDL_GetWindowSize`. |

Rules that make this safe:

1. **One owner per piece of state.** Window geometry is published by the app
   thread into a snapshot; the server thread only reads it.
2. **Input travels one way.** Tool calls append immutable action batches to a
   queue; `SDL3MCP_AppIterate` drains and applies them on the app thread.
3. **Frames travel one way.** `SDL3MCP_WriteFrame` copies pixels under a mutex
   and signals a condition variable; encoding happens after the copy.
4. **No blocking on the app thread.** `SDL3MCP_AppIterate` and
   `SDL3MCP_WriteFrame` never wait for the server, the client, or a lock held
   during encoding.
5. **Shutdown joins.** `SDL3MCP_AppShutdown` sets the stop flag, waits for the
   reader (bounded by its 50 ms poll interval), joins the thread, then releases
   held input and frees frames.

The library assumes one app thread. Calling `SDL3MCP_SetWindow`,
`SDL3MCP_AppIterate`, `SDL3MCP_WriteFrame`, or `SDL3MCP_AppShutdown` from a
different thread is unsupported, because SDL video functions are not
thread-safe.

## Security and process model

stdio-only is a small attack surface by construction:

- There is **no listening socket**, no port, and no authentication. The only
  way in is to start the app with `--mcp` (or a configuration that enables it)
  and own its stdin/stdout.
- The trust boundary is the process boundary. A client that can launch the app
  can already run it; SDL3MCP adds the ability to send input and read frames,
  not to read files, spawn processes, or reach the network.
- Frames come from your app's own rendering and may contain anything the app
  displays. Treat them as you would a screenshot of that window.
- Everything runs with the app's privileges. Do not enable the server in a
  privileged or setuid context.

Two process-level rules the app must respect:

- **stdout belongs to MCP.** With `--mcp`, any `printf`, `std::cout`, or other
  stdout write corrupts the JSON-RPC stream. Use stderr or `SDL_Log` (which
  writes to stderr) for diagnostics.
- **stdin belongs to MCP.** Do not read stdin in the app while the server is
  running. If your app needs console input, use `--mcp=off`.

When the client closes the pipe, the server sees EOF, stops cleanly, and leaves
the app running. `SDL3MCP_AppIterate` keeps returning `SDL_APP_CONTINUE`; the
app's own lifecycle is unaffected.

## Troubleshooting

**The client never receives a response.** The app is writing to stdout. Move
diagnostics to stderr or `SDL_Log`. A single stray `printf` breaks the stream.

**Nothing starts.** The default is no server. Launch with `--mcp`, or set
`cfg.transport = SDL3MCP_TRANSPORT_STDIO`. Check `--mcp=off` is not in the
launcher.

**`sdl_capture` says no frame arrived.** Confirm the render loop calls
`SDL3MCP_FrameRequested()` every frame and, when it is true, calls
`SDL3MCP_WriteFrame` with exactly `width * height * 4` bytes at the window's
pixel size. Common causes: reading pixels after `SDL_RenderPresent` (do it
before), allocating for `SDL_GetWindowSize` instead of
`SDL_GetWindowSizeInPixels`, or a minimized window with zero drawable pixels.

**Frames are upside down or colors are swapped.** The buffer must be
top-left-first RGBA8888 with no row padding. `SDL_RenderReadPixels` returns a
surface; convert it with `SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888)`
and pass its pixels with pitch `width * 4`.

**Mouse clicks land in the wrong place on a HiDPI display.** Frames are in
pixels; mouse coordinates are in window points. `sdl_info` reports both, and the
ratio is `pixelDensity`. Send points.

**Keys have no effect.** The app is probably polling `SDL_GetKeyboardState`
instead of handling key events; see [the keyboard-state
caveat](#the-keyboard-state-caveat). Mouse movement usually still works because
of cursor warping.

**A key is stuck.** The library releases held keys when the client disconnects
and during `SDL3MCP_AppShutdown`. To recover mid-session, send
`{"type":"release_all"}`.

**Payloads are too large.** Pass `max_width` (1280 is a good default for UI
work), and lower `max_frame_bytes` if you want a hard ceiling. Base64 adds about
a third to the PNG size.

**The app hangs in a GUI launch.** Apps started from Finder or Explorer have no
usable stdin/stdout, so there is no MCP session. Launch from a terminal or let
the MCP client spawn the binary.

**The client disconnects but the app keeps running.** That is intended; the
server thread exits on EOF. Return `SDL_APP_SUCCESS` from your event handler if
you want the app to quit when it loses its MCP client, but do not make that the
library's decision.

## Limits and roadmap

Known limits in v0:

- Event-based input only; `SDL_GetKeyboardState` polling is not updated.
- One MCP client per process, stdio only, no TLS, no remote access.
- MCP `2026-07-28` and later stateless revisions are not implemented; clients
  must use the handshake-based revision.
- PNG only, one full frame per capture, no diffs or video.
- No window management (resize, move, title), no gamepad, touch, or audio.
- Capture requires cooperation from the app; the library cannot read a
  swapchain on its own.

Plausible next steps: `sdl_window` for resize/title/fullscreen, `sdl_quit`,
gamepad and touch actions, frame diffs, and a `sdl3mcp://frame/latest`
resource. Anything added has to keep the six-call wiring and the "library never
touches your renderer" rule intact.

## Development

```
include/SDL3MCP/SDL3MCP.h   public C header, hand-written, C11-clean
src/                        C++20 internals: config, transport, protocol,
                            input queue, frame encode
third_party/                vendored stb_image_write.h and nlohmann/json.hpp
examples/minimal/           the quick-start app
tests/                      unit tests plus a dummy-video-driver integration test
CMakeLists.txt
```

```sh
cmake -S . -B build -DSDL3MCP_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Guidelines: the C API never throws, all C++ exceptions stop at the boundary, the
public header must compile as C11 and C++20, and the server thread must never
call SDL. New tool parameters are snake_case on the wire and camelCase in C++.
Behavior changes get a regression test.

## License

zlib, the same license as SDL3. See `LICENSE`.
