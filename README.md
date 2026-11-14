# SDL3MCP

SDL3MCP is an MCP server for SDL3 applications, in two pieces:

- the **`SDL3MCP` library**, linked into the application. It dials one MCP
  client and answers four tools on the application's own thread. It owns no
  window, no thread, and no descriptor of its own.
- the **`sdl3mcp` Handler**, a standalone Linux binary. It speaks MCP to a
  client (stdio, or an upstream socket in Relay mode), accepts one application
  at a time on a listener, presents captures as PNG, and can spawn applications
  on demand from `--targets` and `--web` sources.

The application keeps its own loop. It answers frame requests, pumps the library
once per frame, and the library turns that pump into protocol work. There is no
server thread and no locking: every library call runs on the app thread.

This is the 1.0 release. `CHANGELOG.md` names what broke from v0;
`CONTRACT.md` is the frozen v0 behaviour record and is kept as an archive, not
as the current contract.

## Contents

- [Requirements](#requirements)
- [Build](#build)
- [Quick start](#quick-start)
- [The frame loop](#the-frame-loop)
- [Running an application](#running-an-application)
- [The library API](#the-library-api)
- [Tools](#tools)
- [Input actions](#input-actions)
- [Capture framing](#capture-framing)
- [The Handler](#the-handler)
- [The browser](#the-browser)
- [Platforms and limits](#platforms-and-limits)
- [Tests](#tests)
- [Versioning](#versioning)

## Requirements

- **SDL 3.2 or newer.** The library is built against the 3.2 API:
  `SDL_RenderReadPixels(renderer, rect)` returns an `SDL_Surface *`.
- **Linux.** The Handler is POSIX-only (one `poll` loop, `signalfd`, `pidfd`,
  `fork`/`exec`). A Windows configure stops with a `FATAL_ERROR`; the Windows
  work is a TODO in `src/SocketStream.cpp` and `src/StdioStream.cpp`.
- **C11 or C++20** for a consumer. The library itself is C++20 and its headers
  are self-contained.
- **nlohmann/json** at build time, resolved by this project's dependency
  policy: an existing `nlohmann_json::nlohmann_json` target, then
  `find_package(nlohmann_json CONFIG)`, then a pinned `v3.12.0` download. It is
  a private, header-only dependency: a consumer never needs it installed.
- **doctest** for the test suite, **Python 3** for `readme_check`, and
  **Node.js** plus Emscripten for the optional WebSocket suite.

## Build

The direct flow:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSDL3MCP_BUILD_CLI=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`SDL3MCP_BUILD_CLI=ON` builds the Handler and defines `SDL3MCP::SERVER`, so this
flow also builds and tests `sdl3mcp`. Without it the library and the tests still
build, and the Handler suites are skipped.

`CMakePresets.json` wraps the same configurations:

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
```

Both presets turn on the tests and the Handler. `asan` runs
with leak detection on; the tests that use SDL's dummy video driver scope it off
for themselves because the driver keeps process-wide allocations they cannot
free.

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `SDL3MCP_BUILD_TESTS` | `ON` when top-level | Build the doctest suites. |
| `SDL3MCP_BUILD_CLI` | `OFF` | Build the `sdl3mcp` Handler. |
| `SDL3MCP_BUILD_NODE_TESTS` | `OFF` | Build the Emscripten WebSocket suite (needs Node). |

### Targets

| Target | What it is |
| --- | --- |
| `SDL3MCP::SDL3MCP` | The library: the SDL-free protocol core plus the app half, as a static library. This is the only target the installed package exports. |
| `SDL3MCP::SERVER` | The `sdl3mcp` Handler executable. Defined exactly when `SDL3MCP_BUILD_CLI=ON` on a desktop configure; there is no Emscripten Handler. |
| `SDL3MCP::COMMON` | The SDL-free core both compose as a static library: JSON-RPC, session, tools, stream, configuration, dial. |

The object libraries behind them (`sdl3mcp_common_objects`,
`sdl3mcp_app_objects`, `sdl3mcp_server_objects`) are internal. **The library is
static only and publishes no SDL:** it carries no SDL target, no SDL include
path and no linkage macro. A consumer links `SDL3MCP::SDL3MCP` plus the SDL
target it chooses on its own link line:

```cmake
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(app PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3-static)
```

Name `SDL3::SDL3-static`, not plain `SDL3::SDL3`: the plain alias resolves to
whichever SDL the environment configured, and a shared SDL conflicts with a
static app. On Emscripten the final link also needs `-lwebsocket.js`, because
the library's WebSocket arm calls `emscripten_websocket_*`:

```cmake
target_link_libraries(app PRIVATE SDL3MCP::SDL3MCP SDL3::SDL3-static "-lwebsocket.js")
```

### The Emscripten build

```sh
emcmake cmake -S . -B build/web -DSDL3_DIR=/path/to/an/emscripten/sdl3 \
  -DSDL3MCP_BUILD_NODE_TESTS=ON
cmake --build build/web --parallel
ctest --test-dir build/web --output-on-failure
```

`SDL3MCP_BUILD_NODE_TESTS=ON` adds the WebSocket suite, which CTest runs through
Node; without Node it is skipped with a warning.

A page that links the library must add `-lwebsocket.js` to its own final link,
as §Targets shows: the library no longer carries it, and the flag is what
satisfies the `emscripten_websocket_*` calls.

The browser build compiles `src/WebSocketStream.cpp` in place of
`src/SocketStream.cpp` and `src/StdioStream.cpp`. It accepts only `--mcp=off`
and `--mcp=HOST:PORT`; there is no stdio and no `--mcp-help` or `--mcp-version`,
because a page has no command line to reach them.

## Quick start

The program below is a complete host: a window whose background an agent can
change with synthetic key presses and then see in a capture. This is the whole
file.

```c
/*
  The smallest complete SDL3MCP host: a window whose background an agent can
  change with synthetic key presses and then see in a capture.
*/

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3MCP.h>

typedef enum BackgroundMode {
   BACKGROUND_DEFAULT,
   BACKGROUND_RED,
   BACKGROUND_GREEN,
   BACKGROUND_BLUE
} BackgroundMode;

typedef struct App {
   SDL_Window* window;
   SDL_Renderer* renderer;
   BackgroundMode background;
} App;

/* Pressing the active color key again returns to the default background. */
static BackgroundMode toggledBackground(BackgroundMode current, BackgroundMode target) {
   return current == target ? BACKGROUND_DEFAULT : target;
}

static void setBackgroundColor(App* app) {
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
static void writeFrame(App* app) {
   SDL_Surface* frame = SDL_RenderReadPixels(app->renderer, NULL);
   if (!frame) {
      return;
   }

   SDL_Surface* rgba = frame;
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
      uint8_t* packed = SDL_malloc(size);
      if (packed) {
         for (int y = 0; y < rgba->h; ++y) {
            SDL_memcpy(packed + rowBytes * (size_t)y,
                       (const uint8_t*)rgba->pixels + (size_t)rgba->pitch * (size_t)y,
                       rowBytes);
         }
         SDL3MCP_WriteFrame(packed, size, rgba->w, rgba->h);
         SDL_free(packed);
      }
   }

   SDL_DestroySurface(rgba);
}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
   /* SDL3MCP_AppInit runs first so --mcp-help and --mcp=off can exit early. */
   SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();
   SDL_AppResult result = SDL3MCP_AppInit(&configuration, argc, argv);
   if (result != SDL_APP_CONTINUE) {
      return result;
   }

   if (!SDL_Init(SDL_INIT_VIDEO)) {
      return SDL_APP_FAILURE;
   }

   App* app = SDL_calloc(1, sizeof *app);
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

SDL_AppResult SDL_AppIterate(void* appstate) {
   App* app = appstate;

   setBackgroundColor(app);
   SDL_RenderClear(app->renderer);

   if (SDL3MCP_FrameRequested()) {
      writeFrame(app);
   }

   SDL_RenderPresent(app->renderer);

   /* Last, so this frame's capture and input bookkeeping stay in step. */
   return SDL3MCP_AppIterate();
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
   App* app = appstate;

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

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
   App* app = appstate;
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

Build your host and run it against a Handler, or directly against a listener:

```sh
./build/my-app --mcp=127.0.0.1:7777
```

## The frame loop

Three rules shape a host loop:

1. **`SDL3MCP_AppInit` runs first**, before `SDL_Init`, so `--mcp-help`,
   `--mcp-version` and `--mcp=off` can answer and exit without creating a
   window. Its `SDL_AppResult` is the app's.
2. **`SDL3MCP_SetWindow` runs after the window exists**, once. It is where the
   library learns the window it reports in `sdl_info`.
3. **`SDL3MCP_AppIterate` is the last call of the frame**, after drawing, after
   `SDL_RenderPresent`, and after answering `SDL3MCP_FrameRequested`. It pumps
   the client stream, applies queued input, and completes the requests that the
   frame just answered.

A frame request is answered in the middle of the frame; the protocol work is
done at the end:

```c
SDL_SetRenderDrawColor(renderer, 24, 24, 32, 255);
SDL_RenderClear(renderer);

if (SDL3MCP_FrameRequested()) {
    /* Read back tightly packed RGBA8888 and pass its own width and height. */
}

SDL_RenderPresent(renderer);

/* Last: pump, apply input, complete requests. */
return SDL3MCP_AppIterate();
```

`SDL3MCP_WriteFrame` takes the dimensions the app chose, so a frame may come
from any render target, not only the window:

```c
SDL3MCP_WriteFrame(const uint8_t *data, size_t size, int width, int height);
```

`size` must be exactly `width * height * 4`, top-left origin, no row padding, no
pitch parameter. A dropped frame (size mismatch, null data, non-positive
dimension, over `max_frame_bytes`) leaves the request pending: `FrameRequested`
stays true, the app may retry, and `sdl_info` reports `capture.dropped` and
`capture.lastError`.

A complete manual loop looks like this:

```c
#include <SDL3/SDL.h>
#include <SDL3MCP.h>

static void answerFrame(SDL_Renderer *renderer)
{
    SDL_Surface *frame = SDL_RenderReadPixels(renderer, NULL);
    if (frame == NULL) {
        return;
    }

    SDL_Surface *rgba = frame;
    if (frame->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(frame);
        if (rgba == NULL) {
            return;
        }
    }

    const size_t rowBytes = (size_t)rgba->w * 4u;
    const size_t size = rowBytes * (size_t)rgba->h;
    uint8_t *packed = SDL_malloc(size);
    if (packed != NULL) {
        for (int y = 0; y < rgba->h; ++y) {
            SDL_memcpy(packed + rowBytes * (size_t)y,
                       (const uint8_t *)rgba->pixels + (size_t)rgba->pitch * (size_t)y,
                       rowBytes);
        }
        SDL3MCP_WriteFrame(packed, size, rgba->w, rgba->h);
        SDL_free(packed);
    }

    SDL_DestroySurface(rgba);
}

int main(int argc, char **argv)
{
    SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();
    if (SDL3MCP_AppInit(&configuration, argc, argv) != SDL_APP_CONTINUE) {
        return 1;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 1;
    }

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    if (!SDL_CreateWindowAndRenderer("SDL3MCP manual loop", 640, 360, 0,
                                     &window, &renderer)) {
        return 1;
    }
    if (SDL3MCP_SetWindow(window) != SDL_APP_CONTINUE) {
        return 1;
    }

    int running = 1;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = 0;
            }
        }

        SDL_SetRenderDrawColor(renderer, 24, 24, 32, 255);
        SDL_RenderClear(renderer);

        if (SDL3MCP_FrameRequested()) {
            answerFrame(renderer);
        }

        SDL_RenderPresent(renderer);

        /* Last call of the frame. */
        if (SDL3MCP_AppIterate() != SDL_APP_CONTINUE) {
            running = 0;
        }
    }

    SDL3MCP_AppShutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
```

## Running an application

The library never listens. The application dials an endpoint, and an endpoint is
the whole transport choice:

- `--mcp=HOST:PORT` dials one client endpoint. A failed dial fails
  `SDL3MCP_AppInit`; there is no fallback to no session.
- `--mcp=[V6]:PORT` is the IPv6 form.
- `--mcp=off` starts no session. This is also the default when the argument is
  absent.
- `--mcp-max-frame-bytes=N` sets the largest accepted frame, 1048576..1073741824
  bytes.
- `--mcp-no-warp-cursor` stops synthetic pointer actions from warping the OS
  cursor.
- `--mcp-log-level=LEVEL` accepts `off`, `error`, `warn`, `info` or `debug`.
- `--mcp-help` prints the grammar and exits successfully.
- `--mcp-version` prints `SDL3MCP 1.0.0` and exits successfully.

Only arguments that start with `--mcp` are read; everything else on the command
line is the app's. A bare `--mcp` is an error: it needs a value written as
`--mcp=VALUE`.

In practice the Handler starts the application with the endpoint it owns:

```sh
./build/sdl3mcp --targets=build/sdl3mcp-targets.json
```

`build/sdl3mcp-targets.json` is written by CMake for every executable registered
with `sdl3mcp_app`. Calling `app_spawn_catalyst` then runs the path Catalyst
registered, with `--mcp=<the Handler's listener endpoint>`.

## The library API

`include/SDL3MCP.h` is the complete public surface, and a consumer includes it
from the include root as `<SDL3MCP.h>`. It compiles as C11 and as C++20, and
public symbols carry SDL's `SDL_DECLSPEC` and `SDLCALL`. The library is static
only and publishes no SDL, so the consumer names SDL on its own link line
(§Targets).

The configuration value and the release macros live beside it in
`include/SDL3MCP_types.h`: it includes neither SDL nor the entry points, so a
translation unit that only builds or reads a configuration needs no SDL headers.
`<SDL3MCP.h>` includes it for the rest.

### Configuration

```c
typedef struct SDL3MCP_Configuration {
   char connect_host[64];
   uint16_t connect_port;
   uint32_t connect_timeout_ms;
   uint32_t max_frame_bytes;
   bool warp_cursor;
   int log_level;
} SDL3MCP_Configuration;
```

| Field | Default | Meaning |
| --- | --- | --- |
| `connect_host` | `""` | The endpoint host. Empty means no session at all. |
| `connect_port` | `0` | The endpoint port. Set exactly when the host is set. |
| `connect_timeout_ms` | `5000` | The bound on the one dial attempt, 1..60000. This is the library's only timeout. |
| `max_frame_bytes` | `33554432` | The largest accepted frame, 1048576..1073741824. |
| `warp_cursor` | `true` | Warp the cursor for pointer actions. |
| `log_level` | `-1` | `-1` keeps the default, 0..4 select `off`..`debug`. |

`SDL3MCP_DefaultConfiguration()` is a header-inline value, so it costs no symbol
and works from C and C++.

There is no transport enum and no library frame timeout. A session exists exactly
when `connect_host` names an endpoint, and the library has no frame or capture
timeout: a direct client imposes its own deadline.

### Entry points

| Function | Contract |
| --- | --- |
| `SDL3MCP_AppInit(config, argc, argv)` | Claims the process lifecycle, parses the `--mcp` arguments, and dials when an endpoint is configured. Call it first. |
| `SDL3MCP_SetWindow(window)` | Binds the window `sdl_info` reports. Call it once, after creating the window. |
| `SDL3MCP_AppIterate()` | The server loop: pumps the stream, applies input, completes requests. Call it last in every frame. |
| `SDL3MCP_AppShutdown()` | Closes the session and releases the runtime. Idempotent. |
| `SDL3MCP_FrameRequested()` | True while a capture or input batch waits for the next frame. |
| `SDL3MCP_WriteFrame(data, size, width, height)` | Answers that request with tightly packed RGBA8888. |

The lifecycle is once per process: a second `SDL3MCP_AppInit` fails even after a
shutdown, and a failed init still claims the lifecycle. There is no restart path.
`SDL3MCP_AppIterate` and `SDL3MCP_FrameRequested` before init are no-ops, and a
failed shutdown still leaves no state behind.

## Tools

The application advertises four tools. The schemas below are the library's
`tools/list` entries, in the order the catalog publishes them.

### `sdl_info`

```json
{
  "name": "sdl_info",
  "description": "Report the window the library is bound to (title, width and height in points, pixelWidth and pixelHeight in drawable pixels, pixelDensity, relative mouse mode), the keys and mouse buttons it currently holds, capture statistics, and the maximum frame size. Takes no arguments. Succeeds with a null window when no window is bound, so it is a useful first call. Example: {}",
  "inputSchema": {
    "type": "object",
    "properties": {},
    "additionalProperties": false
  },
  "annotations": {
    "readOnlyHint": true,
    "idempotentHint": true,
    "openWorldHint": false
  }
}
```

The result is one JSON object:

```json
{
  "sdlVersion": "3.2.10",
  "videoDriver": "x11",
  "window": {
    "title": "SDL3MCP demo",
    "width": 960,
    "height": 540,
    "pixelWidth": 960,
    "pixelHeight": 540,
    "pixelDensity": 1.0,
    "relativeMouseMode": false
  },
  "input": {
    "heldKeys": [],
    "heldButtons": []
  },
  "capture": {
    "pending": false,
    "count": 0,
    "dropped": 0,
    "lastWidth": 0,
    "lastHeight": 0,
    "lastBytes": 0,
    "lastMs": 0,
    "lastError": null
  },
  "limits": {
    "maxFrameBytes": 33554432
  },
  "frame": 0,
  "appliedFrame": 0
}
```

`window` is `null` before `SDL3MCP_SetWindow`; `videoDriver` is `null` when SDL
reports none; `capture.lastError` is `null` when the last frame was accepted.
`limits` carries only `maxFrameBytes`: there is no library frame timeout to
report. `capture.lastWidth` and `capture.lastHeight` describe the frame the
library last returned, after any crop, which is what a `region` is measured
against.

### `sdl_input`

```json
{
  "name": "sdl_input",
  "description": "Queue a batch of synthetic input actions; the call returns the frame that follows the batch: text naming both the frame the actions were applied at and the frame attached, plus that frame's pixels, which the Handler presents as an image/png part and a direct library client receives as raw RGBA. The batch is applied atomically at the next frame and the call returns after the following frame, once the application has consumed the generated events. Arguments: actions (required; 1-256 action objects) and optional region. Each action object is one of six types: key {key name or chord, action=down|up|press (default press), modifiers=[ctrl|shift|alt|gui] (at most four)}; text {text, 1-1024 UTF-8 bytes}; mouse_move {x,y in window points or dx,dy relative, never both}; mouse_button {button=left|middle|right|x1|x2, action=down|up|click (default click), clicks=1-3 (default 1), optional x,y window points to move first}; scroll {y in notches (1.0 is one notch, positive scrolls down), x default 0}; release_all {no extra fields}. The optional region crops the returned frame in pixels of the full frame with a top-left origin, exactly like sdl_capture; sdl_input coordinates are window points, so use pixelDensity from sdl_info to convert. There is no timeout: a direct client imposes its own. Example: {\"actions\":[{\"type\":\"key\",\"key\":\"ctrl+s\",\"action\":\"press\"},{\"type\":\"mouse_button\",\"button\":\"left\",\"x\":120,\"y\":80}],\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360}}",
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

The result is a raw frame whose header carries a `text` member:

```json
{
  "format": "rgba8888",
  "width": 640,
  "height": 360,
  "byteLength": 921600,
  "frame": 43,
  "text": "1 actions applied at frame 42; frame 43 attached, rendered after the actions were delivered"
}
```

The text is always `<N> actions applied at frame F; frame F+1 attached, rendered
after the actions were delivered`. A batch applied during frame F is consumed by
the app during F+1, and the call returns during F+1 with the frame the app wrote
there. Because the capture is requested only after that F+1 completion, the
attached frame is always rendered after the actions were delivered: an unchanged
frame means the actions had no effect, not that the observation is stale.

Every batch the pump finds applies in order at F and shares the frame at F+1.
An unknown key or chord is reported at F+1 with its action index; a structural
error fails before anything is queued, and one failed batch does not disturb the
other requests sharing the frame.

### `sdl_capture`

```json
{
  "name": "sdl_capture",
  "description": "Ask the application for a fresh frame through SDL3MCP_FrameRequested() and return its raw pixels. Arguments: optional region. The result is one JSON-RPC line {\"format\":\"rgba8888\",\"width\":W,\"height\":H,\"byteLength\":B,\"frame\":F}, immediately followed by exactly B tightly packed RGBA bytes with no newline and no base64. F is the app frame whose pixels these are; the Handler names it in the image summary. region is a crop rectangle in pixels of the full frame with a top-left origin and must fit inside the frame. sdl_input coordinates are window points, not frame pixels: sdl_info reports pixelWidth, pixelHeight and pixelDensity so a caller can convert between the two. There is no timeout: a direct client imposes its own. Example: {\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360}}",
  "inputSchema": {
    "type": "object",
    "properties": {
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

A capture is a no-op input batch: it registers a request and the next frame the
app writes answers it. Several captures and inputs registered before the same
frame share that one frame. A capture waits; the library never expires it.

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

The tool only sets a flag. `SDL3MCP_AppIterate` exchanges it and pushes exactly
one zeroed `SDL_EVENT_QUIT`, then continues to return `SDL_APP_CONTINUE`: the
app's own quit handling decides what happens next. A failed push restores the
flag so the next frame retries.

## Input actions

Actions become `SDL_PushEvent` traffic applied on the app thread, in order, as
one atomic batch. There are six types:

| Type | Fields | Notes |
| --- | --- | --- |
| `key` | `key`, `action` (`down`/`up`/`press`, default `press`), `modifiers` | Modifiers are valid only on `press`. A chord is `ctrl+shift+s`. |
| `text` | `text` | One `SDL_EVENT_TEXT_INPUT` with the whole UTF-8 string, 1..1024 bytes. The pointer is valid only until F+1. |
| `mouse_move` | `x`,`y` or `dx`,`dy` | Absolute window points or relative deltas, never both. |
| `mouse_button` | `button`, `action` (`down`/`up`/`click`), `clicks`, optional `x`,`y` | `x`/`y` move the cursor there first. |
| `scroll` | `y`, optional `x` | Notches: `1.0` is one notch, positive `y` scrolls down. |
| `release_all` | none | Releases every key and button the library holds. |

Key names are SDL scancode names and their aliases, with `-` and `_` as word
separators; a single character goes through `SDL_GetScancodeFromKey` on the app
thread. There is no `repeat` field: repeat a press with another `press`, and
auto-repeat is a repeated `down` without an intervening `up` (that event's
`repeat` flag is set).

`SDL_GetKeyboardState` cannot be updated, because SDL3 exposes no per-key
setter. Synthetic input is event-based only; that is a documented limitation,
not a bug to work around with SDL internals. Text input needs no focus: the
library starts it for a text batch and restores it afterwards. Warping needs no
focus either; it is best effort, and a synthetic zero-delta motion accompanies
it so event-driven apps still see the move.

## Capture framing

Inside the library a frame is raw, and the Handler is the only place that
presents it:

- The library writes one result line
  `{"format":"rgba8888","width":W,"height":H,"byteLength":B,"frame":F}`, then
  exactly `B` tightly packed RGBA bytes with no newline and no base64. `sdl_input`
  adds `text`.
- The Handler reads that framing, validates the header and payload size, and
  returns the standard content block
  `{"type":"image","mimeType":"image/png","data":"<base64>"}`. A Handler in front
  of another Handler relays an already-presented `content` result untouched;
  only a raw `format`/`width`/`height`/`byteLength` frame is encoded, and exactly
  once, at the hop nearest the application.
- Captures are returned at the app's native pixel size and are never resized or
  downscaled. PNG pixels map 1:1 to app pixels, so `region` is the only
  payload-size lever.
- `region` is `{x, y, width, height}` in pixels of the captured frame, top-left
  origin, all four required. It must fit inside `capture.lastWidth` by
  `capture.lastHeight` from `sdl_info`; the library crops it and the Handler
  encodes the result at those same dimensions. `sdl_input` parses it with the
  same helper, so both tools accept exactly the same region.

## The Handler

`sdl3mcp` is one executable with two modes and optional app sources. It owns one
MCP session on its client-facing face and one application session at a time.

- **`Server`** — the client face is stdio. `--listen=HOST:PORT` names the
  application listener; it is optional, and with an app source it defaults to
  `127.0.0.1:0`, an ephemeral loopback port. The bound endpoint is printed on
  stderr as `sdl3mcp: listening on HOST:PORT`, so the port the kernel assigned is
  the one reported, never the requested zero.
- **`Relay`** — `--connect=HOST:PORT` makes the client face a dialed upstream
  socket instead of stdio. `--listen` still names the application listener.

A run needs something to serve: an app source (`--targets` or `--web`) or an
explicit `--listen`. With neither it exits 2 before binding anything, naming
both app-source flags. `--dump-tools` is the exception: it prints and exits with
no listener and no app source.

```
usage: sdl3mcp [--listen=HOST:PORT] [--connect=HOST:PORT]
               [--targets=PATH ...] [--web=PATH ...] [--timeout-ms=N]
               [--dry-run=ALIAS] [--dump-tools]
```

| Option | Meaning |
| --- | --- |
| `--listen=HOST:PORT` | Accept one application connection at HOST:PORT. Port `0` asks the kernel for an ephemeral port. Optional: with an app source it defaults to `127.0.0.1:0`; an explicit value wins. |
| `--connect=HOST:PORT` | Dial an upstream MCP client (Relay mode). |
| `--targets=PATH` | An alias-to-argv app source: a JSON file or a directory of `*.json` files. Repeatable. |
| `--web=PATH` | An alias-to-web-root app source: a JSON file mapping each alias to the page root it serves. Repeatable. |
| `--timeout-ms=N` | The forward, attach and spawn budget in milliseconds, 1..60000, default 10000. A tool's `timeout_ms` overrides it per call. |
| `--dry-run=ALIAS` | Print what `app_spawn_<alias>` would run and start nothing. |
| `--dump-tools` | Print the catalog `tools/list` advertises as JSON and exit. Needs no listener and no app source; it is the documentation check's view of the real list. |
| `--help` | Print the usage block and exit 0. |
| `--version` | Print `SDL3MCP 1.0.0` and exit 0. |

### Tools and merging

The advertised list is a function of the mode and the app sources, never of
whether an application is attached:

- `wait_session`, plus the four application tools, plus one `app_spawn_<alias>`
  per `--targets` or `--web` entry.
- Before an application attaches, every application tool answers
  `no application session is attached; call wait_session first`.
- When an application attaches, the Handler merges the peer's entries over its
  base by name. `sdl_capture` always keeps the Handler's definition because it
  owns presentation; the other entries take the peer's metadata but keep the
  Handler's forward. Names only the peer advertises are appended.
- A forwarded tool the peer did not advertise answers
  `the application does not provide <name>` instead of being sent.
- A spawn, attach or detach changes no advertised name, so no
  `notifications/tools/list_changed` is sent for it. A notification is sent only
  when the merged set really changes; a peer's own list-changed notification
  makes the Handler ask it for `tools/list` again and re-merge.

The Handler's own copies of the two blocking tools add its forward budget:

```json
{
  "name": "sdl_capture",
  "description": "Ask the application for a fresh frame and return it as a PNG image block at the app's native pixel size. Arguments: optional timeout_ms (1-60000, default the Handler's --timeout-ms) and optional region. region crops pixels from the captured frame with a top-left origin and must fit inside capture.lastWidth x capture.lastHeight from sdl_info. sdl_input coordinates are window points, while region is in pixels of the full frame; sdl_info reports pixelWidth, pixelHeight and pixelDensity so a caller can convert between them. Captures are never resized, so region is the only way to keep the payload small. The text part names the frame attached, so a caller can tell a fresh frame from a stale one. Example: {\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360},\"timeout_ms\":2000}",
  "inputSchema": {
    "type": "object",
    "properties": {
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
      },
      "timeout_ms": { "type": "integer", "minimum": 1, "maximum": 60000 }
    },
    "additionalProperties": false
  }
}
```

```json
{
  "name": "sdl_input",
  "description": "Queue a batch of synthetic input actions; the call returns the frame that follows the batch: text naming both the frame the actions were applied at and the frame attached, plus that frame's pixels, presented as an image/png part. The batch is applied atomically at the next frame and the call returns after the following frame, once the application has consumed the generated events. Arguments: actions (required; 1-256 action objects), optional region, and optional timeout_ms (1-60000, default the Handler's --timeout-ms) bounding the whole call. Each action object is one of six types: key {key name or chord, action=down|up|press (default press), modifiers=[ctrl|shift|alt|gui]}; text {text, 1-1024 UTF-8 bytes}; mouse_move {x,y in window points or dx,dy relative}; mouse_button {button=left|middle|right|x1|x2, action=down|up|click (default click), clicks=1-3, optional x,y}; scroll {y in notches, x default 0}; release_all. The optional region crops the returned frame in pixels of the full frame with a top-left origin. Example: {\"actions\":[{\"type\":\"key\",\"key\":\"ctrl+s\",\"action\":\"press\"}],\"region\":{\"x\":0,\"y\":0,\"width\":640,\"height\":360}}",
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
                "type": {
                  "const": "key"
                },
                "key": {
                  "type": "string",
                  "description": "Key name, or a modifier+modifier+key chord for press, for example \"ctrl+s\"."
                },
                "action": {
                  "enum": [
                    "down",
                    "up",
                    "press"
                  ],
                  "default": "press"
                },
                "modifiers": {
                  "type": "array",
                  "maxItems": 4,
                  "items": {
                    "enum": [
                      "ctrl",
                      "shift",
                      "alt",
                      "gui"
                    ]
                  }
                }
              },
              "required": [
                "type",
                "key"
              ],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Type text as SDL text input.",
              "properties": {
                "type": {
                  "const": "text"
                },
                "text": {
                  "type": "string",
                  "minLength": 1,
                  "maxLength": 1024
                }
              },
              "required": [
                "type",
                "text"
              ],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Move the cursor to absolute window points (x, y) or by relative deltas (dx, dy).",
              "properties": {
                "type": {
                  "const": "mouse_move"
                },
                "x": {
                  "type": "number"
                },
                "y": {
                  "type": "number"
                },
                "dx": {
                  "type": "number"
                },
                "dy": {
                  "type": "number"
                }
              },
              "required": [
                "type"
              ],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Press, release, or click a mouse button at the cursor, or at optional window points (x, y) that move the cursor there first.",
              "properties": {
                "type": {
                  "const": "mouse_button"
                },
                "button": {
                  "enum": [
                    "left",
                    "middle",
                    "right",
                    "x1",
                    "x2"
                  ]
                },
                "action": {
                  "enum": [
                    "down",
                    "up",
                    "click"
                  ],
                  "default": "click"
                },
                "x": {
                  "type": "number"
                },
                "y": {
                  "type": "number"
                },
                "clicks": {
                  "type": "integer",
                  "minimum": 1,
                  "maximum": 3,
                  "default": 1
                }
              },
              "required": [
                "type",
                "button"
              ],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Scroll the wheel; 1.0 is one notch and positive y scrolls down.",
              "properties": {
                "type": {
                  "const": "scroll"
                },
                "x": {
                  "type": "number",
                  "default": 0
                },
                "y": {
                  "type": "number"
                }
              },
              "required": [
                "type",
                "y"
              ],
              "additionalProperties": false
            },
            {
              "type": "object",
              "description": "Release every key and mouse button the library holds.",
              "properties": {
                "type": {
                  "const": "release_all"
                }
              },
              "required": [
                "type"
              ],
              "additionalProperties": false
            }
          ]
        }
      },
      "region": {
        "type": "object",
        "description": "Crop rectangle in pixels of the full frame, top-left origin.",
        "properties": {
          "x": {
            "type": "integer",
            "minimum": 0
          },
          "y": {
            "type": "integer",
            "minimum": 0
          },
          "width": {
            "type": "integer",
            "minimum": 1
          },
          "height": {
            "type": "integer",
            "minimum": 1
          }
        },
        "required": [
          "x",
          "y",
          "width",
          "height"
        ],
        "additionalProperties": false
      },
      "timeout_ms": {
        "type": "integer",
        "minimum": 1,
        "maximum": 60000
      }
    },
    "required": [
      "actions"
    ],
    "additionalProperties": false
  }
}
```

`wait_session` takes an optional `timeout_ms` (1..60000, default
`--timeout-ms`) and returns as soon as an application is attached; an expired
timeout is a normal tool error. `app_spawn_<alias>` takes the same optional
`timeout_ms`, starts the configured application (or the web stack), and waits for
it to connect and complete the handshake.

### Forwarding and timeouts

One forward path serves every tool:

- A call carrying `timeout_ms` gets that budget plus a 500 ms grace. Without
  one, every forwarded call gets the Handler's `--timeout-ms` budget.
  `wait_session` and `app_spawn_*` default to the same budget.
- A result with `content` is relayed verbatim.
- A result with `byteLength` is read, PNG-encoded, and answered as text plus one
  image block. The frame's own `text` is used when it has one; otherwise the
  summary is `WxH PNG, N bytes; frame F attached`.
- The application is the authority on when it has nothing to send; the Handler
  only bounds the wait.

### App sources

A `--targets` file maps aliases to full argv arrays. A path that is a directory
contributes every top-level `*.json` file in sorted order; all sources accumulate
into one map, and an alias defined twice is a startup error naming both files.

```json
{
  "catalyst": ["build/catalyst"],
  "game": ["build/game", "--fullscreen"]
}
```

A `--web` file maps aliases to page roots. The page entry is always
`index.html`, and a relative root resolves against the Handler's working
directory because the Handler never chdirs.

```json
{
  "demo": "build/web"
}
```

Aliases from both sources share one map and the same `[a-z0-9_]+` rule; the
`app_spawn_` prefix is reserved. A collision is a startup error naming both
paths.

### Spawning

`app_spawn_<alias>` is synchronous: it starts the application with
`--mcp=<the Handler's listener endpoint>` appended to its configured argv, then
waits for the connection and the MCP handshake, up to its budget. A second spawn
while one is attached or starting fails with
`an application is already attached; one spawn at a time`. A successful spawn
returns `spawned <alias> (pid N)`.

A spawned application is **spawn-and-forget**: the Handler does not signal, wait
for, or act on it, and it is not killed when the Handler exits. Each application
owns its cleanup, and `sdl_terminate` is forwarded so the app can quit itself.
The poll loop reaps exited children so none becomes a zombie, but a spawned
app's status is ignored.

A `--web` spawn is the exception. It starts three processes that together make
the application reachable:

1. `python3 -m http.server PAGE_PORT --bind 127.0.0.1 --directory ROOT`;
2. `websocat --binary ws-l:127.0.0.1:BRIDGE_PORT tcp:<the Handler's endpoint>`;
3. Firefox, in a throwaway profile, on
   `http://127.0.0.1:PAGE_PORT/index.html?mcp=127.0.0.1:BRIDGE_PORT`.

The ports are ephemeral and the page URL, bridge and profile are printed on
stderr. The Handler owns that whole stack: when the browser exits, or the page
server or bridge dies, or the spawn times out, or the Handler shuts down, it
stops what is left with SIGTERM, one grace period and SIGKILL, and removes the
profile. `websocat` (`cargo install websocat`), `python3` and Firefox
(`firefox` or `firefox-esr`, or `$BROWSER` naming one of them) must be on PATH
and the root must hold `index.html`, or the spawn is one error instead of a
half-launched stack.

### Dry run and catalog dump

`--dry-run=ALIAS` prints what a spawn would do and starts nothing. A target
prints one shell-quoted `command:` line; a web alias prints `page=`, `bridge=`,
`profile=` and one `command:` line per process. With an ephemeral listener the
endpoint is printed as the placeholder `HOST:PORT`.

`--dump-tools` prints the catalog as a JSON array and exits. It reads the same
`--targets`/`--web` sources, so `app_spawn_<alias>` entries are included, and it
needs no listener. `readme_check` uses it to compare the documented schemas and
`Example:` descriptions with the definitions `tools/list` really returns.

### Registering an application

`sdl3mcp_app(<alias> <target> [ARGS <arg>...])` records one entry and, at the end
of the top-level configure, writes `${CMAKE_BINARY_DIR}/sdl3mcp-targets.json`
with each target's full path. It is a function, not a target, so an application
that is not built is simply absent. Catalyst registers its `catalyst` target,
which is why the file above names it.

`sdl3mcp_web(<alias> <target>)` is the web half and mirrors it: registration
records the page root and, at the end of the top-level configure, writes
`${CMAKE_BINARY_DIR}/sdl3mcp-web.json`, mapping each alias to its target's
output directory — the root `--web` serves. The two functions share one alias
namespace, so an alias is taken once: the grammar is `[a-z0-9_]+`, the
`app_spawn_` prefix stays reserved, and a second registration fails naming the
alias and the target that already owns it. A web registration also fails at
configure time when the build is not Emscripten, when the target does not exist,
and when the target does not emit an `.html` artifact, because only a browser
page has a root to serve.

The page target is the Emscripten shape: an executable whose artifact is the
page, which is what `SUFFIX ".html"` makes it.

```cmake
set_target_properties(catalyst PROPERTIES SUFFIX ".html")
sdl3mcp_web(catalyst catalyst)
```

The Handler then serves the generated file, and every registered alias gets the
same `app_spawn_<alias>` tools a `--targets` entry does:

```sh
./build/sdl3mcp --web=build/sdl3mcp-web.json
```

A page registered this way is launched with the page server, the `websocat`
bridge and the throwaway Firefox profile described in §Spawning.

## The browser

A page has no descriptors, no threads, and no command line. The library's
browser arm (`src/WebSocketStream.cpp`) replaces the native socket:

- The page reads `?mcp=HOST:PORT` from `window.location.search`, percent-decodes
  it, and hands the same `--mcp=<value>` argument to the one configuration
  parser. A page without the parameter starts no session.
- The peer adds the `ws://` scheme itself, so the endpoint grammar stays
  `HOST:PORT` on both platforms.
- `SDL3MCP_AppIterate` pumps the WebSocket callbacks at the top of the frame and
  completes deferred calls after the frame work. `sdl_capture` and `sdl_input`
  never wait: they register the request, return a deferred result, and a later
  frame answers the call.
- A WebSocket close or error sets the quit flag; the next `SDL3MCP_AppIterate`
  pushes one `SDL_EVENT_QUIT`; the app's own quit handling runs, and
  `SDL3MCP_AppShutdown` closes the stream and calls `window.close()`.
- A `--web` run seeds the throwaway Firefox profile with
  `devtools.console.stdout.content`, so the page's `console.log`/`console.error`
  — including a wasm abort — arrives on the Handler's stderr instead of being
  visible only in a browser window nobody is watching.

## Platforms and limits

- **Linux only.** The Handler uses one `poll` loop with `signalfd` and `pidfd`,
  `fork`/`exec`, and process groups. Windows is a documented TODO; a `WIN32`
  configure stops with a `FATAL_ERROR`.
- **Static only.** `SDL3MCP` is a static library and publishes no SDL. The
  consumer links the SDL target it chooses (`SDL3::SDL3-static`) and, on
  Emscripten, `-lwebsocket.js`.
- **One application session at a time.** A second connection is accepted then
  closed.
- **One MCP session per library instance.** `--mcp=off` is the only way to have
  none; there is no reconnect after a failed dial.
- **No library timeout.** The only library bound is `connect_timeout_ms`. A
  direct client must impose its own deadline on `sdl_capture` and `sdl_input`;
  the Handler's `timeout_ms` is the shipped budget.
- **Line cap.** One JSON-RPC line is capped at 1 MiB. Frames travel as the raw
  payload after the header, not as base64, so the cap does not apply to them.
- **Frame cap.** `max_frame_bytes` defaults to 32 MiB and accepts
  1048576..1073741824.
- **Input caps.** 1..256 actions per batch, 1..1024 UTF-8 bytes per text action,
  1..3 clicks, at most four modifiers.
- **Timeout cap.** `connect_timeout_ms`, the Handler's `--timeout-ms` and every
  tool `timeout_ms` are 1..60000.
- **One lifecycle per process.** A second `SDL3MCP_AppInit` fails; shutdown is
  idempotent.

## Tests

`ctest --test-dir build --output-on-failure` runs the doctest suites:

- the pure value halves: key map, input planning, replay, mouse normalization,
  frame validation and crop;
- the protocol core: JSON-RPC, tools, results, session, streams, configuration;
- the library end to end over a real loopback socket, with a test MCP server and
  SDL's dummy video driver;
- the lifecycle, one scenario per process;
- the Handler over pipes and sockets, with a fake application and a controllable
  page server/bridge;
- `targets_check`, which configures a parent project and asserts the three public
  target names plus the documented build graph (static library, no published
  SDL, COMMON with sources);
- `web_registration_check`, which configures a tiny project once per case and
  asserts that `sdl3mcp_web` writes the page root, and that a desktop configure,
  a bad alias, a reserved alias, a duplicate alias and a target without an
  `.html` artifact each fail naming the alias or target;
- `readme_check`, which compiles the README's two complete C examples, parses
  every JSON block, compares the documented tool schemas with the Handler's
  `--dump-tools` catalog, checks the framing prose, the `sdl_input` text and the
  loop order, and rejects any reference to a bundled example.

The optional `SDL3MCP_BUILD_NODE_TESTS=ON` Emscripten configure adds the
WebSocket suite, run under Node against a local server.

## Versioning

The release is **1.0.0**. It is the project version, `SDL3MCP_VERSION_STRING`,
`serverInfo.version` in the MCP `initialize` answer, and `--mcp-version`. A
`1.0.0` client handshake looks like this:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2025-11-25",
    "capabilities": {
      "tools": {}
    },
    "serverInfo": {
      "name": "sdl3mcp",
      "version": "1.0.0"
    }
  }
}
```

`CHANGELOG.md` records the 1.0.0 breaks. `CONTRACT.md` is the v0 oracle kept for
reference; it is not the current contract, and `README.md` is.
