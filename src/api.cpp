/*
  The public lifecycle. This file is the only place where the C boundary meets
  the C++ internals, so it owns three things that must not leak:

  - the process-global LibraryState (one per process, app-thread only);
  - the once flag, which makes a second SDL3MCP_AppInit fail even after the
    state has been freed;
  - every translation between an exception and an SDL error plus SDL_AppResult.

  With transport NONE no descriptor is duplicated, redirected, read, or written:
  AppInit only records the configuration and builds the app-thread caches. With
  STDIO the StdioIsolation helper owns the descriptor dance, and the server gets
  private duplicates of the original stdin/stdout so a host printf can no longer
  corrupt the JSON-RPC stream.

  Tool execution, capture, and queue draining are deliberately still stubs; they
  arrive on their own steps.
*/

#include <SDL3MCP/SDL3MCP.h>

#include "configuration.hpp"
#include "keymap.hpp"
#include "queue.hpp"
#include "server.hpp"
#include "session.hpp"
#include "stdio_isolation.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace
{

using sdl3mcp::Configuration;
using sdl3mcp::InputQueue;
using sdl3mcp::KeyMap;
using sdl3mcp::Server;
using sdl3mcp::Session;
using sdl3mcp::StdioIsolation;
using sdl3mcp::ToolCall;
using sdl3mcp::ToolHandler;
using sdl3mcp::ToolResult;

constexpr const char *kToolStubMessage = "tool execution is not implemented yet";

struct LibraryState
{
    explicit LibraryState(Configuration configuration)
        : configuration(std::move(configuration))
    {
    }

    Configuration configuration;
    KeyMap keyMap;
    InputQueue inputQueue;
    Session session;
    StdioIsolation isolation;

    /* Present only while the stdio server runs; owns its thread. */
    std::unique_ptr<Server> server;

    /* App thread only. The server thread has no way to reach it. */
    SDL_Window *window = nullptr;
    uint64_t frameCounter = 0;
};

std::unique_ptr<LibraryState> g_state;

/* Set by the first AppInit and never cleared: one lifecycle per process. */
std::atomic<bool> g_lifecycleClaimed{false};

void writeToStderr(const std::string &text)
{
    std::fprintf(stderr, "%s\n", text.c_str());
}

void writeCommandLineError(const std::string &message)
{
    std::fprintf(stderr, "SDL3MCP: %s\n%s\n",
                 message.c_str(),
                 Configuration::usageText().c_str());
}

ToolHandler makeStubHandler()
{
    return [](const ToolCall &) {
        ToolResult result;
        result.content = nlohmann::json::array({
            nlohmann::json{{"type", "text"}, {"text", kToolStubMessage}},
        });
        result.isError = true;
        return result;
    };
}

/*
  Stops the queue before the server so a handler blocked on F+1 wakes up, then
  joins the reader, then hands stdin/stdout back. Safe with no state, and safe
  when startup failed before the server existed.
*/
void teardownState() noexcept
{
    if (g_state) {
        try {
            g_state->inputQueue.stop();
            if (g_state->server) {
                g_state->server->stop();
                g_state->server->join();
            }
            g_state->isolation.restore();
        } catch (...) {
            /* Shutdown must not throw; dropping the state still stops the thread. */
        }
        g_state.reset();
    }
}

SDL_AppResult reportException(const char *entryPoint, const char *what)
{
    SDL_SetError("%s failed: %s", entryPoint, what);
    return SDL_APP_FAILURE;
}

} // namespace

extern "C" SDL_DECLSPEC SDL_AppResult SDLCALL
SDL3MCP_AppInit(SDL3MCP_Configuration *config, int argc, char **argv)
{
    try {
        if (g_lifecycleClaimed.exchange(true)) {
            SDL_SetError("SDL3MCP_AppInit has already been called in this process");
            return SDL_APP_FAILURE;
        }

        const SDL3MCP_Configuration base =
            config != nullptr ? *config : SDL3MCP_DefaultConfiguration();

        Configuration configuration(base);
        const Configuration::Status status = configuration.parseCommandLine(argc, argv);
        if (status == Configuration::Status::Error) {
            writeCommandLineError(configuration.message());
            SDL_SetError("%s", configuration.message().c_str());
            return SDL_APP_FAILURE;
        }
        if (status == Configuration::Status::ExitSuccess) {
            writeToStderr(configuration.message());
            return SDL_APP_SUCCESS;
        }

        auto state = std::make_unique<LibraryState>(std::move(configuration));
        g_state = std::move(state);

        if (g_state->configuration.value().transport == SDL3MCP_TRANSPORT_STDIO) {
            std::string error;
            if (!g_state->isolation.engage(error)) {
                g_state.reset();
                SDL_SetError("%s", error.c_str());
                return SDL_APP_FAILURE;
            }

            auto server = std::make_unique<Server>(g_state->isolation.mcpReadFd(),
                                                   g_state->isolation.mcpWriteFd(),
                                                   g_state->session,
                                                   makeStubHandler());
            if (!server->start(error)) {
                g_state->isolation.restore();
                g_state.reset();
                SDL_SetError("%s", error.c_str());
                return SDL_APP_FAILURE;
            }
            g_state->server = std::move(server);
        }

        return SDL_APP_CONTINUE;
    } catch (const std::exception &exception) {
        teardownState();
        return reportException("SDL3MCP_AppInit", exception.what());
    } catch (...) {
        teardownState();
        return reportException("SDL3MCP_AppInit", "unknown exception");
    }
}

extern "C" SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_SetWindow(SDL_Window *window)
{
    try {
        if (!g_state) {
            SDL_SetError("SDL3MCP_SetWindow called before SDL3MCP_AppInit");
            return SDL_APP_FAILURE;
        }

        if (window == nullptr) {
            g_state->window = nullptr;
            return SDL_APP_CONTINUE;
        }

        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height)) {
            SDL_SetError("SDL3MCP_SetWindow cannot measure the window: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        if (width <= 0 || height <= 0) {
            SDL_SetError("SDL3MCP_SetWindow needs a drawable window, got %dx%d", width, height);
            return SDL_APP_FAILURE;
        }

        g_state->window = window;
        return SDL_APP_CONTINUE;
    } catch (const std::exception &exception) {
        return reportException("SDL3MCP_SetWindow", exception.what());
    } catch (...) {
        return reportException("SDL3MCP_SetWindow", "unknown exception");
    }
}

extern "C" SDL_DECLSPEC SDL_AppResult SDLCALL SDL3MCP_AppIterate(void)
{
    try {
        if (!g_state) {
            return SDL_APP_CONTINUE;
        }
        if (g_state->configuration.value().transport == SDL3MCP_TRANSPORT_NONE) {
            return SDL_APP_CONTINUE;
        }
        if (g_state->server && g_state->server->failed()) {
            const std::string error = g_state->server->lastError();
            SDL_SetError("%s", error.empty() ? "the MCP server thread failed" : error.c_str());
            return SDL_APP_FAILURE;
        }

        ++g_state->frameCounter;
        return SDL_APP_CONTINUE;
    } catch (const std::exception &exception) {
        return reportException("SDL3MCP_AppIterate", exception.what());
    } catch (...) {
        return reportException("SDL3MCP_AppIterate", "unknown exception");
    }
}

extern "C" SDL_DECLSPEC void SDLCALL SDL3MCP_AppShutdown(void)
{
    try {
        teardownState();
    } catch (...) {
        /* A failed shutdown must still be a no-op rather than an escaping exception. */
        g_state.reset();
    }
}

extern "C" SDL_DECLSPEC bool SDLCALL SDL3MCP_FrameRequested(void)
{
    try {
        /* Capture wiring arrives on its own step; nothing is ever pending yet. */
        return false;
    } catch (...) {
        return false;
    }
}

extern "C" SDL_DECLSPEC void SDLCALL SDL3MCP_WriteFrame(const uint8_t *data, size_t size)
{
    try {
        /* Frame validation and handoff arrive on their own step. */
        (void)data;
        (void)size;
    } catch (...) {
    }
}
