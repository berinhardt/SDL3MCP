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
