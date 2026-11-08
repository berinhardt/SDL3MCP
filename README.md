# SDL3MCP

An MCP server you link into any SDL3 application.

SDL3MCP gives an agent two things every SDL3 app already has: **input** and a
**rendered frame**. Link the library, wire six calls into your existing main
callbacks, and the app speaks the Model Context Protocol over stdio, or over a
socket it dials. The agent can press keys, type text, move and click the mouse,
scroll, inspect the window, and ask for the current frame: raw RGBA pixels from
the library, or a PNG when it runs behind the supervisor.

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
- [Supervisor](#supervisor)
- [Relay](#relay)
- [Spawner](#spawner)
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
| `sdl_input` | Send a batch of key, text, mouse-move, mouse-button, scroll, or release-all actions; the call always returns the frame that follows the batch. Behind `sdl3mcp` that frame arrives as an `image/png` block next to text naming both the applied frame and the attached frame. |
| `sdl_capture` | Ask the app for a fresh frame, optionally cropped, and receive its raw RGBA8888 pixels plus the exact payload size; behind `sdl3mcp` the same pixels are returned at the app's native pixel size as an `image/png` block. |
| `sdl_terminate` | Ask the app to exit cleanly by pushing one `SDL_EVENT_QUIT` into its own event loop. |

Typical agent loop:

1. `sdl_info` to see what it is looking at.
2. `sdl_capture` for a baseline screenshot.
3. `sdl_input` to click, type, or hold a key; the call returns the frame that
   follows the batch, so the observation costs no extra round trip.
4. `sdl_capture` only when a later frame is needed without new input.

## How it works

```
┌────────────────────────────── app process ───────────────────────────────┐
│                                                                          │
│   app thread (SDL)                       server thread                    │
│   ┌─────────────────────┐                ┌────────────────────────────┐   │
│   │ SDL_AppInit         │                │ stdio or socket transport  │   │
│   │ SDL_AppIterate ─────┼─ geometry ───▶ │  JSON-RPC 2.0, one line    │   │
│   │  ├ apply input      │◀─ input batch ─┤  per message               │   │
│   │  ├ WriteFrame ──────┼─ RGBA copy ───▶│  sdl_input  → input queue  │   │
│   │ SDL_AppQuit         │◀─ frame req ───┤  sdl_capture → raw RGBA    │   │
│   └─────────────────────┘                └────────────────────────────┘   │
│             │                                            │                │
│     SDL event queue, renderer                  client's pipes or socket   │
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
That is the first frame in which the app has certainly consumed the events. The
call then asks for a fresh frame, which the app writes on a later iteration, so
the answer names both numbers: the frame the actions were applied at and the
frame attached, for example `3 actions applied at frame 12; frame 14 attached,
rendered after the actions were delivered`. The attached frame is always
rendered after the actions were delivered - one acknowledgement frame and at
least one capture frame later - so an unchanged frame means the actions had no
effect, such as a click that missed a widget; `sdl_info` carries the geometry
for the next attempt. The gap is not fixed, because a busy or slow app may
attach a later frame. A capture follows the same rule: the frame you receive was
written after the request arrived, and `sdl_capture`'s summary names it too.

### What it is not

- Not an OS-level input injector. It posts SDL events into your process, so it
  only affects apps that link it (and only the event-driven parts of them).
- Not a remote desktop. The library itself never listens: in its default stdio
  mode the MCP client must be able to spawn the app and own its stdin/stdout. The
  optional `sdl3mcp` relay adds a socket below the client, so an app can also
  dial in; see [Relay](#relay).
- Not a screenshot API for other processes, and it cannot capture a window it
  was not given.
- It does not create, resize, or move windows, and it has no audio, gamepad, or
  touch tools yet.

## Requirements

- SDL 3.2 or newer. Both complete examples below use only SDL 3.2 APIs.
- A C11 compiler for the public header; the implementation is C++20.
- CMake 3.21 or newer.
- A pthreads-capable platform. The transports, fd isolation, and server thread
  are POSIX today; Windows support is a TODO.
- `nlohmann/json.hpp` for the protocol, plus `stb_image_write.h` for the
  supervisor's PNG encoding. nlohmann is resolved from the system first and
  falls back to `third_party/nlohmann/json.hpp`; stb is vendored in
  `third_party/stb` and used only by `sdl3mcp`.

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
| `SDL3MCP_BUILD_CLI` | `OFF` | Build the `sdl3mcp` executable: child supervisor, relay, and spawner. |
| `SDL3MCP_BUILD_TESTS` | `ON` if top-level | Build the unit tests and register them with CTest. |
| `SDL3MCP_USE_SYSTEM_JSON` | `ON` | Try `find_package(nlohmann_json CONFIG QUIET)` before using the vendored header. |

The library has no image dependency. stb is vendored but linked only into the
supervisor: `third_party/stb/stb_image_write.h` is a private include of
`sdl3mcp_supervisor_objects`, and `STB_IMAGE_WRITE_IMPLEMENTATION` is compiled
exactly once in `supervisor/stb_image_write_impl.cpp`, so no stb symbol reaches
`libSDL3MCP`. nlohmann/json is exported as the
`nlohmann_json::nlohmann_json` interface target: the system package when found,
otherwise the header in `third_party/nlohmann`.

The library's translation units are split into two object libraries:
`sdl3mcp_wire_objects` (protocol, session, framing, transports, and
configuration) and `sdl3mcp_app_objects` (input, replay, capture plumbing, and
info). The wire half references no SDL symbol; the app half is the only one that
links `SDL3::SDL3`. `SDL3MCP` keeps both, so applications are unaffected, while
the `sdl3mcp` supervisor links only `sdl3mcp_supervisor_objects` and
`sdl3mcp_wire_objects`: no SDL target and no SDL symbol reach the relay binary,
which is what makes a minimal relay image possible.

The shared configuration uses a separate build directory:

```sh
cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Debug -DSDL3MCP_BUILD_SHARED=ON
cmake --build build-shared --parallel
ctest --test-dir build-shared --output-on-failure
```

The shared library exports only the six public C entry points while its C++
internals stay hidden, so unit tests that include the private `src/` headers
link the library's object files instead of the shared object. The sources are
compiled once in the wire and app object libraries that both the library and
those tests consume.

`CMakePresets.json` adds optional presets for those two configurations and for
the sanitizer runs. They live under `build/<preset>` and do not replace the
`cmake -S . -B build` flow above:

```sh
cmake --list-presets=all

cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

The `shared` preset is the same sequence with `--preset shared`. All four
presets also set `SDL3MCP_BUILD_CLI=ON`, so the supervisor binary and its
tests are built in every configuration. The `asan` preset compiles with
`-fsanitize=address,undefined` and runs the full suite with leak detection on,
including the supervisor tests; only the SDL dummy-video-driver integration
tests (`input_tool_tests`, `capture_tool_tests`, `terminate_tests`,
`supervisor_real_app`) set `ASAN_OPTIONS=detect_leaks=0`, because SDL keeps
process-wide allocations those tests cannot free. The `tsan` preset compiles
with `-fsanitize=thread` and runs `queue_tests`, `server_tests` and
`supervisor_tests`, the thread handoffs and the fork/exec + monitor-thread
process the design depends on; the real-example supervisor test stays out of
that filter because real SDL under TSan is out of scope. Sanitizer runs are
local only; there is no CI workflow.

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
        SDL3MCP_WriteFrame(rgba->pixels, size, rgba->w, rgba->h);
    } else {
        uint8_t *packed = SDL_malloc(size);
        if (packed) {
            for (int y = 0; y < rgba->h; ++y) {
                SDL_memcpy(packed + rowBytes * (size_t)y,
                           (const uint8_t *)rgba->pixels + (size_t)rgba->pitch * (size_t)y,
                           rowBytes);
            }
            SDL3MCP_WriteFrame(packed, size, rgba->w, rgba->h);
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
enables a transport or the app is launched with `--mcp`.

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

MCP over stdio means the client spawns the app and talks over its pipes. With
`--mcp=HOST:PORT` the app instead dials a server the client or a relay is
listening on. A stdio client configuration looks like:

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

The transport owns real file descriptors, so the transports differ in what they
do to the host process.

With `SDL3MCP_TRANSPORT_NONE` the library opens nothing, duplicates nothing,
reads nothing, and writes nothing: the host's stdin and stdout are untouched.

With `SDL3MCP_TRANSPORT_STDIO`, `SDL3MCP_AppInit` duplicates the original
stdin/stdout for the server and then redirects the host's descriptors:

- stdin is redirected to `/dev/null`, so a host `scanf`/`getline` sees EOF
  instead of eating the client's JSON-RPC bytes;
- stdout is redirected to stderr, so a stray `printf` cannot corrupt the
  JSON-RPC stream;
- stderr is never touched and is where diagnostics belong.

With `SDL3MCP_TRANSPORT_CONNECT`, the library opens one socket to its endpoint
and uses it for both reading and writing. It duplicates and redirects nothing, so
the host keeps its own stdin and stdout, and neither is the MCP stream. The dial
happens once, in `SDL3MCP_AppInit`; a refused or unresolvable endpoint fails
startup instead of leaving the app without MCP.

The host must not write to stdout **before** `SDL3MCP_AppInit` when the stdio
transport is in use: at that point stdout is still the pipe the client is
reading. The originals are restored in `SDL3MCP_AppShutdown`.

## Configuration

```c
typedef enum SDL3MCP_Transport {
    SDL3MCP_TRANSPORT_NONE = 0,
    SDL3MCP_TRANSPORT_STDIO,
    SDL3MCP_TRANSPORT_CONNECT
} SDL3MCP_Transport;

typedef struct SDL3MCP_Configuration {
    SDL3MCP_Transport transport;
    uint32_t frame_timeout_ms;
    uint32_t max_frame_bytes;
    bool warp_cursor;
    int log_level;
    char connect_host[64];
    uint16_t connect_port;
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
| `transport` | `SDL3MCP_TRANSPORT_NONE` | `NONE` starts no server; `STDIO` starts the stdio MCP server; `CONNECT` dials `connect_host:connect_port` and serves over that socket. |
| `frame_timeout_ms` | `2000` | Default `sdl_capture` frame timeout and the time `sdl_input` waits for its F+1 acknowledgement; an `sdl_input` call without `timeout_ms` gets twice this as its overall budget. |
| `max_frame_bytes` | `32 MiB` | Upper bound on a raw RGBA frame accepted by `SDL3MCP_WriteFrame`. 4K (`3840×2160×4`) fits; 5K and above need a larger value. |
| `warp_cursor` | `true` | Move the OS cursor with `SDL_WarpMouseInWindow` on absolute mouse moves and positioned `mouse_button` actions. |
| `log_level` | `-1` | Accepted and validated as -1..4 (`0` off, `1` error, `2` warn, `3` info, `4` debug). The library has no leveled logger yet, so today only command-line usage and errors are written to stderr. |
| `connect_host` | empty | `CONNECT` only: the endpoint to dial, at most 63 bytes before the terminator. |
| `connect_port` | `0` | `CONNECT` only: the endpoint port, 1–65535. |

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
| `--mcp=HOST:PORT` | Dial an MCP endpoint at `HOST:PORT` and serve it over that socket (`[V6]:PORT` is accepted for IPv6). |
| `--mcp=off` | Start nothing. Useful to override a configuration that enabled stdio. |

Transport values are case-insensitive. A dialed endpoint is one connection with
no retry: if the host cannot be resolved or reached, `SDL3MCP_AppInit` returns
`SDL_APP_FAILURE` and `SDL_GetError()` names the endpoint. It never falls back to
stdio or to no server, so a relay that is not listening is a loud startup failure
rather than an app that silently has no MCP. The spawner supplies this argument
itself; a launcher uses it to reach a relay. Any other value prints usage to
stderr and fails the same way.

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

## Supervisor

The library speaks MCP on the app's own transport, so a client that starts the
app directly loses its endpoint whenever the app exits or crashes.
`sdl3mcp` is an optional executable that keeps one MCP connection
alive across app restarts. It has three modes, and they differ only in where the
application peer comes from:

- `--target <executable>` runs the app as a child process behind private pipes
  (this section);
- `--listen=HOST:PORT` relays the client's stdio to whichever app dials in
  ([Relay](#relay));
- `--connect=HOST:PORT --targets=PATH` spawns a configured app behind a relay
  ([Spawner](#spawner)).

In every mode the client keeps the same endpoint. The advertised tool set
depends on the mode, not on whether an application is attached: a spawn, an
attach, and a detach leave it unchanged.

Build it alongside the example:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSDL3MCP_BUILD_EXAMPLES=ON -DSDL3MCP_BUILD_CLI=ON
cmake --build build --parallel
```

`SDL3MCP_BUILD_CLI` stays `OFF` by default, so the plain
`cmake -S . -B build` flow does not build it. The `debug`, `shared`, `asan` and
`tsan` presets set it to `ON`: each one builds `sdl3mcp` and registers
`supervisor_tests` and `supervisor_real_app`. The `asan` preset runs both with
leak detection on, except that `supervisor_real_app` turns leak detection off for
itself and the SDL process it spawns; the `tsan` preset includes only
`supervisor_tests` in its filtered thread-race suite.

### Supervisor command line

```text
sdl3mcp [--startup-timeout-ms N] [--terminate-timeout-ms N] --target <executable> [app args...]
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--startup-timeout-ms N` | `10000` | Budget for fork/exec, the MCP handshake, and the first `sdl_info` that reports a bound window. |
| `--terminate-timeout-ms N` | `60000` | How long `sdl_terminate` may take before the watchdog sends SIGTERM and then SIGKILL. |
| `--target <executable>` | *(required)* | The app to run. Everything after it is passed to the app verbatim. |

Options are parsed only before `--target`; after it, a flag that looks like a
supervisor option belongs to the app. The supervisor never calls SDL, so it has
no window, renderer, or video driver of its own.

### States and `tools/list`

The advertised tool list is a function of the mode, never of whether an
application is attached:

| Mode | `tools/list` |
| --- | --- |
| `--target` (spawn) | `spawn` plus `sdl_info`, `sdl_input`, `sdl_capture`, `sdl_terminate`. |
| `--listen` (relay) | `wait_session` plus the same four application tools. |
| `--connect --targets` (spawner) | one `app_spawn_<alias>` per target plus the same four application tools. |

A spawner behind a relay contributes its `app_spawn_<alias>` entries to the
relay's set when it connects; that is the only topology change a client
observes. Spawning an application, attaching one, or detaching one changes no
advertised name, so none of them sends `notifications/tools/list_changed`.

| State | `tools/list` | When |
| --- | --- | --- |
| `Idle` | `spawn`, `sdl_info`, `sdl_input`, `sdl_capture`, `sdl_terminate` | At startup, and again after the child exits for any reason. |
| `Starting` | unchanged | Between accepting `spawn` and the child reporting a window. |
| `Running` | unchanged | While the child is alive. |

`sdl_capture` always carries the supervisor's definition because presentation
belongs to the supervisor; see
[Capture through the supervisor](#capture-through-the-supervisor). Because the
set does not depend on the child, a spawn sends no
`notifications/tools/list_changed`: a name the client has already listed cannot
arrive late behind an asynchronous refresh.

`spawn` is the supervisor's own tool and is never forwarded. It starts one child
(synchronously, so there is never more than one), completes the MCP handshake,
and waits until `sdl_info` reports a bound window. On failure the result is an
error that includes the child's recent stderr, and the supervisor returns to
`Idle`.

While a child is running, every other `tools/call` whose name the child
advertised is forwarded to it verbatim: the arguments go in unchanged, the
child's content blocks and `isError` come back unchanged, and a JSON-RPC error
from the child is returned unchanged. A name the child did not advertise is
answered with `the application does not provide <name>` instead of being
forwarded. A raw frame is the one result the supervisor presents rather than
relays, because presentation belongs to the supervisor: `sdl_capture` and
`sdl_input` both answer with pixels, and both leave here as text plus one
`image/png` block. `initialize`, `ping`, and `tools/list` are always answered by
the supervisor.
If the child exits or crashes on its own, the exit handler returns the
supervisor to `Idle` and puts `spawn` back; the advertised set is unchanged, so
nothing is announced. `notifications/tools/list_changed` is sent only when the
advertised set itself changes, for example when an attached peer announces a
list of its own that adds or removes names.

### Capture through the supervisor

The library's `sdl_capture` returns raw pixels so the library never links an
image encoder. The supervisor owns presentation: it advertises its own
`sdl_capture` definition in every mode, alongside the application's other three
tools, and keeps the client-facing `image/png` contract. The PNG carries the
pixels at the app's native pixel size: captures are never resized, so a
coordinate read in the image is the same coordinate in the app's pixel space.

The frame `sdl_input` returns travels the same route: one raw frame, one native-
size PNG, one `[text, image/png]` result. The difference is the text: a capture
has none of its own, so the supervisor summarises the PNG and names the frame it
attached, while an input answer carries the library's own line naming both the
applied frame and the attached frame. Both answers therefore say which frame
they show, and a capture's summary is not anonymous either.

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

- `region` and `timeout_ms` are forwarded to the child unchanged. The region is
  applied by the library, and the supervisor encodes whatever dimensions it
  receives.
- The supervisor never resizes: the PNG has exactly the dimensions the app wrote,
  or the region's when one was given. `region` is therefore the only lever for
  payload size.
- The supervisor reads the child's result line, checks `format == "rgba8888"`,
  that both dimensions are positive, and that
  `byteLength == width * height * 4`. It rejects a frame larger than 64 MiB
  before allocating the payload. A mismatched header or a short payload is a
  tool error.
- It then encodes the frame as PNG, base64-encodes it, and answers with the usual
  `[text, image/png]` content and `isError: false`. The header's `frame` names
  the app frame whose pixels arrived. For a capture the text summary names the
  size, the region when one was given, the PNG size, and that frame, for example
  `960x540 PNG at 0,0, 41234 bytes; frame 31 attached`; for an input answer the
  library's text is kept, for example `1 actions applied at frame 12; frame 14
  attached, rendered after the actions were delivered`.
- A JSON-RPC error object or a text error result from the child passes through
  unchanged, so a capture timeout still reports the library's own message.

```text
→ {"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"sdl_capture","arguments":{}}}
← {"jsonrpc":"2.0","id":4,"result":{"content":[{"type":"text","text":"1920x1080 PNG, 91234 bytes; frame 42 attached"},{"type":"image","mimeType":"image/png","data":"iVBORw0KGgo..."}],"isError":false}}
```

### Terminating the app

`sdl_terminate` still means "ask the app to quit cleanly": the supervisor
forwards the call, then waits for the child to actually exit within
`--terminate-timeout-ms`. If the child exits, the result is the text
`terminated`. If the deadline passes, the supervisor sends SIGTERM, waits about
two seconds, sends SIGKILL, and returns a text result that says termination was
forced. Either way the exit handler restores `Idle`/`spawn`; the advertised set
is unchanged, so it sends no `notifications/tools/list_changed`.

The watchdog exists because `sdl_terminate` only pushes a quit event: an app
that ignores `SDL_EVENT_QUIT`, hangs in a render loop, or never returns from
`SDL_AppIterate` would otherwise keep the child alive forever behind a
working-looking MCP endpoint.

Over a relay the request cannot be enforced: the relay holds no pid, so
`sdl_terminate` is an ordinary forwarded call and the application alone decides
whether it quits.

### Stopping the supervisor

Supervisor stdin EOF, SIGINT, SIGTERM, and a normal return all stop the child
before the supervisor exits: SIGTERM, a grace period, then SIGKILL if it is
still alive. The child is reaped by the supervisor, so stopping the client never
leaves an orphan app behind.

### Client configuration

A local client can point at the supervisor instead of the app. This is the
`opencode.json` used during development; run it from the repository root so the
relative paths resolve:

```json
{
  "mcp": {
    "servers": {
      "sdl3mcp": {
        "type": "local",
        "command": [
          "build/sdl3mcp",
          "--target",
          "build/examples/minimal",
          "--mcp"
        ]
      }
    }
  }
}
```

The supervisor is POSIX-only, like the rest of the project: it uses
`fork`/`exec`, pipes, sockets, and POSIX signals. `CMakeLists.txt` rejects a
Windows configure rather than shipping an untested path.

## Relay

A client cannot always launch the app: it may live in another container, on a
device, or behind a port forward. `sdl3mcp --listen=HOST:PORT` is the same
executable in **relay** mode. It still speaks MCP on its own stdin/stdout, but
instead of starting a child it binds one TCP listener and proxies every request
to whichever application dials in with `--mcp=HOST:PORT`. The client keeps one
endpoint for the relay's whole lifetime; only the application behind it comes and
goes.

### Relay command line

```text
sdl3mcp --listen=HOST:PORT [--startup-timeout-ms N]
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--listen=HOST:PORT` | *(required)* | Address to bind. `[V6]:PORT` is accepted for IPv6, and port `0` asks the kernel for an ephemeral port. The bound endpoint is printed to stderr as `sdl3mcp: listening on HOST:PORT`. |
| `--startup-timeout-ms N` | `10000` | Budget for the application's private `initialize` and `tools/list` handshake after it connects. |

`--listen` may be given only once and cannot be combined with `--target`. A bind
failure is a startup error and the relay exits non-zero.

### Before a session: `wait_session`

`tools/list` always advertises `wait_session` together with the application's
four tools (`sdl_info`, `sdl_input`, `sdl_capture`, `sdl_terminate`), whether or
not a session is attached. Before one attaches, every forwarded `tools/call`
fails with `no application session is attached; call wait_session first`.

```json
{
  "name": "wait_session",
  "description": "Wait for an application to connect to the relay's listening socket. Arguments: optional timeout_ms (1-60000); without it the call waits indefinitely, and an expired timeout is a normal tool error. Returns as soon as a session is attached. The application's tools are advertised alongside this one whether or not it is attached. Example: {\"timeout_ms\":5000}",
  "inputSchema": {
    "type": "object",
    "properties": {
      "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 }
    },
    "additionalProperties": false
  },
  "annotations": {
    "readOnlyHint": true,
    "idempotentHint": false,
    "openWorldHint": true
  }
}
```

- Without `timeout_ms` the call waits indefinitely; with it, an expired timeout
  is a normal `isError: true` content result, not a JSON-RPC error.
- It returns `an application session attached`, or
  `an application session is already attached` when one is up.
- Requests are serialized like every other call, so a `wait_session` parked on
  the relay delays `ping` behind it.

### After a session attaches

The relay completes a private MCP handshake with the application (the same
protocol version and a `tools/list`). It then merges the application's entries
over its mode-derived list by name: the application's definition wins on a
collision except `sdl_capture`, which always keeps the supervisor's PNG
definition, and names the application does not advertise stay advertised.
Attaching a bare application is therefore byte-identical to the list without it,
and no `notifications/tools/list_changed` is sent on attach or detach.

The one visible topology change is a spawner connecting: its `app_spawn_<alias>`
entries are added to the relay's list. A spawner client necessarily connects
before any spawn call, and a client that connects afterwards sees the new names
in its first `tools/list`.

A peer that announces a change of its own is followed rather than quoted: on
`notifications/tools/list_changed` the relay asks that peer for `tools/list`
again, re-merges it, and republishes the result with its own `list_changed`.
That keeps a chained peer's genuine additions and removals visible.

Exactly one application session is accepted at a time. A second connection is
closed as soon as it is accepted instead of queueing behind the first, and the
listener never hands it to the proxy. When the attached application's socket
closes — a clean exit, a crash, or a network drop — the relay drops the peer's
merged entries and keeps the client's stdio endpoint open for the next
application. The hang-up is probed on every loop pass, so an idle relay notices
it while nothing is in flight.

A forwarded call is bounded as well: when the peer stops answering, the relay
detaches it, returns a normal error content block naming the tool, and is back
in `wait_session` instead of holding the request slot forever.

`sdl_capture` keeps the supervisor contract while a session is attached: the
library's raw `rgba8888` frame is encoded as a PNG at the app's native pixel size
and returned as `image/png`, with `region` meaning the same crop rectangle in
app pixels. Every other call, including `sdl_terminate`, is forwarded verbatim.
When the peer is itself a supervisor — a spawner — its answer is already the
final `content` result with the image block, so the relay passes that through
unchanged instead of encoding it a second time; the frame keeps the dimensions
and format the hop nearest the application reported.

### Stopping the relay

Client stdin EOF, SIGINT, SIGTERM, and a normal return all close the listener and
the attached session and exit. A relayed application is not a child: the relay
never signals it, so it keeps running after the relay goes away.

### Transport

The MCP client keeps talking stdio to `sdl3mcp`; the relay is the only process
that owns the client's pipes. Everything below the relay is a TCP socket. With a
spawner in between there are two hops and both are sockets: the spawner dials the
relay with `--connect`, and each application it starts dials the spawner with the
`--mcp=HOST:PORT` endpoint it was given. That split is what lets the application
run outside the client's container: publish the relay's port, forward it over
SSH, or map it to a device with `adb reverse`, and the app on the other side
dials in with its own `--mcp=`.

The relay does no authentication and no TLS. Bind it to loopback or expose it
only on a network you trust; anyone who can reach the port can drive the
application that connects.

### Relay client and application configuration

A client points at the relay where it used to point at the supervisor, and the
application is started separately with the relay's address:

```json
{
  "mcp": {
    "servers": {
      "sdl3mcp": {
        "type": "local",
        "command": ["build/sdl3mcp", "--listen=127.0.0.1:7333"]
      }
    }
  }
}
```

```sh
# On the machine or device that runs the app:
./build/examples/minimal --mcp=relay-host:7333
```

## Spawner

A relay waits for an application that somebody else started. The **spawner**
closes that gap on the application's side: `sdl3mcp --connect=HOST:PORT
--targets=PATH` dials a relay, offers one spawn tool per configured application,
and starts the chosen one on demand, handing it a listener of its own to dial
back to.

```text
client ──stdio──▶ relay (--listen) ◀──TCP── spawner (--connect --targets)
                                                 │ spawn, then listen
                                                 ▼
                                        app (--mcp=<spawner endpoint>)
```

The relay sees the spawner as its one application session and merges the
spawner's entries over its own list, so the client sees `wait_session`, the four
application tools, and one `app_spawn_<alias>` per target. The spawner's list
does not change when an application starts or exits; it always advertises the
`app_spawn_*` set alongside the application block, and the relay forwards an
application call through to the live child.

### Spawner command line

```text
sdl3mcp --connect=HOST:PORT --targets=PATH [--targets=PATH ...] [--listen=HOST:PORT]
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--connect=HOST:PORT` | *(required)* | Relay to dial. A failed dial is fatal: the spawner prints `--connect:` with the endpoint and exits non-zero. |
| `--targets=PATH` | *(required)* | A JSON file or a directory of `*.json` files listing the applications the spawner may start. Repeatable; every source contributes entries to one alias map. |
| `--listen=HOST:PORT` | `127.0.0.1:0` | The address and port a spawned application dials back to. `[V6]:PORT` is accepted. |

`--connect` must be given once, and `--targets` at least once (it may repeat);
neither can be combined with `--target`. Without `--listen` the spawner binds
`127.0.0.1:0`, so the kernel picks a port and only that machine can reach it.
With an explicit `--listen` a bind failure fails the spawn; it never silently
falls back to an ephemeral port.

### Targets sources

Each source is either a file or a directory:

- a regular file is a JSON object, read as one source;
- a directory contributes every `*.json` regular file at its top level, read in
  sorted filename order so the resulting menu is deterministic. Other files are
  ignored, and a directory without any `*.json` file is an error.

All sources accumulate into one map. That is what makes generated files usable:
each build tree writes its own targets file, so two trees are combined by
repeating the flag:

```sh
sdl3mcp --connect=HOST:PORT \
  --targets=build/vulkan/sdl3mcp-targets.json \
  --targets=build/opengl/sdl3mcp-targets.json \
  --targets=./extra-targets
```

An alias may be defined by only one source. Seeing the same alias in two files
is a startup error that names both paths; the later one never replaces the
earlier one, because that would silently make one of the two binaries
unreachable.

In a file, each key is an alias and each value is the application's argument
vector, executable first, arguments after it:

```json
{
  "demo": ["./build/examples/minimal"],
  "probe": ["/opt/probe/probe", "--fast", "--no-audio"]
}
```

The spawner appends `--mcp=HOST:PORT` as the last argument, naming the listener it
just bound, so an entry must not already carry an MCP argument.

Validation happens at startup and a failure is fatal before any socket or child
exists; the error names the offending path, file, or entry:

- every path must exist and every file must open, inside a directory included;
- each file must parse as JSON;
- each file's top level must be a non-empty JSON object;
- every key must match `[a-z0-9_]+` and must not begin with the reserved
  `app_spawn_` prefix;
- every value must be a non-empty array of strings;
- no element may contain `--mcp` anywhere, because the spawner supplies it;
- the accumulated map must not be empty;
- an alias may not appear in two sources.

### Generating the targets file

A project that builds its applications against SDL3MCP can register them in
CMake instead of hand-writing the file. `sdl3mcp_app(<alias> <target> [ARGS
<arg>...])` records an executable target and writes the targets file at generate
time:

```cmake
add_executable(minimal minimal/main.c)
target_link_libraries(minimal PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3)
sdl3mcp_app(minimal minimal)
sdl3mcp_app(probe probe ARGS --fast --no-audio)
```

- The alias is required and must match `[a-z0-9_]+`; the reserved `app_spawn_`
  prefix is rejected, because the spawner derives the tool name from it.
- The target must exist, and an alias may be registered once per configure: a
  duplicate fails at configure time with a message naming both targets.
- The entry's first element is `$<TARGET_FILE:<target>>`, so the generated file
  carries absolute paths resolved by the generator.
- The file lands at `${CMAKE_BINARY_DIR}/sdl3mcp-targets.json`, the top-level
  build directory of the configure.
- Registration only appends to a global property. The write is deferred to the
  end of the top-level directory, so applications registered from different
  subdirectories all end up in one file and no CMakeLists calls the writer
  itself. With no registration the file is never written.
- Extra `ARGS` elements become the entry's trailing arguments; quotes and
  backslashes are escaped for JSON. The `--mcp` rule above still applies, and
  the spawner rejects such an entry at startup.

The submodule's own `examples/CMakeLists.txt` registers the `minimal` example
this way, so configuring with `SDL3MCP_BUILD_EXAMPLES=ON` and building the
example produces `build/sdl3mcp-targets.json` with a single `minimal` entry.

Each configure writes its own file, so a spawner that should offer applications
from several build trees passes them as repeated `--targets` values, or points
`--targets` at a directory holding them. Generated entries carry absolute paths,
so mixing a generated file with a hand-written one is safe.

### Tool view and spawning

`tools/list` is one `app_spawn_<alias>` per targets entry plus the four
application tools, in every state. The set does not change when an application
is spawned or exits, so the spawner sends no
`notifications/tools/list_changed` around a spawn. Before one is attached, an
application `tools/call` fails with
`no application is attached; call one of the app_spawn_* tools`.

One application runs at a time. Calling another `app_spawn_<alias>` while one is
attached fails with `an application is already attached; one spawn at a time`;
wait for the running application to exit first.

```json
{
  "name": "app_spawn_demo",
  "description": "Start the application registered as \"demo\" and wait until it connects back and completes the MCP handshake. Arguments: optional timeout_ms (1-60000, default 5000) covering bind, exec, dial-back and the handshake. Returns the child's process id; its tools are advertised alongside this one, and call app_spawn_<alias> again only after it has exited. Example: {\"timeout_ms\":5000}",
  "inputSchema": {
    "type": "object",
    "properties": {
      "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000, "default": 5000 }
    },
    "additionalProperties": false
  },
  "annotations": {
    "readOnlyHint": false,
    "destructiveHint": false,
    "idempotentHint": false,
    "openWorldHint": false
  }
}
```

`timeout_ms` defaults to 5000 and covers the whole spawn — bind, fork/exec, the
application dialing back, and its `initialize` and `tools/list` handshake — not
just the connection. `app_spawn_<alias>` has three outcomes:

1. **Success.** The application connected and completed the handshake, and its
   session is still live when the answer is written. The result is
   `spawned <alias> (pid N)`; the tool list is unchanged and application calls
   now forward through to the child.
2. **The application died first.** It exited, or could not be executed, before it
   connected. The `isError` result names the exit status or `exec` failure and
   the pid; the tool list is unchanged.
3. **Timeout.** `timeout_ms` expired before the handshake completed. The error
   names the timeout and the pid, and the spawner terminates the child (SIGTERM,
   about two seconds of grace, then SIGKILL) before returning, so a failed spawn
   leaves no process behind.

A bind or `exec` failure is reported like the second outcome, with the reason in
the error text. After any failure the spawner is idle again and a later
`app_spawn_<alias>` may be tried.

### Terminating an application

`sdl_terminate` is forwarded like any other application tool; the spawner does
not wait for the exit or force it, and the application decides what the request
means. When the application exits, whether from a clean request or on its own,
the spawner returns to Idle with the same list it always advertised, so nothing
is announced. A forwarded call to an application that stopped answering is
bounded the same way as the relay's: it returns an error naming the tool and
recovers the session.

### Stopping the spawner

The spawner lives exactly as long as its relay connection: when the relay closes
the socket, the run loop ends and the spawner exits. On the way out it stops a
live application with SIGTERM, a grace period, and SIGKILL, reaps it, and only
then exits; SIGINT and SIGTERM take the same path. Closing the relay therefore
never leaves an orphan application behind. The spawned application does not
inherit the relay connection or the spawn listener, so the relay sees the socket
close as soon as the spawner dies, even if the application is still running.

### Spawner client configuration

The client points at the relay, not at the spawner. This is a local development
pair: a relay on 7333 and a spawner on the same machine.

```json
{
  "mcp": {
    "servers": {
      "sdl3mcp": {
        "type": "local",
        "command": ["build/sdl3mcp", "--listen=127.0.0.1:7333"]
      }
    }
  }
}
```

```sh
build/sdl3mcp --connect=127.0.0.1:7333 --targets=./targets.json
```

## Public API

The complete public surface:

```c
SDL3MCP_Configuration SDL3MCP_DefaultConfiguration(void); /* header-inline */
SDL_AppResult SDL3MCP_AppInit(SDL3MCP_Configuration *config, int argc, char **argv);
SDL_AppResult SDL3MCP_SetWindow(SDL_Window *window);
SDL_AppResult SDL3MCP_AppIterate(void);
void          SDL3MCP_AppShutdown(void);
bool          SDL3MCP_FrameRequested(void);
void          SDL3MCP_WriteFrame(const uint8_t *data, size_t size, int width, int height);
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
- Starts the server thread when the resulting transport is `STDIO`, or dials the
  configured endpoint and starts it for `CONNECT`; with `NONE` it records the
  configuration and does nothing else.
- Returns `SDL_APP_CONTINUE` on success, `SDL_APP_SUCCESS` for
  `--mcp-help`/`--mcp-version`, and `SDL_APP_FAILURE` for invalid arguments, a
  broken stdin/stdout, an unreachable `CONNECT` endpoint, or a thread that cannot
  start.
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
  `SDL_GetWindowSizeInPixels` for the reported pixel size, published to the
  server thread once per `SDL3MCP_AppIterate`;
- **event routing** — synthetic events carry this window's `windowID`;
- **text input and cursor** — `SDL_TextInputActive`, `SDL_StartTextInput`,
  `SDL_StopTextInput`, `SDL_WarpMouseInWindow`, and
  `SDL_GetWindowRelativeMouseMode`.

App thread only. Returns `SDL_APP_FAILURE` if `SDL3MCP_AppInit` was not called
or the window has no drawable area yet; a window that is later minimized is fine
and only affects what the app can read back, not frame validation. Rebinding
resets held keys, held buttons, and the virtual cursor, so nothing crosses
windows.

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
void SDL3MCP_WriteFrame(const uint8_t *data, size_t size, int width, int height);
```

Hands a rendered frame to a waiting capture. There is no pitch or stride
parameter: the buffer must already be packed. `width` and `height` are the
frame's own dimensions, so the pixels may come from any render target and need
not match the bound window's drawable size.

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
- **Size:** `width * height * 4`, where `width` and `height` are the dimensions
  of the pixels the app passes in. The library never measures the window, so a
  readback of a smaller render target, a region, or an offscreen buffer is
  accepted as long as the byte count matches.
- **Guard:** `size <= max_frame_bytes`; larger frames are dropped.
- **Frame stamp:** the accepted frame is stamped with the index of the frame
  whose pixels it holds. The app frame counter advances in `SDL3MCP_AppIterate`
  at the end of each frame, so during the iteration that renders the pixels the
  counter still names the previous one and the stamp is that counter plus one.
  The raw header carries the index as `frame`, and the region crop preserves it.
- The pixels are copied before the function returns, so the app can reuse or
  free its buffer immediately. The server thread crops the optional region and
  sends raw pixels; PNG encoding and base64 are the supervisor's job and never
  touch the render thread.
- Call it only when `SDL3MCP_FrameRequested()` is true. Calls with no pending
  capture are ignored.
- App thread only, because it reads the pending capture state.

The function returns nothing on purpose: a capture is a request that can fail,
and the error belongs to the client's `sdl_capture` call, not to the render
loop.

## Frame pipeline

Capture is pull-based because the library has no idea how the app draws. It
cannot read a swapchain from another thread, and it does not want to know
whether the app uses SDL_Renderer, OpenGL, Vulkan, Metal, or software. So it
asks:

1. The client calls `sdl_capture`, or `sdl_input` once its batch has been
   acknowledged.
2. The server validates the arguments, checks that a window is bound, marks a
   request pending, and waits up to `timeout_ms`.
3. On the next frame, the app's `SDL3MCP_FrameRequested()` returns true.
4. The app renders, reads its own pixels, and calls `SDL3MCP_WriteFrame`.
5. `SDL3MCP_WriteFrame` validates the byte count against the width and height
   the app supplies, copies it, clears the pending request, and wakes the server
   thread.
6. The server thread crops the optional region and answers with one result line,
   `{"format":"rgba8888","width":W,"height":H,"byteLength":B}`, immediately
   followed by exactly `B` raw RGBA bytes and nothing else.
7. The app continues its loop; nothing blocked.

A direct library client receives those raw bytes. When the app runs behind
`sdl3mcp`, the supervisor reads the payload, encodes it as PNG at
exactly the dimensions the app sent, base64-encodes it, and returns the usual
`image/png` content block. Captures are never resized, so image pixels map 1:1
to app pixels. That split keeps stb and every image decision out of the library
while the client-facing image contract stays stable.

Rules that fall out of this design:

- The returned frame is one written **after** the request arrived; stale frames
  are never served.
- A capture requested before the application has rendered its first frames is
  not answered by the first rendered frame. It stays pending, and the first frame
  the library is willing to serve answers it: the request waits rather than
  failing, and it is never cancelled or rejected for arriving early. The first
  rendered frame carries no draw data, so a frame taken from it would show only
  the clear colour. `kFirstServableFrame` in `src/capture.hpp`, applied by
  `servableFrame()` in `src/capture.cpp`, is the single place that decides that
  bound; this document deliberately does not restate its value.
- Only one capture is pending at a time. Tool calls are serialized on the server
  thread, so a slow capture delays the next tool call until it completes or
  times out. `sdl_input` uses the same pipeline for the frame it returns, so an
  action and its observation are one call.
- A frame is dropped when `size` is not exactly `width * height * 4` for the
  supplied dimensions, when the data pointer is null, when a dimension is not
  positive, or when the frame exceeds `max_frame_bytes`. A drop leaves the
  request pending so the app can retry; the last reason is appended to the
  timeout message as "Last drop: ..." and is visible in `sdl_info` as
  `capture.lastError` with `capture.dropped` counting
  the attempts.
- `sdl_capture` fails immediately with a clear error if no window is bound, if
  the region does not fit inside the frame, or if the app never answers in time.
  A timeout is a normal content result with `isError: true`.
- The library does only the crop on the server thread: no resize and no image
  encoding happen in the app process. An `sdl_capture` result is one framed
  header plus its pixel payload, with no base64 and no newline inside the frame.
- The supervisor encodes the cropped frame unchanged. It has no resizing option:
  image pixels map 1:1 to app pixels, so coordinates measured in the returned
  PNG are app coordinates and colours are never averaged. `region` is the only
  lever for payload size.
- Through the supervisor, base64 adds about 33% to the PNG and the whole image
  travels as one JSON content block. A 960×540 PNG is typically 40–120 KB, or
  ~55–160 KB on the wire. A direct library client instead pays
  `width * height * 4` bytes for the raw frame.

## Input model

`sdl_input` takes an ordered array of 1–256 actions. The batch is validated as a
whole, then queued as one unit and applied in a single `SDL3MCP_AppIterate`, so
no frame boundary can appear in the middle of a gesture.

| Action | Fields |
| --- | --- |
| `key` | `key` (name or chord), `action` = `down`/`up`/`press` (default `press`), optional `modifiers`. |
| `text` | `text` (UTF-8, 1–1024 bytes). |
| `mouse_move` | `x`,`y` (window points) **or** `dx`,`dy` (relative), never both and never half a pair. |
| `mouse_button` | `button` = `left`/`middle`/`right`/`x1`/`x2`, `action` = `down`/`up`/`click`, optional `x`,`y` (moves the cursor there first), `clicks` 1–3. |
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
  malformed chord, an unknown chord modifier, or SDL refusing to start text
  input on the bound window therefore fails the `sdl_input` call when it returns
  at frame F+1, as an `isError` result.
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
requires only a bound window: when text input is not active on that window the
library starts it for the batch and restores the previous state once the events
are queued, so a window that lost focus no longer fails. Only a window SDL
refuses to start text input on still produces the `text input is not active on
the bound window` error.

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
- A `mouse_button` with `x`/`y` performs the same absolute move first, in the
  same action: one synthetic motion event at the clamped target, the warp under
  `warp_cursor`, and then the button events. That makes a click on a UI item a
  single action instead of `mouse_move` followed by `mouse_button`, which
  matters because event-driven consumers such as ImGui's SDL3 backend take the
  pointer position from `SDL_EVENT_MOUSE_MOTION` and ignore a button event's own
  coordinates. Without `x`/`y` the action emits only its button events and never
  warps.
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

SDL3MCP speaks MCP over stdio or a dialed socket: UTF-8 JSON-RPC 2.0, one
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
  tool call delays every later request: `sdl_input` waits up to its
  `timeout_ms` budget for the F+1 acknowledgement and then for the frame, and
  `sdl_capture` waits up to its timeout for a frame, so a `ping` queued behind
  one of them is answered only after that call returns. Nothing is cancelled by
  the delay.
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
| `sdl_terminate` | `false` | `false` | `false` |

`sdl_terminate` additionally sets `destructiveHint: true`, because asking the
app to quit ends the session.

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
    "lastBytes": 8294400,
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
- `capture.pending` is true while a frame request waits; `count` is
  successful frames handed to a client (a `sdl_capture`, or the frame
  `sdl_input` returned); `dropped` counts rejected `SDL3MCP_WriteFrame`
  attempts.
  `lastWidth`/`lastHeight` describe the frame the library returned, after any
  region crop, and are the dimensions of the PNG the supervisor encodes,
  `lastBytes` is the raw RGBA payload size (`width * height * 4`), `lastMs`
  covers the wait for the frame plus the crop, and `lastError` is `null` when
  the last frame request succeeded (or none ran), otherwise the last drop or
  capture error string.
- `limits` repeats the effective `frame_timeout_ms` and `max_frame_bytes`.
- `frame` is the library's frame counter and `appliedFrame` is the last frame at
  which an input batch was applied (0 when none was).

When no window is bound, `window` is `null` and the tool still succeeds, which
makes it a useful first call after startup.

### `sdl_input`

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

The result carries both halves of the observation in one message: the result
line names the frame and the payload follows it exactly as it does for
`sdl_capture`.

```text
← {"jsonrpc":"2.0","id":3,"result":{"format":"rgba8888","width":960,"height":540,"byteLength":2073600,"frame":14,"text":"1 actions applied at frame 12; frame 14 attached, rendered after the actions were delivered"}}
<2073600 raw RGBA8888 bytes; no newline, no base64>
```

- `text` names both the frame the batch was applied at and the frame attached.
  The two are always separated by at least one frame because the acknowledgement
  waits for the F+1 completion; the attached frame is rendered after the actions
  were delivered, so an unchanged frame means they had no effect. The gap is not
  fixed, so do not read a constant delta from the example.
- `frame` in the header is the appended stamp, so a client that ignores `text`
  still knows which frame the payload holds.
- `region` crops the returned frame with the same rules and the same parser
  `sdl_capture` uses; the framed dimensions are the region's. It is in pixels of
  the full frame, while the action coordinates above are window points: use
  `window.pixelWidth`, `window.pixelHeight` and `window.pixelDensity` from
  `sdl_info` to convert between them.
- `timeout_ms` bounds the whole call: the wait for the F+1 acknowledgement plus
  the wait for the frame. It defaults to twice `frame_timeout_ms` and is clamped
  to the published 1–60000 range.
- An input batch the app rejected, or a key it could not resolve, still returns
  an `isError` content result with no frame.
- When the input applies but no frame arrives in the remaining budget, the
  result is an `isError` content result whose text names the applied frame and
  why the frame is missing; the input is never reported as failed.

Structural errors fail the call before queueing; key and chord errors fail it at
F+1. See [Validation and error timing](#validation-and-error-timing).

### `sdl_capture`

The library's own schema returns raw pixels and advertises only `region` and
`timeout_ms`: presentation is supervisor policy.

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

- `region` is a crop in **captured-frame pixels** — the `capture.lastWidth` ×
  `capture.lastHeight` reported by `sdl_info` — with a top-left origin. All four
  fields are required, `x`/`y` must be at least 0, `width`/`height` at least 1,
  and the rectangle must fit inside the frame. It is applied by the library.
  `sdl_input` coordinates are **window points**, so they are not the same space
  unless `pixelDensity` is 1.0: `sdl_info` reports `window.pixelWidth`,
  `window.pixelHeight` and `window.pixelDensity` so a caller can convert.
- `timeout_ms` is 1–60000 and defaults to `frame_timeout_ms`.
- Any other property is rejected before a frame is requested.

A successful call is **one JSON-RPC result line followed by raw bytes**, not a
content array:

```text
← {"jsonrpc":"2.0","id":4,"result":{"format":"rgba8888","width":640,"height":360,"byteLength":921600}}
<921600 raw RGBA8888 bytes; no newline, no base64>
```

- `format` is always `"rgba8888"`: bytes R, G, B, A, top-left origin, row-major,
  stride exactly `width * 4`.
- `byteLength` is exactly `width * height * 4`, and exactly that many bytes
  follow the result line before the next JSON-RPC message.
- `width` and `height` describe the frame that was sent: the region when one was
  given, otherwise the dimensions the app passed to `SDL3MCP_WriteFrame`.
- `sdl_input` uses the same framing for the frame it returns, and adds its own
  `text` to the result line; `sdl_capture` never has one.
- A client that wants a self-contained image should go through the supervisor,
  which turns this framing into the standard `image/png` block. See
  [Capture through the supervisor](#capture-through-the-supervisor).

On timeout the result is an ordinary content error, not a frame, and names the
last drop reason when there was one:

```json
{
  "content": [
    { "type": "text", "text": "no frame arrived within 2000 ms. The app must call SDL3MCP_WriteFrame() when SDL3MCP_FrameRequested() returns true, with tightly packed RGBA8888, its width, and its height. Last drop: size 1036800 != 960*540*4." }
  ],
  "isError": true
}
```

### `sdl_terminate`

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

No arguments. The tool sets a flag and returns the text `termination requested`
with `isError: false`. It does not call SDL, and it does not end the process:
the app decides what a quit means.

On the next `SDL3MCP_AppIterate`, after the usual batch and snapshot work, the
library exchanges the flag and pushes exactly one zeroed `SDL_EVENT_QUIT` with
`SDL_PushEvent`. If the push fails, the flag is restored and the next iterate
retries; either way `SDL3MCP_AppIterate` still returns `SDL_APP_CONTINUE`, so
the app's own quit handling runs unchanged. A second iterate with no new
request pushes nothing.

### Example session

```text
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"agent","version":"1.0"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"sdl3mcp","version":"0.1.0"}}}
→ {"jsonrpc":"2.0","method":"notifications/initialized"}
→ {"jsonrpc":"2.0","id":2,"method":"tools/list"}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[ ... sdl_info, sdl_input, sdl_capture, sdl_terminate ... ]}}
→ {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"sdl_input","arguments":{"actions":[{"type":"key","key":"right","action":"down"},{"type":"key","key":"space","action":"press"}]}}}
← {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"2 actions applied at frame 7; frame 9 attached, rendered after the actions were delivered"}],"isError":false}}
→ {"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"sdl_capture","arguments":{"region":{"x":0,"y":0,"width":640,"height":360}}}}
← {"jsonrpc":"2.0","id":4,"result":{"format":"rgba8888","width":640,"height":360,"byteLength":921600,"frame":11}}
← 921600 raw RGBA8888 bytes follow, with no newline and no base64
→ {"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"sdl_input","arguments":{"actions":[{"type":"key","key":"right","action":"up"},{"type":"key","key":"escape","action":"press"}]}}}
← {"jsonrpc":"2.0","id":5,"result":{"content":[{"type":"text","text":"2 actions applied at frame 11; frame 13 attached, rendered after the actions were delivered"}],"isError":false}}
→ {"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"sdl_terminate","arguments":{}}}
← {"jsonrpc":"2.0","id":6,"result":{"content":[{"type":"text","text":"termination requested"}],"isError":false}}
```

The supervisor keeps the client-facing contract an image: the same session
through `sdl3mcp` ends the capture at
`← {"jsonrpc":"2.0","id":4,"result":{"content":[{"type":"text","text":"1920x1080 PNG, 91234 bytes; frame 42 attached"},{"type":"image","mimeType":"image/png","data":"iVBORw0KGgo..."}],"isError":false}}`.
The PNG is the app's native 1920x1080 frame, never resized, so a smaller
`region` is the only way to shrink it.

## Threading and lifetime

| Thread | May touch | Must not touch |
| --- | --- | --- |
| App thread | SDL, the window, the renderer, all `SDL3MCP_*` functions. | — |
| Server thread | the transport's fds (stdio duplicates or the dialed socket), JSON, the copied frame buffer, the input queue. | Any SDL function, including `SDL_GetWindowSize`. |

Rules that make this safe:

1. **One owner per piece of state.** Window geometry is published by the app
   thread into a snapshot; the server thread only reads it.
2. **Input travels one way.** Tool calls append immutable action batches to a
   queue; `SDL3MCP_AppIterate` drains and applies them on the app thread.
3. **Frames travel one way.** `SDL3MCP_WriteFrame` copies pixels under a mutex
   and signals a condition variable; the crop happens after the copy, and the
   supervisor's encoding happens in a different process.
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

stdio-first is a small attack surface by construction:

- The library opens **no listening socket**, no port, and no authentication. In
  its stdio mode the only way in is to start the app with `--mcp` (or a
  configuration that enables it) and own its stdin/stdout. In `CONNECT` mode it
  dials exactly the endpoint it was configured with and still listens on
  nothing.
- The trust boundary is the process boundary. A client that can launch the app
  can already run it; SDL3MCP adds the ability to send input and read frames,
  not to read files or spawn processes.
- Frames come from your app's own rendering and may contain anything the app
  displays. Treat them as you would a screenshot of that window.
- Everything runs with the app's privileges. Do not enable the server in a
  privileged or setuid context.

The optional `sdl3mcp` executable is the one part that listens. Its relay mode is
plain TCP with no authentication and no TLS, so anyone who can reach the port can
drive whichever application connects; bind it to loopback, use a private network,
or tunnel the port rather than exposing it.

Two process-level rules the app must respect when the stdio transport is in use:

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
`SDL3MCP_WriteFrame` with exactly `width * height * 4` bytes and the width and
height of what it read back, before `SDL_RenderPresent`. Read the appended
"Last drop" reason and `sdl_info`'s `capture.lastError`. Common causes: reading
pixels after `SDL_RenderPresent` (do it before), passing a surface with row
padding, a width/height that does not match the buffer, or a render target that
produced no pixels this frame.

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

**Payloads are too large.** Captures are never resized, so crop with `region`:
a smaller region is a smaller PNG. Base64 adds about a third to the PNG size. A
direct library client receives raw RGBA at `width * height * 4` bytes per
capture, so keep `region` small and lower `max_frame_bytes` if you want a hard
ceiling.

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
- One MCP client per process. Stdio between the client and `sdl3mcp`; the relay
  and spawner add plain TCP below it with no TLS or authentication.
- MCP `2026-07-28` and later stateless revisions are not implemented; clients
  must use the handshake-based revision.
- Frames are raw RGBA8888 from the library; PNG is only available through the
  supervisor, one full frame per capture, no diffs or video.
- No window management (resize, move, title), no gamepad, touch, or audio.
- Capture requires cooperation from the app; the library cannot read a
  swapchain on its own.
- POSIX only for now: the transport and fd isolation use `poll`, `dup2`, and
  `/dev/null`. Windows support is a TODO.
- `log_level` is validated but not yet connected to a leveled logger.

Plausible next steps: `sdl_window` for resize/title/fullscreen, gamepad and
touch actions, frame diffs, and a `sdl3mcp://frame/latest` resource. Anything
added has to keep the six-call wiring and the "library never touches your
renderer" rule intact.

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
src/frame.*                        frame validation and region crop
src/capture.*                      pending capture state and statistics
src/transport.*                    line transport plus raw frame read/write
src/socket_connect.*               the app's --mcp=HOST:PORT dial
src/server.*                       server thread loop
src/stdio_isolation.*              fd duplication and redirect
third_party/stb/                   vendored stb_image_write.h (supervisor only)
third_party/nlohmann/              vendored json.hpp fallback
examples/minimal/main.c           the quick-start app
supervisor/                        sdl3mcp: CLI, peers, child process, targets, proxy
supervisor/peer.hpp                the data-path seam: stdio, accepted, or dialed
supervisor/png.*                   PNG encode and base64
supervisor/stb_image_write_impl.cpp
                                   the one stb implementation translation unit
tests/                             unit and integration tests, check_readme.py
CMakeLists.txt
```

Test suite (all registered with CTest): `header_c`, `header_cpp`,
`configuration_tests`, `keymap_tests`, `input_tests`, `input_json_tests`,
`replay_tests`, `mouse_tests`, `queue_tests`, `app_input_tests`,
`input_tool_tests`, `capture_tests`, `capture_tool_tests`, `terminate_tests`,
`json_rpc_tests`, `session_tests`, `frame_tests`, `info_tests`, `server_tests`,
and six `lifecycle_*` scenarios. With `SDL3MCP_BUILD_CLI=ON` it also adds
`supervisor_png_tests` for the moved presentation code, `supervisor_tests`,
which drives the real supervisor binary against a fake MCP child, `relay_tests`,
which drives `--listen` against a fake application over TCP, `spawner_tests`,
which drives `--connect --targets` against a relay the test owns, and
`supervisor_real_app`, which repeats the round trip through
`examples/minimal` under `SDL_VIDEODRIVER=dummy`. `connect_tests` covers the
app's dialed `--mcp=HOST:PORT` transport. When Python 3 is available,
`readme_check` runs `tests/check_readme.py`: it compiles both complete C
examples, parses every JSON block, and checks both `sdl_capture` schemas, the
example run path, the supervisor states, and the loop order. It also loads the
catalog the built supervisor test binary dumps with `--dump-tools` and requires
every tool description (in the catalog and in the README's tool documents) to
carry a parseable `Example:` JSON object and to name each argument its schema
accepts.

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
