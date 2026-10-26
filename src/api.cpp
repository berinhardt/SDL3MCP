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

  sdl_input runs on the server thread and only fills the input queue; the app
  thread turns a taken batch into SDL events in AppIterate. sdl_capture is still
  a stub and arrives on its own step; sdl_info is answered from a copied
  snapshot.
*/

#include <SDL3MCP/SDL3MCP.h>

#include "app_input.hpp"
#include "configuration.hpp"
#include "info.hpp"
#include "input_json.hpp"
#include "keymap.hpp"
#include "mouse.hpp"
#include "queue.hpp"
#include "replay.hpp"
#include "server.hpp"
#include "session.hpp"
#include "stdio_isolation.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{

using sdl3mcp::AppliedBatchPlan;
using sdl3mcp::Configuration;
using sdl3mcp::InfoSnapshot;
using sdl3mcp::InputBatch;
using sdl3mcp::InputOutcome;
using sdl3mcp::InputQueue;
using sdl3mcp::InputStatus;
using sdl3mcp::KeyMap;
using sdl3mcp::KeyReplayer;
using sdl3mcp::MouseButton;
using sdl3mcp::MouseMetrics;
using sdl3mcp::MouseReplayer;
using sdl3mcp::Server;
using sdl3mcp::Session;
using sdl3mcp::StdioIsolation;
using sdl3mcp::TakenBatch;
using sdl3mcp::ToolCall;
using sdl3mcp::ToolHandler;
using sdl3mcp::ToolResult;
using sdl3mcp::WindowInfo;

constexpr const char *kToolStubMessage = "tool execution is not implemented yet";

struct LibraryState
{
    explicit LibraryState(Configuration configuration)
        : configuration(std::move(configuration)),
          keyReplayer(keyMap, 0),
          mouseReplayer(0, MouseMetrics{})
    {
    }

    Configuration configuration;
    KeyMap keyMap;

    /* App thread only: replay state committed by the next applied batch. */
    KeyReplayer keyReplayer;
    MouseReplayer mouseReplayer;

    InputQueue inputQueue;
    Session session;
    StdioIsolation isolation;

    /* Present only while the stdio server runs; owns its thread. */
    std::unique_ptr<Server> server;

    /* App thread only. The server thread has no way to reach it. */
    SDL_Window *window = nullptr;
    SDL_WindowID windowID = 0;
    uint64_t frameCounter = 0;
    uint64_t appliedFrame = 0;

    /*
      The batch taken this frame. It stays here until the frame after it was
      applied, because its text strings are what the pushed text events point at.
    */
    std::optional<TakenBatch> pendingBatch;
    bool pendingNeedsComplete = false;
    uint64_t pendingAppliedFrame = 0;

    /*
      The sdl_info payload. The app thread is the only writer because every
      SDL-derived field is read there; the server thread only copies the whole
      value under the mutex, so no SDL call crosses the boundary.
    */
    mutable std::mutex infoMutex;
    InfoSnapshot info;

    /* App thread only: writes the served snapshot. */
    void publishInfo(InfoSnapshot snapshot)
    {
        std::lock_guard<std::mutex> lock(infoMutex);
        info = std::move(snapshot);
    }

    /* Any thread: returns a copy that outlives the lock. */
    InfoSnapshot snapshotInfo() const
    {
        std::lock_guard<std::mutex> lock(infoMutex);
        return info;
    }
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

/* "major.minor.micro" from the SDL the library was linked against. */
std::string formatSdlVersion()
{
    const int version = SDL_GetVersion();
    return std::to_string(SDL_VERSIONNUM_MAJOR(version)) + "." +
           std::to_string(SDL_VERSIONNUM_MINOR(version)) + "." +
           std::to_string(SDL_VERSIONNUM_MICRO(version));
}

/*
  App thread only. Reads every SDL field sdl_info reports for a bound window;
  false means SDL could not measure it and `info` is unusable.
*/
bool measureWindow(SDL_Window *window, WindowInfo &info)
{
    info = WindowInfo{};
    info.bound = true;

    const char *title = SDL_GetWindowTitle(window);
    info.title = title != nullptr ? title : "";
    SDL_GetWindowSize(window, &info.width, &info.height);
    if (!SDL_GetWindowSizeInPixels(window, &info.pixelWidth, &info.pixelHeight)) {
        info.bound = false;
        return false;
    }
    info.pixelDensity = SDL_GetWindowPixelDensity(window);
    info.relativeMouseMode = SDL_GetWindowRelativeMouseMode(window);
    return true;
}

ToolResult textResult(std::string text, bool isError)
{
    ToolResult result;
    result.content = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", std::move(text)}},
    });
    result.isError = isError;
    return result;
}

/* The wire name of a held button, matching the sdl_info example. */
std::string_view buttonName(MouseButton button)
{
    switch (button) {
    case MouseButton::Left:
        return "left";
    case MouseButton::Middle:
        return "middle";
    case MouseButton::Right:
        return "right";
    case MouseButton::X1:
        return "x1";
    case MouseButton::X2:
        return "x2";
    }
    return {};
}

/* App thread only. Releases held input, pushing the events only for a window. */
void releaseHeldInput(LibraryState &state)
{
    std::vector<SDL_Event> events;
    const Uint64 timestamp = SDL_GetTicksNS();
    state.keyReplayer.releaseAllKeys(timestamp, events);
    state.mouseReplayer.releaseAllButtons(timestamp, events);

    if (state.windowID != 0) {
        for (SDL_Event &event : events) {
            SDL_PushEvent(&event);
        }
    }
    SDL_SetModState(state.keyReplayer.modifiers());
}

/*
  App thread only. Completes the previous batch one frame after it was applied,
  which is when the app has consumed its events and the text may be freed.
*/
void completePendingBatch(LibraryState &state, uint64_t frame)
{
    if (!state.pendingBatch.has_value() || frame <= state.pendingAppliedFrame) {
        return;
    }
    if (state.pendingNeedsComplete) {
        state.inputQueue.complete(state.pendingBatch->seq);
    }
    state.pendingBatch.reset();
}

/* True when every event reached SDL's queue; modifiers are mirrored either way. */
bool pushPlanEvents(const LibraryState &state, AppliedBatchPlan &plan)
{
    bool allPushed = true;
    for (std::size_t index = 0; index < plan.eventsByAction.size(); ++index) {
        const std::optional<SDL_FPoint> &warp = plan.warpsByAction[index];
        if (warp.has_value() && state.window != nullptr) {
            SDL_WarpMouseInWindow(state.window, warp->x, warp->y);
        }
        for (SDL_Event &event : plan.eventsByAction[index]) {
            if (!SDL_PushEvent(&event)) {
                allPushed = false;
                break;
            }
        }
        if (!allPushed) {
            break;
        }
    }
    SDL_SetModState(state.keyReplayer.modifiers());
    return allPushed;
}

/*
  App thread only. Takes at most one batch per frame, builds its plan, and either
  publishes it to SDL's queue or fails it back to the waiting sdl_input call.
*/
void applyNextBatch(LibraryState &state, uint64_t frame)
{
    std::optional<TakenBatch> taken = state.inputQueue.takeBatch();
    if (!taken.has_value()) {
        return;
    }

    /* Move before building: the plan's text events borrow this batch's strings. */
    state.pendingBatch = std::move(taken);
    const TakenBatch &pending = *state.pendingBatch;

    const bool textInputActive = state.window != nullptr && SDL_TextInputActive(state.window);
    AppliedBatchPlan plan{state.keyReplayer, state.mouseReplayer};
    std::string error;
    if (!sdl3mcp::buildBatchPlan(pending.batch, state.keyReplayer, state.mouseReplayer,
                                 state.windowID, textInputActive, SDL_GetTicksNS(), plan,
                                 error)) {
        state.inputQueue.markApplied(pending.seq, frame, std::move(error));
        state.pendingBatch.reset();
        return;
    }

    state.keyReplayer = plan.keyState;
    state.mouseReplayer = plan.mouseState;

    if (!pushPlanEvents(state, plan)) {
        /* Nothing was rolled back; only the acknowledgement is missing. */
        state.inputQueue.markApplied(pending.seq, frame, std::string("SDL event queue full"));
        state.pendingNeedsComplete = false;
        return;
    }

    state.appliedFrame = frame;
    state.inputQueue.markApplied(pending.seq, frame, std::nullopt);
    state.pendingNeedsComplete = true;
    state.pendingAppliedFrame = frame;
}

/* App thread only. Rewrites the snapshot the server thread may read at any time. */
void publishSnapshot(LibraryState &state, uint64_t frame)
{
    InfoSnapshot snapshot = state.snapshotInfo();
    snapshot.frame = frame;
    snapshot.appliedFrame = state.appliedFrame;

    snapshot.heldKeys.clear();
    for (const SDL_Scancode scancode : state.keyReplayer.heldKeys()) {
        const std::string_view name = state.keyMap.canonicalName(scancode);
        if (!name.empty()) {
            snapshot.heldKeys.emplace_back(name);
        }
    }
    snapshot.heldButtons.clear();
    for (const MouseButton button : state.mouseReplayer.heldButtons()) {
        snapshot.heldButtons.emplace_back(buttonName(button));
    }

    WindowInfo window;
    if (state.window != nullptr && measureWindow(state.window, window)) {
        snapshot.window = std::move(window);
    }
    state.publishInfo(std::move(snapshot));
}

/* Server thread: parses the arguments, queues the batch, and waits for the app. */
ToolResult inputResult(LibraryState &state, const ToolCall &call)
{
    std::string error;
    std::optional<InputBatch> batch = sdl3mcp::parseInputArguments(call.arguments, error);
    if (!batch.has_value()) {
        return textResult(std::move(error), true);
    }

    const std::size_t actionCount = batch->actions.size();
    const auto timeout = std::chrono::milliseconds(state.configuration.value().frame_timeout_ms);
    InputOutcome outcome;
    state.inputQueue.submit(std::move(*batch), timeout, outcome);

    switch (outcome.status) {
    case InputStatus::Applied:
        return textResult(std::to_string(actionCount) + " actions applied at frame " +
                              std::to_string(outcome.appliedFrame),
                          false);
    case InputStatus::AppliedUnconfirmed:
        return textResult("applied at frame " + std::to_string(outcome.appliedFrame) +
                              " but not confirmed before the timeout",
                          true);
    case InputStatus::Cancelled:
        return textResult("input not applied; is SDL3MCP_AppIterate called every loop?", true);
    case InputStatus::Failed:
        return textResult(outcome.error, true);
    case InputStatus::Stopped:
        return textResult("SDL3MCP is shutting down", true);
    }
    return textResult("unknown input outcome", true);
}

/*
  The server thread dispatcher. sdl_info answers from the published copy and
  sdl_input blocks in the queue; capture is not implemented yet. Only the queue
  crosses the boundary, never SDL.
*/
ToolHandler makeToolHandler(LibraryState *state)
{
    return [state](const ToolCall &call) {
        if (call.name == "sdl_info") {
            return textResult(sdl3mcp::infoToJson(state->snapshotInfo()).dump(2), false);
        }
        if (call.name == "sdl_input") {
            return inputResult(*state, call);
        }
        return textResult(kToolStubMessage, true);
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
            releaseHeldInput(*g_state);
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

        /*
          The effective configuration decides the limits, so publish before the
          server can answer anything.
        */
        InfoSnapshot info;
        info.sdlVersion = formatSdlVersion();
        info.frameTimeoutMs = g_state->configuration.value().frame_timeout_ms;
        info.maxFrameBytes = g_state->configuration.value().max_frame_bytes;
        g_state->publishInfo(std::move(info));

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
                                                   makeToolHandler(g_state.get()));
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
            g_state->windowID = 0;
            g_state->keyReplayer.reset();
            g_state->keyReplayer.setWindowID(0);
            g_state->mouseReplayer.reset();
            g_state->mouseReplayer.setWindowID(0);

            InfoSnapshot snapshot = g_state->snapshotInfo();
            snapshot.window = WindowInfo{};
            g_state->publishInfo(std::move(snapshot));
            return SDL_APP_CONTINUE;
        }

        WindowInfo info;
        if (!measureWindow(window, info)) {
            SDL_SetError("SDL3MCP_SetWindow cannot measure the window: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        if (info.pixelWidth <= 0 || info.pixelHeight <= 0) {
            SDL_SetError("SDL3MCP_SetWindow needs a drawable window, got %dx%d",
                         info.pixelWidth, info.pixelHeight);
            return SDL_APP_FAILURE;
        }

        /* A rebind starts from clean input state: nothing held crosses windows. */
        const SDL_WindowID windowID = SDL_GetWindowID(window);
        g_state->window = window;
        g_state->windowID = windowID;
        g_state->keyReplayer.setWindowID(windowID);
        g_state->keyReplayer.reset();
        g_state->mouseReplayer.setWindowID(windowID);
        g_state->mouseReplayer.setMetrics(MouseMetrics{
            static_cast<float>(info.width), static_cast<float>(info.height),
            info.relativeMouseMode, g_state->configuration.value().warp_cursor});
        g_state->mouseReplayer.reset();
        float mouseX = 0.0F;
        float mouseY = 0.0F;
        SDL_GetMouseState(&mouseX, &mouseY);
        g_state->mouseReplayer.setCursor(mouseX, mouseY);

        InfoSnapshot snapshot = g_state->snapshotInfo();
        snapshot.window = std::move(info);
        const char *driver = SDL_GetCurrentVideoDriver();
        snapshot.videoDriver = driver != nullptr ? driver : "";
        g_state->publishInfo(std::move(snapshot));
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
        const uint64_t frame = g_state->frameCounter;

        completePendingBatch(*g_state, frame);
        if (!g_state->pendingBatch.has_value()) {
            applyNextBatch(*g_state, frame);
        }
        publishSnapshot(*g_state, frame);

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
