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

> **Status:** v0 API. The library is implemented and this document is the
> contract it follows; the public header is `include/SDL3MCP/SDL3MCP.h`.

## Contents

- [What the agent can do](#what-the-agent-can-do)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Platforms](#platforms)
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
| `sdl_info` | Window title, size in points and pixels, pixel density, video driver, SDL version, held input, frame counters, capture statistics. |
| `sdl_input` | Send a batch of key, text, mouse-move, mouse-button, scroll, or release-all actions. |
| `sdl_capture` | Ask the app for a fresh frame, optionally cropped or downscaled, and receive it as an `image/png` content block. |

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
- `SDL3MCP_AppIterate` is the meeting point on the app thread, and it is the
  **last** SDL3MCP call of the frame. It acknowledges the previous input batch,
  applies at most one new queued batch to SDL's event queue, and publishes the
  window geometry.
- `SDL3MCP_FrameRequested` / `SDL3MCP_WriteFrame` are a pull-based capture
  protocol. The app owns the renderer, so the library asks for pixels and the
  app answers; renderer type does not matter.
- `SDL3MCP_AppShutdown` joins the server thread, releases whatever input the
  library still holds, and restores the host's stdin/stdout.

A workflow is a single process lifetime: one `SDL3MCP_AppInit`, many
`SDL3MCP_AppIterate` calls, one `SDL3MCP_AppShutdown`.

### Frame numbering: F and F+1

The library counts app frames. When an input batch is applied during
`SDL3MCP_AppIterate` of frame **F**, its events are pushed onto SDL's queue, and
the calling `sdl_input` returns during `SDL3MCP_AppIterate` of frame **F+1**.
That is the first frame in which the app has certainly consumed the events, so
the result names frame F ("3 actions applied at frame 12"). A capture follows
the same rule: the frame you receive was written after the request arrived.

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

- SDL 3.2 or newer. Both complete examples below use only SDL 3.2 APIs.
- A C11 compiler for the public header; the implementation is C++20.
- CMake 3.21 or newer.
- A pthreads-capable platform. The stdio transport, fd isolation, and server
  thread are POSIX today; Windows support is a TODO.
- `stb_image_write.h` and `nlohmann/json.hpp`. stb is vendored in
  `third_party/stb`; nlohmann is resolved from the system first and falls back
  to `third_party/nlohmann/json.hpp`.

No other runtime dependencies. The public header includes only `<SDL3/SDL.h>`
and the C standard headers.

## Platforms

- **Linux and macOS are supported** through the POSIX transport: `poll` for
  idle and shutdown wake-up, `read`/`write` on the duplicated descriptors,
  `dup`/`dup2` for fd isolation, `/dev/null` for the host-descriptor redirect,
  and `SIGPIPE` blocked on the server thread so a client that closes its stdout
  cannot kill the app.
- **Windows is a TODO, not implemented.** A Windows configure fails with a
  `FATAL_ERROR` instead of producing a library that cannot work. The missing
  pieces are:
  - binary stdio: `_setmode(_fileno(stdin), _O_BINARY)` and the same for
    stdout, because the transport frames one message per newline and must not
    let the CRT translate `\r\n`;
  - a Win32 reader wake-up: `PeekNamedPipe`/`WaitForSingleObject` in place of
    `poll`, so the read loop sees shutdown without input and keeps the existing
    idle timeout;
  - stdio isolation via `_dup`/`_dup2` and the `NUL` device in place of
    `dup`/`dup2` and `/dev/null`, plus handling `ERROR_NO_DATA`/broken-pipe
    writes instead of relying on `SIGPIPE`;
  - test guards that skip the POSIX-only scenarios on Windows, and a Windows CI
    matrix so the guard above can be removed only against a real Windows host.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSDL3MCP_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Targets and options:

| Option | Default | Meaning |
| --- | --- | --- |
| `SDL3MCP_BUILD_SHARED` | `OFF` | Build a shared library instead of a static one. |
| `SDL3MCP_BUILD_EXAMPLES` | `OFF` | Build the `minimal` executable from `examples/minimal/main.c`. |
| `SDL3MCP_BUILD_TESTS` | `ON` if top-level | Build the unit tests and register them with CTest. |
| `SDL3MCP_USE_SYSTEM_JSON` | `ON` | Try `find_package(nlohmann_json CONFIG QUIET)` before using the vendored header. |

stb is always vendored: `third_party/stb/stb_image_write.h` is a private include
directory of the library, and `STB_IMAGE_WRITE_IMPLEMENTATION` is compiled
exactly once in `src/stb_image_write_impl.cpp`. nlohmann/json is exported as the
`nlohmann_json::nlohmann_json` interface target: the system package when found,
otherwise the header in `third_party/nlohmann`.

The shared configuration uses a separate build directory:

```sh
cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Debug -DSDL3MCP_BUILD_SHARED=ON
cmake --build build-shared --parallel
ctest --test-dir build-shared --output-on-failure
```

The shared library exports only the six public C entry points while its C++
internals stay hidden, so unit tests that include the private `src/` headers
link the library's object files instead of the shared object. The sources are
compiled once in an object library that both the library and those tests
consume.

`CMakePresets.json` adds optional presets for those two configurations and for
the sanitizer runs. They live under `build/<preset>` and do not replace the
`cmake -S . -B build` flow above:

```sh
cmake --list-presets=all

cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

The `shared` preset is the same sequence with `--preset shared`. The `asan`
preset compiles with `-fsanitize=address,undefined` and runs the full suite with
leak detection on; only the two SDL dummy-video-driver integration tests
(`input_tool_tests`, `capture_tool_tests`) set `ASAN_OPTIONS=detect_leaks=0`,
because SDL keeps process-wide allocations those tests cannot free. The `tsan`
preset compiles with `-fsanitize=thread` and runs only `queue_tests` and
`server_tests`, the two thread handoffs the design depends on. Sanitizer runs
are local only; there is no CI workflow.

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

This is `examples/minimal/main.c`, a complete app using SDL3's main callbacks:

```c
/*
  The smallest complete SDL3MCP host: a window whose background an agent can
  change with synthetic key presses and then see in a capture.
*/

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3MCP/SDL3MCP.h>

typedef enum BackgroundMode
{
    BACKGROUND_DEFAULT,
    BACKGROUND_RED,
    BACKGROUND_GREEN,
    BACKGROUND_BLUE
} BackgroundMode;

typedef struct App
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    BackgroundMode background;
} App;

/* Pressing the active color key again returns to the default background. */
static BackgroundMode toggledBackground(BackgroundMode current, BackgroundMode target)
{
    return current == target ? BACKGROUND_DEFAULT : target;
}

static void setBackgroundColor(App *app)
{
    switch (app->background) {
    case BACKGROUND_RED:
        SDL_SetRenderDrawColor(app->renderer, 200, 30, 30, 255);
        break;
    case BACKGROUND_GREEN:
        SDL_SetRenderDrawColor(app->renderer, 30, 180, 60, 255);
        break;
    case BACKGROUND_BLUE:
        SDL_SetRenderDrawColor(app->renderer, 40, 80, 220, 255);
        break;
    case BACKGROUND_DEFAULT:
        SDL_SetRenderDrawColor(app->renderer, 24, 24, 32, 255);
        break;
    }
}

/*
  SDL3MCP_WriteFrame wants tightly packed RGBA8888, while a readback surface is
  free to pad its rows, so copy when the pitch does not match the row size.
  SDL_PIXELFORMAT_RGBA32 is SDL's byte-order alias for bytes R,G,B,A; the
  similarly named SDL_PIXELFORMAT_RGBA8888 is packed 32-bit order and stores
  A,B,G,R in memory on little-endian hosts.
*/
static void writeFrame(App *app)
{
    SDL_Surface *frame = SDL_RenderReadPixels(app->renderer, NULL);
    if (!frame) {
        return;
    }

    SDL_Surface *rgba = frame;
    if (frame->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(frame);
        if (!rgba) {
            return;
        }
    }

    const size_t rowBytes = (size_t)rgba->w * 4u;
    const size_t size = rowBytes * (size_t)rgba->h;
    if ((size_t)rgba->pitch == rowBytes) {
        SDL3MCP_WriteFrame(rgba->pixels, size);
    } else {
        uint8_t *packed = SDL_malloc(size);
        if (packed) {
            for (int y = 0; y < rgba->h; ++y) {
                SDL_memcpy(packed + rowBytes * (size_t)y,
                           (const uint8_t *)rgba->pixels + (size_t)rgba->pitch * (size_t)y,
                           rowBytes);
            }
            SDL3MCP_WriteFrame(packed, size);
            SDL_free(packed);
        }
    }

    SDL_DestroySurface(rgba);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    /* SDL3MCP_AppInit runs first so --mcp-help and --mcp=off can exit early. */
    SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();
    SDL_AppResult result = SDL3MCP_AppInit(&configuration, argc, argv);
    if (result != SDL_APP_CONTINUE) {
        return result;
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

    setBackgroundColor(app);
    SDL_RenderClear(app->renderer);

    if (SDL3MCP_FrameRequested()) {
        writeFrame(app);
    }

    SDL_RenderPresent(app->renderer);

    /* Last, so this frame's capture and input bookkeeping stay in step. */
    return SDL3MCP_AppIterate();
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    App *app = appstate;

    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.repeat) {
        return SDL_APP_CONTINUE;
    }

    switch (event->key.key) {
    case SDLK_ESCAPE:
        return SDL_APP_SUCCESS;
    case SDLK_R:
        app->background = toggledBackground(app->background, BACKGROUND_RED);
        break;
    case SDLK_G:
        app->background = toggledBackground(app->background, BACKGROUND_GREEN);
        break;
    case SDLK_B:
        app->background = toggledBackground(app->background, BACKGROUND_BLUE);
        break;
    default:
        break;
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

Press `r`, `g`, or `b` to switch the background to red, green, or blue, and
press the active color again to return to the default; key repeats are ignored
so holding a key does not flicker the background. Escape or the window's close
button quits.

`SDL_AppIterate` draws, answers a pending capture from the readback surface,
presents, and then calls `SDL3MCP_AppIterate` **last**. Every synthetic event
arrives through `SDL_AppEvent` like a real one, so an app that already handles
input needs no extra wiring.

### Manual loop

The same six calls work without main callbacks. `SDL3MCP_AppIterate` is still the
last call of the frame:

```c
#include <SDL3/SDL.h>
#include <SDL3MCP/SDL3MCP.h>

int main(int argc, char **argv)
{
    SDL3MCP_Configuration cfg = SDL3MCP_DefaultConfiguration();
    if (SDL3MCP_AppInit(&cfg, argc, argv) != SDL_APP_CONTINUE) {
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL3MCP_AppShutdown();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("demo", 960, 540, 0);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    if (!renderer || SDL3MCP_SetWindow(window) != SDL_APP_CONTINUE) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL3MCP_AppShutdown();
        SDL_Quit();
        return 1;
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        /* Draw this frame. */
        SDL_SetRenderDrawColor(renderer, 24, 24, 32, 255);
        SDL_RenderClear(renderer);

        if (SDL3MCP_FrameRequested()) {
            /* Read back tightly packed RGBA8888 and call SDL3MCP_WriteFrame. */
        }
        SDL_RenderPresent(renderer);

        if (SDL3MCP_AppIterate() != SDL_APP_CONTINUE) {
            break;
        }
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
enables stdio or the app is launched with `--mcp`.

`examples/CMakeLists.txt` builds the target `minimal` from
`examples/minimal/main.c` at the examples level, so with a top-level `build`
directory the binary is `<build>/examples/minimal`:

```sh
./build/examples/minimal --mcp
```

or, equivalently:

```sh
cd build && ./examples/minimal --mcp
```

MCP over stdio means the client spawns the app and talks over its pipes. A
client configuration looks like:

```json
{
  "mcpServers": {
    "demo": {
      "command": "/path/to/build/examples/minimal",
      "args": ["--mcp", "--mcp-log-level=warn"]
    }
  }
}
```

Running `./build/examples/minimal --mcp` in a terminal is valid but silent: the
terminal is not an MCP client, and the server simply waits for a request on
stdin.

### File descriptors

The transport owns real file descriptors, so the two transports differ in what
they do to the host process.

With `SDL3MCP_TRANSPORT_NONE` the library opens nothing, duplicates nothing,
reads nothing, and writes nothing: the host's stdin and stdout are untouched.

With `SDL3MCP_TRANSPORT_STDIO`, `SDL3MCP_AppInit` duplicates the original
stdin/stdout for the server and then redirects the host's descriptors:

- stdin is redirected to `/dev/null`, so a host `scanf`/`getline` sees EOF
  instead of eating the client's JSON-RPC bytes;
- stdout is redirected to stderr, so a stray `printf` cannot corrupt the
  JSON-RPC stream;
- stderr is never touched and is where diagnostics belong.

The host must not write to stdout **before** `SDL3MCP_AppInit`: at that point
stdout is still the pipe the client is reading. The originals are restored in
`SDL3MCP_AppShutdown`.

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
| `frame_timeout_ms` | `2000` | Default `sdl_capture` timeout and the time `sdl_input` waits for its F+1 acknowledgement. |
| `max_frame_bytes` | `32 MiB` | Upper bound on a raw RGBA frame accepted by `SDL3MCP_WriteFrame`. 4K (`3840×2160×4`) fits; 5K and above need a larger value. |
| `warp_cursor` | `true` | Move the OS cursor with `SDL_WarpMouseInWindow` on absolute mouse moves. |
| `log_level` | `-1` | Accepted and validated as -1..4 (`0` off, `1` error, `2` warn, `3` info, `4` debug). The library has no leveled logger yet, so today only command-line usage and errors are written to stderr. |

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
- Calling it a second time in the same process fails, even after a shutdown.
  One lifecycle per process.

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
- **text input and cursor** — `SDL_TextInputActive`, `SDL_WarpMouseInWindow`,
  and `SDL_GetWindowRelativeMouseMode`.

App thread only. Returns `SDL_APP_FAILURE` if `SDL3MCP_AppInit` was not called
or the window has no drawable area yet; a window that is later minimized is fine
and only makes captures fail until it is restored. Rebinding resets held keys,
held buttons, and the virtual cursor, so nothing crosses windows.

### `SDL3MCP_AppIterate`

```c
SDL_AppResult SDL3MCP_AppIterate(void);
```

Call once per frame, **last**: after drawing, after `SDL_RenderPresent`, and
after answering `SDL3MCP_FrameRequested`.

- Acknowledges the batch that was applied at frame F, which is why the matching
  `sdl_input` returns at frame F+1. The acknowledgement happens before a new
  batch is taken, so at most one batch is in flight.
- Takes at most one queued input batch and pushes its events onto SDL's event
  queue. This is the only place input enters the app.
- Publishes the current window geometry, held input, and frame counters for the
  server thread.
- Never blocks and never waits for the network or for a client.
- Returns `SDL_APP_CONTINUE` normally and `SDL_APP_FAILURE` when the server thread
  has died from a fatal transport error; the app should return that failure from
  its own iterate callback.

With `transport == NONE` it is a no-op that returns `SDL_APP_CONTINUE`.

### `SDL3MCP_AppShutdown`

```c
void SDL3MCP_AppShutdown(void);
```

Stops the server thread, closes the transport, restores the host's stdin/stdout,
releases any keys or buttons the library still holds down, and frees the frame
buffer. Call it from `SDL_AppQuit`.

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
- Always `false` when no server is running or no window is bound.

### `SDL3MCP_WriteFrame`

```c
void SDL3MCP_WriteFrame(const uint8_t *data, size_t size);
```

Hands a rendered frame to a waiting capture. There is no pitch or stride
parameter: the buffer must already be packed.

- **Format:** tightly packed RGBA8888, top-left origin, row-major, stride
  exactly `width * 4`, bytes in the order R, G, B, A. On SDL 3.2+,
  `SDL_RenderReadPixels` returns a surface; convert it with
  `SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32)` and pass its pixels.
  `SDL_PIXELFORMAT_RGBA32` is SDL's byte-order alias for an R, G, B, A byte
  array; the similarly named `SDL_PIXELFORMAT_RGBA8888` is integer-packed order
  and stores A, B, G, R in memory on little-endian hosts.
- **Pitch:** a readback surface may pad its rows. When `surface->pitch` is not
  `width * 4`, copy row by row into a packed buffer first; the quick-start
  `writeFrame` helper shows the conversion and copy.
- **Size:** `width * height * 4`, where `width` and `height` are the bound
  window's `SDL_GetWindowSizeInPixels` at the moment of the call.
- **Guard:** `size <= max_frame_bytes`; larger frames are dropped.
- The pixels are copied before the function returns, so the app can reuse or
  free its buffer immediately. PNG encoding and base64 happen on the server
  thread, never on the render thread.
- Call it only when `SDL3MCP_FrameRequested()` is true. Calls with no pending
  capture are ignored.
- App thread only, because it queries the window.

The function returns nothing on purpose: a capture is a request that can fail,
and the error belongs to the client's `sdl_capture` call, not to the render
loop.

## Frame pipeline

Capture is pull-based because the library has no idea how the app draws. It
cannot read a swapchain from another thread, and it does not want to know
whether the app uses SDL_Renderer, OpenGL, Vulkan, Metal, or software. So it
asks:

1. The client calls `sdl_capture`.
2. The server validates the arguments, checks that a window is bound, marks a
   request pending, and waits up to `timeout_ms`.
3. On the next frame, the app's `SDL3MCP_FrameRequested()` returns true.
4. The app renders, reads its own pixels, and calls `SDL3MCP_WriteFrame`.
5. `SDL3MCP_WriteFrame` validates the buffer against the bound window's
   `SDL_GetWindowSizeInPixels`, copies it, clears the pending request, and wakes
   the server thread.
6. The server thread crops the optional region, optionally downscales, encodes
   PNG, base64-encodes it, and answers the tool call.
7. The app continues its loop; nothing blocked.

Rules that fall out of this design:

- The returned frame is one written **after** the request arrived; stale frames
  are never served.
- Only one capture is pending at a time. Tool calls are serialized on the server
  thread, so a slow capture delays the next tool call until it completes or
  times out. Agents that need to act "between" two frames should call
  `sdl_input`, then `sdl_capture`.
- A frame is dropped when no window is bound, when the window has zero drawable
  pixels, when `size` is not exactly `width * height * 4`, when the data pointer
  is null, when a dimension is not positive, or when the frame exceeds
  `max_frame_bytes`. A drop leaves the request pending so the app can retry; the
  last reason is appended to the timeout message as "Last drop: ..." and is
  visible in `sdl_info` as `capture.lastError` with `capture.dropped` counting
  the attempts.
- `sdl_capture` fails immediately with a clear error if no window is bound, if
  the region does not fit inside the frame, or if the app never answers in time.
- PNG encoding is the expensive part (tens of milliseconds at 1080p) and happens
  on the server thread while the app keeps rendering.
- `max_width` box-filters the image down before encoding, after any region crop.
  This is the main lever for payload size; 1280 or 1600 wide is usually enough
  for an agent to read UI text.
- Base64 adds about 33% to the PNG, and the whole image travels as one JSON
  line. A 1280×720 PNG is typically 50–150 KB, or ~70–200 KB on the wire.

## Input model

`sdl_input` takes an ordered array of 1–256 actions. The batch is validated as a
whole, then queued as one unit and applied in a single `SDL3MCP_AppIterate`, so
no frame boundary can appear in the middle of a gesture.

| Action | Fields |
| --- | --- |
| `key` | `key` (name or chord), `action` = `down`/`up`/`press` (default `press`), optional `modifiers`. |
| `text` | `text` (UTF-8, 1–1024 bytes). |
| `mouse_move` | `x`,`y` (window points) **or** `dx`,`dy` (relative), never both and never half a pair. |
| `mouse_button` | `button` = `left`/`middle`/`right`/`x1`/`x2`, `action` = `down`/`up`/`click`, optional `x`,`y`, `clicks` 1–3. |
| `scroll` | `x` (default 0), `y` in wheel notches. |
| `release_all` | Releases every key and button the library holds. |

There is no `repeat` field. A tap is a `press`; to repeat it, send another press
action. Auto-repeat is expressed as a repeated `down`: a `down` for a key that is
already held pushes another key-down event with SDL's `repeat` flag set and no
key-up, which is what a held keyboard key produces.

### Validation and error timing

- Structural problems fail the tool call before anything is queued: an unknown
  action type or property, a missing required field, a bad `mouse_move` shape, a
  `click` count outside 1–3, modifiers on a `down`/`up` action, more than four
  modifiers, an unknown modifier name, an empty or over-long key or text, or
  more than 256 actions. The message names the offending index, for example
  `actions[2]: x and y must be provided together`.
- Key names and chords are resolved at apply time on the app thread, because a
  single character depends on the current keyboard layout. An unknown key, a
  malformed chord, an unknown chord modifier, or a missing text-input focus
  therefore fails the `sdl_input` call when it returns at frame F+1, as an
  `isError` result.
- A `down` on a key that only exists with a modifier (such as `!`) is refused at
  apply time; use `press`, which supplies the modifier itself.

### Keys

- Names are matched case-insensitively against SDL's scancode names. A name is
  limited to 64 bytes. `-` and `_` are word separators, so `left-ctrl`,
  `left_ctrl`, and `left ctrl` are the same key; SDL's own names keep their
  punctuation (`-`, `keypad +`).
- Aliases cover the common spellings: `enter`, `esc`, `del`, `ins`, `pgup`,
  `pgdn`, `ctrl`, `control`, `shift`, `alt`, `option`, `gui`, `cmd`, `command`,
  `super`, `win`, `windows`, and `keypad plus`/`minus`/`multiply`/`divide`/
  `period`/`comma`/`equals`.
- Single printable characters are accepted and resolved against the active
  layout at apply time, including characters SDL has no scancode name for.
- Chords are written with `+`: `ctrl+shift+s`, `alt+f4`. An optional `modifiers`
  array (at most four entries) is merged into the chord. Modifiers are only
  valid on `press`; a held `down` carries no modifier mask.
- `press` is `down` immediately followed by `up`. On a key that is already held
  the key-down carries `repeat` set, and the following up still releases the
  key, because `press` always ends released.
- A key sent with `down` stays down across frames until an `up`, a
  `release_all`, or `SDL3MCP_AppShutdown`. That is how an agent holds a
  movement key. An `up` for a key the library is not holding is an error.

### Text

`text` sends one `SDL_EVENT_TEXT_INPUT` carrying the whole UTF-8 string, not key
events. It is meant for text fields and consoles; games that read key codes
should use `key` actions. The schema's `maxLength` is 1024 and the
implementation counts UTF-8 bytes, so a 1024-byte string is the hard limit. Text
requires a bound window and an active text input (`SDL_TextInputActive`);
otherwise the action fails at apply time.

The event's `text` pointer refers to the queued batch, which the library keeps
alive until frame F+1. The app must consume it during that window and must not
retain the pointer.

### Mouse

- Absolute coordinates are in **window points**, the same coordinate space as
  `SDL_EVENT_MOUSE_MOTION.x/y`, and are clamped to the window.
- The library tracks a virtual cursor. `xrel`/`yrel` are derived from the move,
  except when `warp_cursor` is on and the platform's warp event already carries
  the delta; in that case the synthetic event leaves its delta at zero so delta
  accumulators do not count the move twice.
- With `warp_cursor = true` (the default), an absolute move also warps the OS
  cursor with `SDL_WarpMouseInWindow`, so `SDL_GetMouseState` and hit testing
  follow the synthetic motion. The library does not require window focus, and a
  synthetic zero-delta motion event always accompanies the warp so event-driven
  apps see the move even when the platform ignores the warp (a Remote Desktop
  or headless session, for example).
- In relative mouse mode, warping is skipped and absolute moves are converted to
  deltas from the virtual cursor. `dx`/`dy` are passed through as `xrel`/`yrel`.
- `click` is a full down/up pair with the requested `clicks` count. Pressing a
  button that is already held, or releasing one that is not, fails the action.
- Buttons and wheel use `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` and
  `SDL_EVENT_MOUSE_WHEEL` on the bound window.

### Scroll

`scroll` sends one `SDL_EVENT_MOUSE_WHEEL` with the virtual cursor position.
`1.0` is one wheel notch and positive `y` scrolls down; `x` defaults to 0. The
event's integer fields are left at 0, and the values are passed through
unchanged.

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
`2025-11-25` and always answers `initialize` with that revision, whatever the
client asked for.

- Advertised capability: `tools` only. No resources, prompts, sampling, or
  logging.
- `initialize`, `notifications/initialized`, `ping`, `tools/list`, and
  `tools/call` are supported. `serverInfo` is `{"name": "sdl3mcp", "version":
  "0.1.0"}`.
- `initialize` must come first and only once: `tools/list` and `tools/call`
  before it fail with `-32600` ("tools are unavailable before initialize"), and
  a second `initialize` also fails with `-32600`. `ping` is answered at any time.
- Requests are handled one at a time on the single server thread. A blocking
  tool call delays every later request: `sdl_input` waits up to
  `frame_timeout_ms` for its F+1 acknowledgement and `sdl_capture` waits up to
  its timeout for a frame, so a `ping` queued behind one of them is answered
  only after that call returns. Nothing is cancelled by the delay.
- A JSON array is rejected as a batch with `-32600`; other malformed input is
  answered with a JSON-RPC error and the session continues. Invalid JSON is
  `-32700`, an unknown method is `-32601`, and bad parameters or an unknown tool
  is `-32602`.
- Request ids may be strings or numbers; any other id is rejected with `-32600`
  and a `null` id. Notifications get no response and change no state.
- Inbound lines are capped at 1 MiB; a longer line is a transport error and ends
  the session.
- Every outbound message is flushed immediately.
- Tool failures use `isError: true` with a text explanation; JSON-RPC errors are
  reserved for protocol-level problems.

Tool annotations:

| Tool | `readOnlyHint` | `idempotentHint` | `openWorldHint` |
| --- | --- | --- | --- |
| `sdl_info` | `true` | `true` | `false` |
| `sdl_input` | `false` | `false` | `false` |
| `sdl_capture` | `true` | `false` | `false` |

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
  "input": { "heldKeys": ["right"], "heldButtons": ["left"] },
  "capture": {
    "pending": false,
    "count": 3,
    "dropped": 0,
    "lastWidth": 1920,
    "lastHeight": 1080,
    "lastBytes": 84213,
    "lastMs": 27,
    "lastError": null
  },
  "limits": { "frameTimeoutMs": 2000, "maxFrameBytes": 33554432 },
  "frame": 128,
  "appliedFrame": 127
}
```

- `sdlVersion` is the linked SDL version as `major.minor.micro`.
- `videoDriver` is the current SDL video driver, or `null` until a window is
  bound.
- `window` is `null` when no window is bound; otherwise sizes are in points
  (`width`, `height`) and drawable pixels (`pixelWidth`, `pixelHeight`).
- `heldKeys` are canonical lowercase scancode names in press order; a key held
  with `down` is listed until released. `heldButtons` use the same names as the
  `sdl_input` button enum.
- `capture.pending` is true while a capture waits for a frame; `count` is
  successful captures; `dropped` counts rejected `SDL3MCP_WriteFrame` attempts.
  `lastWidth`/`lastHeight` describe the frame **after** any region crop and
  **before** `max_width` downscaling, `lastBytes` is the PNG size, `lastMs` is
  the encode time, and `lastError` is `null` when the last capture succeeded (or
  none ran), otherwise the last drop or capture error string.
- `limits` repeats the effective `frame_timeout_ms` and `max_frame_bytes`.
- `frame` is the library's frame counter and `appliedFrame` is the last frame at
  which an input batch was applied (0 when none was).

When no window is bound, `window` is `null` and the tool still succeeds, which
makes it a useful first call after startup.

### `sdl_input`

```json
{
  "name": "sdl_input",
  "description": "Queue a batch of synthetic key, text, mouse, scroll, or release-all actions. The batch is applied atomically at the next frame, and the call returns after the following frame, once the application has consumed the generated events. Follow with sdl_capture to observe the result.",
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
              "description": "Press, release, or click a mouse button at the cursor or at optional window points (x, y).",
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

The result is a text summary such as `3 actions applied at frame 12`. Structural
errors fail the call before queueing; key and chord errors fail it at F+1. See
[Validation and error timing](#validation-and-error-timing).

### `sdl_capture`

```json
{
  "name": "sdl_capture",
  "description": "Ask the application for a fresh frame through SDL3MCP_FrameRequested() and return it as a PNG image block. The optional region crops pixels from the full frame, using pixelWidth x pixelHeight from sdl_info, with a top-left origin; it must fit inside the frame and is applied before max_width downscaling. Use max_width to keep the payload small.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "max_width": { "type": "integer", "minimum": 16, "maximum": 16384 },
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

- `region` is a crop in **full-frame pixels** — the `pixelWidth` × `pixelHeight`
  reported by `sdl_info` — with a top-left origin. All four fields are required,
  `x`/`y` must be at least 0, `width`/`height` at least 1, and the rectangle must
  fit inside the frame. It is applied before `max_width`.
- `max_width` is 16–16384; the crop is downscaled with a box filter to that
  width, preserving the aspect ratio.
- `timeout_ms` is 1–60000 and defaults to `frame_timeout_ms`.

The result has two content blocks: a short text summary and the image.

```json
{
  "content": [
    { "type": "text", "text": "1280x720 PNG from 1920x1080 at 0,0, 91234 bytes, 34 ms" },
    { "type": "image", "mimeType": "image/png", "data": "iVBORw0KGgoAAAANSUhEUg..." }
  ],
  "isError": false
}
```

On timeout the result is an error, not an image, and names the last drop reason
when there was one:

```json
{
  "content": [
    { "type": "text", "text": "no frame arrived within 2000 ms. The app must call SDL3MCP_WriteFrame() when SDL3MCP_FrameRequested() returns true, with tightly packed RGBA8888 at SDL_GetWindowSizeInPixels(). Last drop: size 1036800 != 960*540*4." }
  ],
  "isError": true
}
```

### Example session

```text
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"agent","version":"1.0"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"sdl3mcp","version":"0.1.0"}}}
→ {"jsonrpc":"2.0","method":"notifications/initialized"}
→ {"jsonrpc":"2.0","id":2,"method":"tools/list"}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[ ... sdl_info, sdl_input, sdl_capture ... ]}}
→ {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"sdl_input","arguments":{"actions":[{"type":"key","key":"right","action":"down"},{"type":"key","key":"space","action":"press"}]}}}
← {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"2 actions applied at frame 7"}],"isError":false}}
→ {"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"sdl_capture","arguments":{"max_width":1280}}}
← {"jsonrpc":"2.0","id":4,"result":{"content":[{"type":"text","text":"1280x720 PNG, 91234 bytes, 34 ms"},{"type":"image","mimeType":"image/png","data":"iVBORw0KGgo..."}],"isError":false}}
→ {"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"sdl_input","arguments":{"actions":[{"type":"key","key":"right","action":"up"},{"type":"key","key":"escape","action":"press"}]}}}
← {"jsonrpc":"2.0","id":5,"result":{"content":[{"type":"text","text":"2 actions applied at frame 9"}],"isError":false}}
```

## Threading and lifetime

| Thread | May touch | Must not touch |
| --- | --- | --- |
| App thread | SDL, the window, the renderer, all `SDL3MCP_*` functions. | — |
| Server thread | stdin/stdout duplicates, JSON, the copied frame buffer, the input queue. | Any SDL function, including `SDL_GetWindowSize`. |

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
   held input, restores stdin/stdout, and frees frames.

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

- **stdout belongs to MCP until `SDL3MCP_AppInit` returns.** With `--mcp` the
  library duplicates the original stdout for the protocol and redirects the
  host's stdout to stderr, so later writes are safe, but anything printed
  before `SDL3MCP_AppInit` goes straight into the JSON-RPC stream. Use stderr or
  `SDL_Log` (which writes to stderr) for diagnostics.
- **stdin belongs to MCP.** Do not read stdin in the app; after `SDL3MCP_AppInit`
  the host's stdin is `/dev/null`. If your app needs console input, use
  `--mcp=off`.

When the client closes the pipe, the server sees EOF, stops cleanly, and leaves
the app running. `SDL3MCP_AppIterate` keeps returning `SDL_APP_CONTINUE`; the
app's own lifecycle is unaffected.

## Troubleshooting

**The client never receives a response.** The app wrote to stdout before
`SDL3MCP_AppInit`, or closed its stdout. Move all early output to stderr.

**Nothing starts.** The default is no server. Launch with `--mcp`, or set
`cfg.transport = SDL3MCP_TRANSPORT_STDIO`. Check `--mcp=off` is not in the
launcher.

**`sdl_capture` says no frame arrived.** Confirm the render loop calls
`SDL3MCP_FrameRequested()` every frame and, when it is true, calls
`SDL3MCP_WriteFrame` with exactly `width * height * 4` bytes at the window's
pixel size, before `SDL_RenderPresent`. Read the appended "Last drop" reason and
`sdl_info`'s `capture.lastError`. Common causes: reading pixels after
`SDL_RenderPresent` (do it before), allocating for `SDL_GetWindowSize` instead of
`SDL_GetWindowSizeInPixels`, passing a surface with row padding, or a minimized
window with zero drawable pixels.

**Frames are upside down or colors are swapped.** The buffer must be
top-left-first RGBA8888 with no row padding. `SDL_RenderReadPixels` returns a
surface; convert it with `SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32)`
and pack its rows when the pitch is not `width * 4`.

**Mouse clicks land in the wrong place on a HiDPI display.** Frames are in
pixels; mouse coordinates are in window points. `sdl_info` reports both, and the
ratio is `pixelDensity`. Send points.

**Keys have no effect.** The app is probably polling `SDL_GetKeyboardState`
instead of handling key events; see [the keyboard-state
caveat](#the-keyboard-state-caveat). Mouse movement usually still works because
of cursor warping.

**A key is stuck.** The library releases held keys during
`SDL3MCP_AppShutdown`. To recover mid-session, send `{"type":"release_all"}`.

**Payloads are too large.** Pass `max_width` (1280 is a good default for UI
work), crop with `region`, and lower `max_frame_bytes` if you want a hard
ceiling. Base64 adds about a third to the PNG size.

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
- POSIX only for now: the transport and fd isolation use `poll`, `dup2`, and
  `/dev/null`. Windows support is a TODO.
- `log_level` is validated but not yet connected to a leveled logger.

Plausible next steps: `sdl_window` for resize/title/fullscreen, `sdl_quit`,
gamepad and touch actions, frame diffs, and a `sdl3mcp://frame/latest`
resource. Anything added has to keep the six-call wiring and the "library never
touches your renderer" rule intact.

## Development

```
include/SDL3MCP/SDL3MCP.h          public C header, hand-written, C11-clean
include/SDL3MCP/SDL3MCP_version.h.in
src/api.cpp                        lifecycle, tool handlers, info snapshot
src/configuration.*                SDL3MCP_Configuration and the --mcp parser
src/keymap.*                       scancode names, aliases, layout fallback
src/input.*                        input action model and structural validation
src/input_json.*                   sdl_input argument parser
src/replay.*                       key, chord, and text event replay
src/mouse.*                        mouse motion, buttons, wheel, warp
src/app_input.*                    batch -> event plan
src/queue.*                        input queue and F+1 completion
src/json_rpc.*                     JSON-RPC 2.0 message layer
src/tools.*                        tool catalog and JSON schemas
src/session.*                      MCP initialize/tools/ping session
src/info.*                         sdl_info serialization
src/frame.*                        frame validation, crop, downscale, base64
src/capture.*                      pending capture state and statistics
src/transport.*                    line-oriented stdio transport
src/server.*                       server thread loop
src/stdio_isolation.*              fd duplication and redirect
src/png.hpp, src/stb_image_write_impl.cpp
third_party/stb/                   vendored stb_image_write.h
third_party/nlohmann/              vendored json.hpp fallback
examples/minimal/main.c           the quick-start app
tests/                             unit and integration tests, check_readme.py
CMakeLists.txt
```

Test suite (all registered with CTest): `header_c`, `header_cpp`,
`configuration_tests`, `keymap_tests`, `input_tests`, `input_json_tests`,
`replay_tests`, `mouse_tests`, `queue_tests`, `app_input_tests`,
`input_tool_tests`, `capture_tests`, `capture_tool_tests`, `json_rpc_tests`,
`session_tests`, `frame_tests`, `info_tests`, `server_tests`, and six
`lifecycle_*` scenarios. When Python 3 is available, `readme_check` runs
`tests/check_readme.py`: it compiles both complete C examples, parses every
JSON block, and checks the schemas, the example run path, and the loop order.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSDL3MCP_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run just the documentation check:

```sh
ctest --test-dir build -R readme_check --output-on-failure
```

Guidelines: the C API never throws, all C++ exceptions stop at the boundary, the
public header must compile as C11 and C++20, and the server thread must never
call SDL. New tool parameters are snake_case on the wire and camelCase in C++.
Behavior changes get a regression test, and `README.md` changes with them.

## License

The project is zlib-licensed, the same license as SDL3. See `LICENSE`.

Vendored dependencies keep their own licenses: `third_party/nlohmann/json.hpp`
is MIT (the header carries its SPDX identifier), and
`third_party/stb/stb_image_write.h` is dual-licensed public domain / MIT with
both texts at the end of the file.
