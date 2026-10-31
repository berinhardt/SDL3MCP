/*
  The input path end to end through the public API: JSON-RPC on pipes, the
  session, the input queue, SDL3MCP_AppIterate on the app thread, SDL's event
  queue, the app's poll, and sdl_info answering from the published snapshot.

  The video driver is the dummy one, so the test claims no display and sees no
  real input. The client runs on its own thread because sdl_input blocks until
  the app applies the batch and confirms it at F+1; the main thread is the app,
  iterating and polling exactly as a game loop would.

  Text is the interesting lifetime case: SDL's text event borrows a string owned
  by the batch, which AppIterate frees one frame later. The recorder copies the
  string at poll time, which is the contract every app has to follow too.
*/

#include <SDL3MCP/SDL3MCP.h>

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;

/* A pipe pair that closes both ends on destruction. */
struct Pipe
{
    int readFd = -1;
    int writeFd = -1;

    Pipe()
    {
        int descriptors[2] = {-1, -1};
        CHECK(::pipe(descriptors) == 0);
        readFd = descriptors[0];
        writeFd = descriptors[1];
    }

    ~Pipe()
    {
        if (readFd >= 0) {
            ::close(readFd);
        }
        if (writeFd >= 0) {
            ::close(writeFd);
        }
    }

    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;
};

void writeAll(int fd, std::string_view data)
{
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(fd, data.data() + written, data.size() - written);
        CHECK(count > 0);
        written += static_cast<std::size_t>(count);
    }
}

/* Reads one newline-terminated line, failing instead of hanging on a dead server. */
std::string readLine(int fd, std::chrono::milliseconds timeout = 10000ms)
{
    std::string pending;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        const std::size_t newline = pending.find('\n');
        if (newline != std::string::npos) {
            return pending.substr(0, newline);
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        CHECK(remaining > 0ms);

        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        CHECK(ready > 0);

        char chunk[512];
        const ssize_t count = ::read(fd, chunk, sizeof chunk);
        CHECK(count > 0);
        pending.append(chunk, static_cast<std::size_t>(count));
    }
}

void writeLine(int fd, const nlohmann::json &message)
{
    writeAll(fd, message.dump() + "\n");
}

nlohmann::json call(int requestsFd, int responsesFd, int id, const std::string &name,
                    nlohmann::json arguments)
{
    const nlohmann::json request{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "tools/call"},
        {"params", nlohmann::json{{"name", name}, {"arguments", std::move(arguments)}}},
    };
    writeLine(requestsFd, request);
    return nlohmann::json::parse(readLine(responsesFd));
}

/* The single text block every tool result carries here. */
std::string resultText(const nlohmann::json &response)
{
    CHECK(response.contains("result"));
    const nlohmann::json &result = response["result"];
    CHECK(result["content"].is_array());
    CHECK(result["content"].size() == 1);
    CHECK(result["content"][0]["type"] == "text");
    return result["content"][0]["text"].get<std::string>();
}

nlohmann::json actionsOf(std::initializer_list<nlohmann::json> actions)
{
    return nlohmann::json{{"actions", nlohmann::json(actions)}};
}

bool containsKey(const nlohmann::json &heldKeys, const std::string &name)
{
    for (const nlohmann::json &key : heldKeys) {
        if (key == name) {
            return true;
        }
    }
    return false;
}

/* What the app saw when it drained SDL's queue. */
struct Recorder
{
    bool sawKeyDownA = false;
    bool sawKeyUpA = false;
    std::vector<std::string> textInputs;
    std::vector<SDL_Event> motions;

    void record(const SDL_Event &event)
    {
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            sawKeyDownA = sawKeyDownA || event.key.key == SDLK_A;
            break;
        case SDL_EVENT_KEY_UP:
            sawKeyUpA = sawKeyUpA || event.key.key == SDLK_A;
            break;
        case SDL_EVENT_TEXT_INPUT:
            /* Copy now: the batch that owns this string dies at F+1. */
            textInputs.emplace_back(event.text.text);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            motions.push_back(event);
            break;
        default:
            break;
        }
    }
};

/* The client half: one ordered conversation, asserted response by response. */
void runClient(int requestsFd, int responsesFd)
{
    writeAll(requestsFd, R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
                         R"("params":{"protocolVersion":"2025-11-25"}})" "\n");
    const nlohmann::json initialized = nlohmann::json::parse(readLine(responsesFd));
    CHECK(initialized["id"] == 1);
    CHECK(initialized["result"]["protocolVersion"] == "2025-11-25");

    writeAll(requestsFd, R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");

    const nlohmann::json pressed = call(
        requestsFd, responsesFd, 2, "sdl_input", actionsOf({{{"type", "key"}, {"key", "a"}}}));
    CHECK(pressed["result"]["isError"] == false);
    CHECK(resultText(pressed).find("applied") != std::string::npos);

    const nlohmann::json held = call(
        requestsFd, responsesFd, 3, "sdl_input",
        actionsOf({{{"type", "key"}, {"key", "right"}, {"action", "down"}}}));
    CHECK(held["result"]["isError"] == false);

    const nlohmann::json heldInfo = nlohmann::json::parse(
        resultText(call(requestsFd, responsesFd, 4, "sdl_info", nlohmann::json::object())));
    CHECK(heldInfo["input"]["heldKeys"].is_array());
    CHECK(containsKey(heldInfo["input"]["heldKeys"], "right"));

    const nlohmann::json released = call(
        requestsFd, responsesFd, 5, "sdl_input", actionsOf({{{"type", "release_all"}}}));
    CHECK(released["result"]["isError"] == false);

    const nlohmann::json releasedInfo = nlohmann::json::parse(
        resultText(call(requestsFd, responsesFd, 6, "sdl_info", nlohmann::json::object())));
    CHECK(releasedInfo["input"]["heldKeys"].empty());
    CHECK(releasedInfo["input"]["heldButtons"].empty());
    CHECK(releasedInfo["appliedFrame"].get<uint64_t>() > 0);

    const nlohmann::json typed = call(
        requestsFd, responsesFd, 7, "sdl_input", actionsOf({{{"type", "text"}, {"text", "hi"}}}));
    CHECK(typed["result"]["isError"] == false);

    const nlohmann::json moved = call(
        requestsFd, responsesFd, 8, "sdl_input",
        actionsOf({{{"type", "mouse_move"}, {"x", 10}, {"y", 20}}}));
    CHECK(moved["result"]["isError"] == false);
}

} // namespace

int main()
{
    /* The CTest environment sets this too; the hint makes a manual run work. */
    (void)SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    const char *videoDriver = SDL_GetCurrentVideoDriver();
    CHECK(videoDriver != nullptr);
    CHECK(std::string_view(videoDriver) == "dummy");

    SDL_Window *window = SDL_CreateWindow("SDL3MCP input", 100, 100, 0);
    CHECK(window != nullptr);
    CHECK(SDL_StartTextInput(window));

    const int savedStdin = ::dup(STDIN_FILENO);
    const int savedStdout = ::dup(STDOUT_FILENO);
    CHECK(savedStdin >= 0);
    CHECK(savedStdout >= 0);

    Pipe requests;
    Pipe responses;
    CHECK(::dup2(requests.readFd, STDIN_FILENO) == STDIN_FILENO);
    CHECK(::dup2(responses.writeFd, STDOUT_FILENO) == STDOUT_FILENO);

    SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();
    configuration.transport = SDL3MCP_TRANSPORT_STDIO;
    /* Warping needs a real cursor; this test only checks the synthetic events. */
    configuration.warp_cursor = false;
    configuration.frame_timeout_ms = 5000;

    CHECK(SDL3MCP_AppInit(&configuration, 0, nullptr) == SDL_APP_CONTINUE);
    CHECK(SDL3MCP_SetWindow(window) == SDL_APP_CONTINUE);

    std::atomic<bool> clientDone{false};
    std::thread client([&requests, &responses, &clientDone] {
        runClient(requests.writeFd, responses.readFd);
        clientDone.store(true);
    });

    Recorder recorder;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!clientDone.load() && std::chrono::steady_clock::now() < deadline) {
        CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            recorder.record(event);
        }
        std::this_thread::sleep_for(1ms);
    }
    client.join();
    CHECK(clientDone.load());

    /* Drain anything the final batch pushed before the client's last reply. */
    CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        recorder.record(event);
    }

    CHECK(recorder.sawKeyDownA);
    CHECK(recorder.sawKeyUpA);
    CHECK(!recorder.textInputs.empty());
    CHECK(recorder.textInputs.front() == "hi");

    bool sawMove = false;
    for (const SDL_Event &motion : recorder.motions) {
        sawMove = sawMove || (motion.motion.x == 10.0F && motion.motion.y == 20.0F);
    }
    CHECK(sawMove);

    SDL3MCP_AppShutdown();

    CHECK(::dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO);
    CHECK(::dup2(savedStdout, STDOUT_FILENO) == STDOUT_FILENO);
    ::close(savedStdin);
    ::close(savedStdout);

    /* The app started text input; the library must not have stopped it. */
    CHECK(SDL_TextInputActive(window));
    SDL_StopTextInput(window);
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("input_tool_tests passed\n");
    return 0;
}
