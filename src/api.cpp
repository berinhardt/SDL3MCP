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
  thread turns a taken batch into SDL events in AppIterate. sdl_capture waits on
  the CaptureState the server marks pending and the app answers with a copied
  frame; sdl_info is answered from a copied snapshot.

  A browser has no second thread, so the same requests are split across frames
  instead of across threads: the pump at the top of AppIterate parses a call,
  registers what it needs on the app's own thread (CaptureState::begin for a
  frame, InputQueue::post for a batch), and a later AppIterate answers it through
  the same Responder and the same framing. Nothing on that path waits for a frame
  the same thread has yet to render.
*/

#include <SDL3MCP/SDL3MCP.h>

#include "app_input.hpp"
#include "capture.hpp"
#include "capture_arguments.hpp"
#include "configuration.hpp"
#include "frame.hpp"
#include "info.hpp"
#include "input_json.hpp"
#include "keymap.hpp"
#include "mouse.hpp"
#include "queue.hpp"
#include "replay.hpp"
#include "session.hpp"
#ifdef __EMSCRIPTEN__
/* The browser endpoint: one WebSocket, and the responder that answers on it. */
#include "responder.hpp"
#include "websocket_peer.hpp"
#else
#include "server.hpp"
#include "socket_connect.hpp"
#include "stdio_isolation.hpp"
#endif

#include <algorithm>
#include <array>
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
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#include <unistd.h>

namespace
{

using sdl3mcp::AppliedBatchPlan;
using sdl3mcp::CaptureRegion;
using sdl3mcp::CaptureState;
using sdl3mcp::CaptureStats;
using sdl3mcp::Configuration;
using sdl3mcp::Frame;
using sdl3mcp::InfoSnapshot;
using sdl3mcp::InputAction;
using sdl3mcp::InputBatch;
using sdl3mcp::InputOutcome;
using sdl3mcp::InputQueue;
using sdl3mcp::InputStatus;
using sdl3mcp::KeyMap;
using sdl3mcp::KeyReplayer;
using sdl3mcp::MouseButton;
using sdl3mcp::MouseMetrics;
using sdl3mcp::MouseReplayer;
#ifdef __EMSCRIPTEN__
using sdl3mcp::Responder;
using sdl3mcp::WebSocketPeer;
#else
using sdl3mcp::Server;
using sdl3mcp::StdioIsolation;
#endif
using sdl3mcp::Session;
using sdl3mcp::TakenBatch;
using sdl3mcp::TextInput;
using sdl3mcp::ToolCall;
using sdl3mcp::ToolHandler;
using sdl3mcp::ToolResult;
using sdl3mcp::WindowInfo;

constexpr const char *kToolStubMessage = "tool execution is not implemented yet";

#ifdef __EMSCRIPTEN__
/*
  What a deferred tool call is waiting for. The browser answers one call at a
  time, because a capture and a batch both borrow the same frame loop.
*/
enum class DeferredRequest
{
    Capture,
    Input
};

/*
  A tool call the pump could not answer. The call is kept whole so the answer is
  framed exactly as the desktop path frames it, and the request the call needs is
  registered on the app's own thread. `started` is what sdl_info reports as the
  capture time; `deadline` is the budget the request was given.
*/
struct DeferredCall
{
    ToolCall call;
    DeferredRequest request = DeferredRequest::Capture;
    /* The region parsed when the call arrived; absent means the whole frame. */
    std::optional<CaptureRegion> region;
    /* InputQueue::post()'s sequence number; input requests only. */
    uint64_t token = 0;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point deadline;
};
#endif

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
    CaptureState capture;
    Session session;
#ifndef __EMSCRIPTEN__
    StdioIsolation isolation;

    /* The connected socket in CONNECT mode; -1 otherwise. */
    int connectFd = -1;
#endif

    /*
      Set by whichever endpoint answers sdl_terminate and exchanged by the app
      thread, which owns the only SDL call (SDL_PushEvent).
    */
    std::atomic<bool> terminateRequested{false};

#ifndef __EMSCRIPTEN__
    /* Present only while the stdio server runs; owns its thread. */
    std::unique_ptr<Server> server;
#else
    /*
      The browser's whole endpoint: the socket, the responder that reads and
      writes it, and the one call waiting for a frame or a batch. All three run
      on the app thread, and they are destroyed in this order, so the responder
      goes before the peer it writes to.
    */
    WebSocketPeer peer;
    std::unique_ptr<Responder> responder;
    std::optional<DeferredCall> deferred;
#endif

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

    /*
      Any thread: returns a copy that outlives both locks. The app-written
      window/frame fields and the server-written capture statistics have
      separate mutexes, so they are copied one after the other rather than
      under one lock that both threads would have to share.
    */
    InfoSnapshot snapshotInfo() const
    {
        InfoSnapshot copy;
        {
            std::lock_guard<std::mutex> lock(infoMutex);
            copy = info;
        }

        const CaptureStats stats = capture.stats();
        copy.capture.pending = stats.pending;
        copy.capture.count = stats.count;
        copy.capture.dropped = stats.dropped;
        copy.capture.lastWidth = stats.lastWidth;
        copy.capture.lastHeight = stats.lastHeight;
        copy.capture.lastBytes = stats.lastBytes;
        copy.capture.lastMs = stats.lastMs;
        copy.capture.lastError = stats.lastError;
        return copy;
    }
};

std::unique_ptr<LibraryState> g_state;

/* Set by the first AppInit and never cleared: one lifecycle per process. */
std::atomic<bool> g_lifecycleClaimed{false};

void writeToStderr(const std::string &text)
{
    std::fprintf(stderr, "%s\n", text.c_str());
}

#ifdef __EMSCRIPTEN__
/*
  A page has no command line, so the query string is the only place a server can
  be named. Reading it yields the same --mcp=<value> argument the desktop build
  would parse, which keeps one parser and one validator on both platforms.
*/

/* -1 for anything that is not a hexadecimal digit. */
int hexDigitValue(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

/*
  Decodes %XX escapes: a browser writes HOST:PORT as HOST%3APORT. An incomplete
  escape is copied through unchanged, so the configuration parser rejects the
  value with its own message instead of this helper inventing one.
*/
std::string percentDecode(std::string_view encoded)
{
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '%' && index + 2 < encoded.size()) {
            const int high = hexDigitValue(encoded[index + 1]);
            const int low = hexDigitValue(encoded[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>(high * 16 + low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(encoded[index]);
    }
    return decoded;
}

/* The decoded value of the query's mcp parameter; absent when the page has none. */
std::optional<std::string> mcpQueryValue()
{
    const char *search = emscripten_run_script_string("window.location.search");
    std::string_view query = search != nullptr ? std::string_view(search) : std::string_view();
    if (!query.empty() && query.front() == '?') {
        query.remove_prefix(1);
    }

    while (!query.empty()) {
        const std::size_t separator = query.find('&');
        const std::string_view parameter = query.substr(0, separator);
        query = separator == std::string_view::npos ? std::string_view()
                                                    : query.substr(separator + 1);

        const std::size_t equals = parameter.find('=');
        if (parameter.substr(0, equals) != "mcp") {
            continue;
        }
        return equals == std::string_view::npos
                   ? std::string()
                   : percentDecode(parameter.substr(equals + 1));
    }
    return std::nullopt;
}

/*
  Commits the page's configuration. No mcp parameter means the page asked for no
  server, so the values the caller supplied are only validated.
*/
Configuration::Status applyPageQuery(Configuration &configuration)
{
    const std::optional<std::string> value = mcpQueryValue();
    if (!value.has_value()) {
        return configuration.validate();
    }

    char program[] = "catalyst";
    std::string argument = "--mcp=" + *value;
    std::array<char *, 2> argv{program, argument.data()};
    return configuration.parseCommandLine(static_cast<int>(argv.size()), argv.data());
}
#endif

void writeCommandLineError(const std::string &message)
{
#ifdef __EMSCRIPTEN__
    /* The browser console gets the diagnosis; there is no usage listing there. */
    std::fprintf(stderr, "SDL3MCP: %s\n", message.c_str());
#else
    std::fprintf(stderr, "SDL3MCP: %s\n%s\n",
                 message.c_str(),
                 Configuration::usageText().c_str());
#endif
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
  App thread only. True when any action in the batch carries text, so the batch
  needs text input active to be applied.
*/
bool batchHasText(const InputBatch &batch)
{
    return std::any_of(batch.actions.begin(), batch.actions.end(), [](const InputAction &action) {
        return std::holds_alternative<TextInput>(action);
    });
}

/*
  App thread only. Starts text input for the lifetime of one batch, because
  synthetic text needs no keyboard focus, and stops it again only when this
  scope started it, so the previous state is restored on every exit path.
*/
class TextInputScope
{
public:
    TextInputScope(SDL_Window *window, bool needed)
        : window_(window)
    {
        if (needed && window_ != nullptr && !SDL_TextInputActive(window_)) {
            started_ = SDL_StartTextInput(window_);
        }
    }

    ~TextInputScope()
    {
        if (started_) {
            SDL_StopTextInput(window_);
        }
    }

    TextInputScope(const TextInputScope &) = delete;
    TextInputScope &operator=(const TextInputScope &) = delete;

private:
    SDL_Window *window_ = nullptr;
    bool started_ = false;
};

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

    TextInputScope textInput(state.window, batchHasText(pending.batch));
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

/*
  The text a capture answers with when no frame arrived in time. `failure` is
  the first dropped frame's reason when the app tried and its data was rejected;
  the browser path passes nothing, because it has no wait to attribute a drop to.
*/
std::string frameTimeoutText(std::chrono::milliseconds timeout, const std::string &failure)
{
    std::string text = "no frame arrived within " + std::to_string(timeout.count()) +
                       " ms. The app must call SDL3MCP_WriteFrame() when "
                       "SDL3MCP_FrameRequested() returns true, with tightly packed "
                       "RGBA8888, its width, and its height.";
    if (!failure.empty()) {
        text += " Last drop: " + failure + ".";
    }
    return text;
}

#ifndef __EMSCRIPTEN__
/*
  Server thread: what is left of a per-call budget after `started`. A spent
  budget is zero, never negative, so it can be handed to a blocking wait.
*/
std::chrono::milliseconds remainingBudget(std::chrono::steady_clock::time_point started,
                                          std::chrono::milliseconds budget)
{
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return spent >= budget ? std::chrono::milliseconds(0) : budget - spent;
}

/*
  Server thread: one request, one cropped frame. Both sdl_capture and the
  observation half of sdl_input go through here, so the two report a missing
  frame the same way and count the same statistics.
*/
bool takeFrame(LibraryState &state,
               std::chrono::milliseconds timeout,
               const std::optional<CaptureRegion> &region,
               Frame &frame,
               std::string &error)
{
    if (!state.snapshotInfo().window.bound) {
        error = "no window bound";
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    if (!state.capture.begin(error)) {
        return false;
    }

    std::string failure;
    if (!state.capture.wait(timeout, frame, failure)) {
        error = frameTimeoutText(timeout, failure);
        state.capture.recordError(error);
        return false;
    }

    if (region.has_value()) {
        const std::optional<Frame> cropped = sdl3mcp::cropFrame(frame, *region, error);
        if (!cropped.has_value()) {
            state.capture.recordError(error);
            return false;
        }
        frame = std::move(*cropped);
    }

    /* The statistics describe the raw pixels this handler hands to the server. */
    const double elapsedMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
    state.capture.updateSuccess(frame.width, frame.height, frame.pixels.size(), elapsedMs);
    return true;
}

/* Server thread: parses the arguments, queues the batch, and waits for the app. */
ToolResult inputResult(LibraryState &state, const ToolCall &call)
{
    const auto started = std::chrono::steady_clock::now();

    std::string error;
    std::optional<sdl3mcp::InputRequest> request = sdl3mcp::parseInputArguments(
        call.arguments, state.configuration.value().frame_timeout_ms, error);
    if (!request.has_value()) {
        return textResult(std::move(error), true);
    }

    const std::size_t actionCount = request->batch.actions.size();
    InputOutcome outcome;
    state.inputQueue.submit(std::move(request->batch),
                            remainingBudget(started, request->timeout), outcome);

    switch (outcome.status) {
    case InputStatus::Applied:
        break;
    case InputStatus::AppliedUnconfirmed:
        return textResult("applied at frame " + std::to_string(outcome.appliedFrame) +
                              " but not confirmed before the timeout; no frame was taken",
                          true);
    case InputStatus::Cancelled:
        return textResult("input not applied; is SDL3MCP_AppIterate called every loop?", true);
    case InputStatus::Failed:
        return textResult(outcome.error, true);
    case InputStatus::Stopped:
        return textResult("SDL3MCP is shutting down", true);
    }

    const std::string appliedText =
        std::to_string(actionCount) + " actions applied at frame " +
        std::to_string(outcome.appliedFrame);

    /*
      The whole call, apply included, is bounded by timeout_ms, so the capture
      gets what is left. The input already reached the app: a frame that cannot
      be taken is reported as a missing observation, never as a failed batch.
    */
    const std::chrono::milliseconds remaining = remainingBudget(started, request->timeout);
    if (remaining.count() <= 0) {
        return textResult(appliedText + " but no frame was taken: the timeout_ms budget ran "
                                        "out before the capture",
                          true);
    }

    Frame frame;
    std::string failure;
    if (!takeFrame(state, remaining, request->region, frame, failure)) {
        return textResult(appliedText + " but no frame was taken: " + failure, true);
    }

    /*
      The acknowledgement waits for the F+1 completion before this capture is
      requested, so the attached frame is always rendered after the actions were
      delivered. Naming both indices makes that a fact the caller can act on: an
      unchanged frame then means the actions had no effect, not that it is stale.
    */
    if (const std::optional<std::string> order =
            sdl3mcp::checkFrameOrder(outcome.appliedFrame, frame.frame);
        order.has_value()) {
        return textResult(*order, true);
    }

    ToolResult result;
    result.frameText = appliedText + "; frame " + std::to_string(frame.frame) +
                       " attached, rendered after the actions were delivered";
    result.frame = std::move(frame);
    return result;
}
#endif /* __EMSCRIPTEN__ */

constexpr std::array<std::string_view, 2> kCaptureProperties{"timeout_ms", "region"};

/* The sdl_capture arguments as the handler uses them, already range-checked. */
struct CaptureArguments
{
    std::chrono::milliseconds timeout{0};
    std::optional<CaptureRegion> region;
};

template <std::size_t N>
std::optional<std::string> rejectUnknownProperties(const nlohmann::json &value,
                                                   const std::array<std::string_view, N> &allowed)
{
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
        const std::string_view name = entry.key();
        bool isAllowed = false;
        for (const std::string_view candidate : allowed) {
            if (name == candidate) {
                isAllowed = true;
                break;
            }
        }
        if (!isAllowed) {
            return "unknown property \"" + entry.key() + "\"";
        }
    }
    return std::nullopt;
}

/*
  Server thread: validates the wire shape before anything is requested. Every
  property the schema does not name is rejected, so a typo cannot silently fall
  back to a default; ranges match the published tool schema. The region and
  timeout rules themselves live in capture_arguments.cpp, shared with sdl_input.
*/
std::optional<CaptureArguments> parseCaptureArguments(const nlohmann::json &arguments,
                                                      uint32_t defaultTimeoutMs,
                                                      std::string &error)
{
    error.clear();

    if (!arguments.is_object()) {
        error = "arguments must be a JSON object";
        return std::nullopt;
    }
    if (const std::optional<std::string> unknown =
            rejectUnknownProperties(arguments, kCaptureProperties);
        unknown.has_value()) {
        error = *unknown;
        return std::nullopt;
    }

    CaptureArguments parsed;
    if (!sdl3mcp::parseTimeoutArgument(arguments,
                                       std::chrono::milliseconds(defaultTimeoutMs),
                                       parsed.timeout, error)) {
        return std::nullopt;
    }
    if (!sdl3mcp::parseRegionArgument(arguments, parsed.region, error)) {
        return std::nullopt;
    }
    return parsed;
}

#ifndef __EMSCRIPTEN__
/* Server thread: one request, one cropped frame, then raw RGBA framing. */
ToolResult captureResult(LibraryState &state, const ToolCall &call)
{
    std::string error;
    const std::optional<CaptureArguments> arguments = parseCaptureArguments(
        call.arguments, state.configuration.value().frame_timeout_ms, error);
    if (!arguments.has_value()) {
        return textResult(std::move(error), true);
    }

    Frame frame;
    if (!takeFrame(state, arguments->timeout, arguments->region, frame, error)) {
        return textResult(std::move(error), true);
    }

    ToolResult result;
    result.frame = std::move(frame);
    return result;
}

/*
  The server thread dispatcher. sdl_info answers from the published copy,
  sdl_input blocks in the queue, and sdl_capture waits on the capture state.
  Only those queues cross the boundary, never SDL.
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
        if (call.name == "sdl_capture") {
            return captureResult(*state, call);
        }
        if (call.name == "sdl_terminate") {
            state->terminateRequested.store(true);
            return textResult("termination requested", false);
        }
        return textResult(kToolStubMessage, true);
    };
}
#else /* __EMSCRIPTEN__ */
/*
  App thread, called from the pump. Registers the request the call needs and
  returns a deferred result, which the responder leaves unanswered: the frame or
  the batch has to exist first, and this thread cannot wait for itself. One call
  at a time, so a capture and a batch never race for the same frame.
*/
ToolResult deferToolCall(LibraryState &state, const ToolCall &call)
{
    if (state.deferred.has_value()) {
        return textResult(state.deferred->request == DeferredRequest::Capture
                              ? "a capture request is already pending"
                              : "an input request is already pending",
                          true);
    }

    const SDL3MCP_Configuration &configuration = state.configuration.value();
    DeferredCall deferred;
    deferred.call = call;
    deferred.started = std::chrono::steady_clock::now();
    deferred.deadline =
        deferred.started + std::chrono::milliseconds(configuration.frame_timeout_ms);

    std::string error;
    if (call.name == "sdl_capture") {
        /* Validated here, so a bad document is answered now and not by a frame. */
        const std::optional<CaptureArguments> arguments =
            parseCaptureArguments(call.arguments, configuration.frame_timeout_ms, error);
        if (!arguments.has_value()) {
            return textResult(std::move(error), true);
        }
        /* Same gate as the desktop path: no window, no frame to ask the app for. */
        if (!state.snapshotInfo().window.bound) {
            return textResult("no window bound", true);
        }
        deferred.request = DeferredRequest::Capture;
        deferred.region = arguments->region;
        if (!state.capture.begin(error)) {
            return textResult(std::move(error), true);
        }
    } else {
        /* The whole document is parsed, so both transports accept the same calls. */
        std::optional<sdl3mcp::InputRequest> request = sdl3mcp::parseInputArguments(
            call.arguments, configuration.frame_timeout_ms, error);
        if (!request.has_value()) {
            return textResult(std::move(error), true);
        }
        deferred.request = DeferredRequest::Input;
        if (!state.inputQueue.post(std::move(request->batch), deferred.token, error)) {
            return textResult(std::move(error), true);
        }
    }

    state.deferred = std::move(deferred);
    ToolResult result;
    result.deferred = true;
    return result;
}

/*
  App thread. The browser's dispatcher. sdl_info and sdl_terminate need nothing
  from the frame loop and are answered here; sdl_capture and sdl_input are
  registered and answered by a later AppIterate.
*/
ToolHandler makePeerToolHandler(LibraryState *state)
{
    return [state](const ToolCall &call) {
        if (call.name == "sdl_info") {
            return textResult(sdl3mcp::infoToJson(state->snapshotInfo()).dump(2), false);
        }
        if (call.name == "sdl_terminate") {
            state->terminateRequested.store(true);
            return textResult("termination requested", false);
        }
        if (call.name == "sdl_capture" || call.name == "sdl_input") {
            return deferToolCall(*state, call);
        }
        return textResult(kToolStubMessage, true);
    };
}

/*
  App thread. Drains the browser's byte queue before the frame counter, so a
  request registered here is visible to this frame's SDL3MCP_FrameRequested()
  and answered by the frame the app produces afterwards.
*/
void pumpPeer(LibraryState &state)
{
    if (!state.responder) {
        return;
    }

    std::string error;
    if (state.peer.pump(*state.responder, error)) {
        return;
    }

    /*
      The endpoint is gone. A browser app outlives its debug client, so this is
      not an application failure; nothing can be answered any more, and a frame
      request left pending would keep asking for frames nobody reads.
    */
    if (state.deferred.has_value() && state.deferred->request == DeferredRequest::Capture) {
        state.capture.cancel();
    }
    state.deferred.reset();
}

/*
  App thread. One settled batch, as the browser answers it. There is no frame to
  attach: the deferred call ends with the batch, and a frame is a separate
  sdl_capture.
*/
ToolResult inputOutcomeResult(const InputOutcome &outcome)
{
    switch (outcome.status) {
    case InputStatus::Applied:
        return textResult("input applied at frame " + std::to_string(outcome.appliedFrame) +
                              " but no frame was taken: the browser transport answers a frame "
                              "with a separate sdl_capture call",
                          false);
    case InputStatus::AppliedUnconfirmed:
        return textResult("applied at frame " + std::to_string(outcome.appliedFrame) +
                              " but not confirmed before the timeout; no frame was taken",
                          true);
    case InputStatus::Cancelled:
        return textResult("input not applied; is SDL3MCP_AppIterate called every loop?", true);
    case InputStatus::Failed:
        return textResult(outcome.error, true);
    case InputStatus::Stopped:
        return textResult("SDL3MCP is shutting down", true);
    }
    return textResult("input ended in an unknown state", true);
}

/* True when the deferred capture was answered, with `result` filled in. */
bool completeDeferredCapture(LibraryState &state,
                             const DeferredCall &deferred,
                             ToolResult &result)
{
    Frame frame;
    if (state.capture.tryTake(frame)) {
        if (deferred.region.has_value()) {
            std::string error;
            const std::optional<Frame> cropped =
                sdl3mcp::cropFrame(frame, *deferred.region, error);
            if (!cropped.has_value()) {
                state.capture.recordError(error);
                result = textResult(std::move(error), true);
                return true;
            }
            frame = std::move(*cropped);
        }

        const double elapsedMs = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - deferred.started)
                                     .count();
        state.capture.updateSuccess(frame.width, frame.height, frame.pixels.size(), elapsedMs);
        result = ToolResult{};
        result.frame = std::move(frame);
        return true;
    }

    if (std::chrono::steady_clock::now() < deferred.deadline) {
        return false;
    }

    /* The deadline passed: clear the request so the next call can ask again. */
    const std::chrono::milliseconds budget = std::chrono::duration_cast<std::chrono::milliseconds>(
        deferred.deadline - deferred.started);
    state.capture.cancel();
    const std::string timeout = frameTimeoutText(budget, std::string());
    state.capture.recordError(timeout);
    result = textResult(timeout, true);
    return true;
}

/*
  App thread. Answers the deferred call once its request can be met: a frame
  taken after it was registered, or a batch that has settled. Nothing here
  waits; an unfinished request simply stays deferred for the next frame.
*/
void completeDeferredCall(LibraryState &state)
{
    if (!state.deferred.has_value()) {
        return;
    }

    const DeferredCall &deferred = *state.deferred;
    ToolResult result;
    if (deferred.request == DeferredRequest::Capture) {
        if (!completeDeferredCapture(state, deferred, result)) {
            return;
        }
    } else {
        InputOutcome outcome;
        if (!state.inputQueue.poll(deferred.token, outcome)) {
            return;
        }
        result = inputOutcomeResult(outcome);
    }

    /* Clear the stored call before writing, so no reference into it survives. */
    const ToolCall call = deferred.call;
    state.deferred.reset();
    if (state.responder) {
        std::string error;
        state.responder->answer(call, result, error);
    }
}
#endif /* __EMSCRIPTEN__ */

/*
  Wakes a waiting sdl_capture and stops the queue before the endpoint so both
  blocked handlers return, then joins the reader, then hands stdin/stdout back.
  On the browser the same three steps release the frame request and the batch,
  and the peer is stopped instead of a server thread being joined. Safe with no
  state, and safe when startup failed before the endpoint existed.
*/
void teardownState() noexcept
{
    if (g_state) {
        try {
            g_state->capture.cancel();
            g_state->inputQueue.stop();
            releaseHeldInput(*g_state);
#ifndef __EMSCRIPTEN__
            if (g_state->server) {
                g_state->server->stop();
                g_state->server->join();
            }
            if (g_state->connectFd >= 0) {
                ::close(g_state->connectFd);
                g_state->connectFd = -1;
            }
            g_state->isolation.restore();
#else
            g_state->peer.stop();
            g_state->deferred.reset();
            g_state->responder.reset();
#endif
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

        SDL3MCP_Configuration base =
            config != nullptr ? *config : SDL3MCP_DefaultConfiguration();
#ifdef __EMSCRIPTEN__
        /*
          A page has no command line, so a transport in the caller's value cannot
          name a server: only the query string can, and its absence means none.
        */
        base.transport = SDL3MCP_TRANSPORT_NONE;
#endif

        Configuration configuration(base);
#ifdef __EMSCRIPTEN__
        const Configuration::Status status = applyPageQuery(configuration);
#else
        const Configuration::Status status = configuration.parseCommandLine(argc, argv);
#endif
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

#ifndef __EMSCRIPTEN__
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
        } else if (g_state->configuration.value().transport == SDL3MCP_TRANSPORT_CONNECT) {
            /*
              One dial, no retry. Storing the descriptor before start() lets the
              exception path and teardownState close it exactly once.
            */
            const SDL3MCP_Configuration &configuration = g_state->configuration.value();
            std::string error;
            int fd = -1;
            if (!sdl3mcp::connectSocket(configuration.connect_host, configuration.connect_port,
                                        fd, error)) {
                g_state.reset();
                SDL_SetError("%s", error.c_str());
                return SDL_APP_FAILURE;
            }
            g_state->connectFd = fd;

            auto server = std::make_unique<Server>(fd, fd, g_state->session,
                                                   makeToolHandler(g_state.get()));
            if (!server->start(error)) {
                ::close(g_state->connectFd);
                g_state->connectFd = -1;
                g_state.reset();
                SDL_SetError("%s", error.c_str());
                return SDL_APP_FAILURE;
            }
            g_state->server = std::move(server);
        }
#else
        /*
          The page's only server is the endpoint it named. The dial completes in
          the browser, so a URL the browser refuses fails here and a connection
          that never opens surfaces through the peer instead of blocking startup.
        */
        if (g_state->configuration.value().transport == SDL3MCP_TRANSPORT_WEBSOCKET) {
            const SDL3MCP_Configuration &configuration = g_state->configuration.value();
            std::string error;
            if (!g_state->peer.open(configuration.connect_host, configuration.connect_port,
                                    error)) {
                g_state.reset();
                SDL_SetError("%s", error.c_str());
                return SDL_APP_FAILURE;
            }
            g_state->responder = std::make_unique<Responder>(
                g_state->peer, g_state->session, makePeerToolHandler(g_state.get()));
        }
#endif

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
#ifndef __EMSCRIPTEN__
        if (g_state->server && g_state->server->failed()) {
            const std::string error = g_state->server->lastError();
            SDL_SetError("%s", error.empty() ? "the MCP server thread failed" : error.c_str());
            return SDL_APP_FAILURE;
        }
#else
        /* Before the frame counter, so a request registered here is answered by
           a frame this loop has not produced yet. */
        pumpPeer(*g_state);
#endif

        ++g_state->frameCounter;
        const uint64_t frame = g_state->frameCounter;

        completePendingBatch(*g_state, frame);
        if (!g_state->pendingBatch.has_value()) {
            applyNextBatch(*g_state, frame);
        }
        publishSnapshot(*g_state, frame);
#ifdef __EMSCRIPTEN__
        /* After the frame work, so the frame the app answered this frame's
           request with is the one a completed capture carries. */
        completeDeferredCall(*g_state);
#endif

        /*
          One quit request makes one SDL_EVENT_QUIT. A failed push restores the
          flag so the next iterate retries instead of losing the request.
        */
        if (g_state->terminateRequested.exchange(false)) {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            if (!SDL_PushEvent(&event)) {
                g_state->terminateRequested.store(true);
            }
        }

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
        if (!g_state || g_state->window == nullptr) {
            return false;
        }
#ifndef __EMSCRIPTEN__
        if (!g_state->server) {
            return false;
        }
#else
        /* The browser has no Server: the peer is the endpoint, and a peer that
           closed cannot answer a frame anyway. */
        if (!g_state->responder || g_state->peer.failed()) {
            return false;
        }
#endif
        /*
          Decline the poll before the app has completed a frame worth answering:
          the request stays pending, so the next servable frame answers it.
        */
        if (!sdl3mcp::servableFrame(g_state->frameCounter)) {
            return false;
        }
        return g_state->capture.pending();
    } catch (...) {
        return false;
    }
}

extern "C" SDL_DECLSPEC void SDLCALL
SDL3MCP_WriteFrame(const uint8_t *data, size_t size, int width, int height)
{
    try {
        if (!g_state || !g_state->capture.pending()) {
            return;
        }

        if (const std::optional<std::string> error = sdl3mcp::validateFrame(
                data, size, width, height, g_state->configuration.value().max_frame_bytes);
            error.has_value()) {
            /* Keep the request pending: the app may retry on the next frame. */
            g_state->capture.recordFailure(*error);
            return;
        }

        Frame frame;
        frame.width = width;
        frame.height = height;
        frame.frame = g_state->frameCounter + 1;
        frame.pixels.assign(data, data + size);
        g_state->capture.submit(std::move(frame));
    } catch (...) {
        /* The C boundary never throws; a lost frame becomes a capture timeout. */
    }
}
