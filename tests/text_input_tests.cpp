/*
  A text action on a window whose text input was never started, end to end
  through the public API: JSON-RPC on pipes, the session, the input queue,
  SDL3MCP_AppIterate on the app thread, SDL's event queue, the app's poll, and
  the frame the call returns.

  The harness is input_tool_tests': the dummy video driver, the client on its own
  thread because sdl_input blocks until the app applies the batch, confirms it
  at F+1, and answers the capture request, and the main thread iterating,
  polling, and writing a known frame whenever one is requested. The difference
  from input_tool_tests is that this test never calls SDL_StartTextInput, so the
  library has to start text input for the batch itself and stop it again. The
  text events prove the first half happened; SDL_TextInputActive staying false
  at every point the app can observe proves the state was restored.
*/

#include <SDL3MCP/SDL3MCP.h>

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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

constexpr int kFrameWidth = 8;
constexpr int kFrameHeight = 8;
constexpr std::array<uint8_t, 4> kFrameColor{1, 2, 3, 255};

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

/*
  Reads framed messages from the server: one line at a time, or exactly the
  announced number of raw frame bytes, with the remainder kept for the next
  call so the payload never bleeds into the following line.
*/
struct MessageReader
{
    int fd = -1;
    std::string pending;

    void fill(std::chrono::steady_clock::time_point deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        CHECK(remaining > 0ms);

        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) {
            return;
        }
        CHECK(ready > 0);

        char chunk[512];
        const ssize_t count = ::read(fd, chunk, sizeof chunk);
        CHECK(count > 0);
        pending.append(chunk, static_cast<std::size_t>(count));
    }

    std::string readLine(std::chrono::milliseconds timeout = 10000ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const std::size_t newline = pending.find('\n');
            if (newline != std::string::npos) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                return line;
            }
            fill(deadline);
        }
    }

    void readExact(std::size_t count)
    {
        const std::size_t buffered = std::min(count, pending.size());
        pending.erase(0, buffered);
        std::size_t consumed = buffered;
        const auto deadline = std::chrono::steady_clock::now() + 10000ms;
        while (consumed < count) {
            if (pending.empty()) {
                fill(deadline);
            }
            const std::size_t take = std::min(count - consumed, pending.size());
            pending.erase(0, take);
            consumed += take;
        }
    }
};

/* One input result: the text naming the applied frame plus exactly one payload. */
nlohmann::json readFramed(MessageReader &reader, int id)
{
    const nlohmann::json message = nlohmann::json::parse(reader.readLine());
    CHECK(message["jsonrpc"] == "2.0");
    CHECK(message["id"] == id);
    CHECK(message.contains("result"));

    const nlohmann::json &result = message["result"];
    CHECK(result["format"] == "rgba8888");
    CHECK(result["text"].is_string());
    CHECK(result["text"].get<std::string>().find("actions applied at frame ") !=
          std::string::npos);
    const std::size_t byteLength = result["byteLength"].get<std::size_t>();
    CHECK(byteLength == static_cast<std::size_t>(result["width"].get<int>()) *
                            static_cast<std::size_t>(result["height"].get<int>()) * 4);
    reader.readExact(byteLength);
    return result;
}

nlohmann::json textAction(const std::string &text)
{
    return nlohmann::json{{"actions", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}};
}

void sendTextInput(int requestsFd, int id, const std::string &text)
{
    writeAll(requestsFd, nlohmann::json{
                             {"jsonrpc", "2.0"},
                             {"id", id},
                             {"method", "tools/call"},
                             {"params", nlohmann::json{{"name", "sdl_input"},
                                                       {"arguments", textAction(text)}}},
                         }
                             .dump() +
                             "\n");
}

/* What the app saw when it drained SDL's queue. */
struct Recorder
{
    std::vector<std::string> textInputs;

    void record(const SDL_Event &event)
    {
        if (event.type == SDL_EVENT_TEXT_INPUT) {
            /* Copy now: the batch that owns this string dies at F+1. */
            textInputs.emplace_back(event.text.text);
        }
    }
};

void writeFrameIfRequested()
{
    if (!SDL3MCP_FrameRequested()) {
        return;
    }

    std::vector<uint8_t> pixels(static_cast<std::size_t>(kFrameWidth) * kFrameHeight * 4);
    for (std::size_t index = 0; index < pixels.size(); index += 4) {
        pixels[index] = kFrameColor[0];
        pixels[index + 1] = kFrameColor[1];
        pixels[index + 2] = kFrameColor[2];
        pixels[index + 3] = kFrameColor[3];
    }
    SDL3MCP_WriteFrame(pixels.data(), pixels.size(), kFrameWidth, kFrameHeight);
}

/* The client half: initialize, then one text action per call, asserting each reply. */
void runClient(int requestsFd, MessageReader &reader)
{
    writeAll(requestsFd, R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
                         R"("params":{"protocolVersion":"2025-11-25"}})" "\n");
    const nlohmann::json initialized = nlohmann::json::parse(reader.readLine());
    CHECK(initialized["id"] == 1);
    CHECK(initialized["result"]["protocolVersion"] == "2025-11-25");

    writeAll(requestsFd, R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");

    sendTextInput(requestsFd, 2, "hi");
    const nlohmann::json first = readFramed(reader, 2);
    CHECK(first["width"] == kFrameWidth);

    sendTextInput(requestsFd, 3, "ok");
    const nlohmann::json second = readFramed(reader, 3);
    CHECK(second["width"] == kFrameWidth);
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

    SDL_Window *window = SDL_CreateWindow("SDL3MCP text input", 100, 100, 0);
    CHECK(window != nullptr);
    /* The point of the test: text input stays untouched before the library runs. */
    CHECK(!SDL_TextInputActive(window));

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

    MessageReader reader;
    reader.fd = responses.readFd;

    std::atomic<bool> clientDone{false};
    std::thread client([&requests, &reader, &clientDone] {
        runClient(requests.writeFd, reader);
        clientDone.store(true);
    });

    Recorder recorder;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!clientDone.load() && std::chrono::steady_clock::now() < deadline) {
        writeFrameIfRequested();
        CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
        const std::size_t seen = recorder.textInputs.size();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            recorder.record(event);
        }
        /* A text event can only arrive after the library restored the state. */
        if (recorder.textInputs.size() != seen) {
            CHECK(!SDL_TextInputActive(window));
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

    CHECK(recorder.textInputs.size() == 2);
    CHECK(recorder.textInputs[0] == "hi");
    CHECK(recorder.textInputs[1] == "ok");
    CHECK(!SDL_TextInputActive(window));

    SDL3MCP_AppShutdown();

    CHECK(::dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO);
    CHECK(::dup2(savedStdout, STDOUT_FILENO) == STDOUT_FILENO);
    ::close(savedStdin);
    ::close(savedStdout);

    SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("text_input_tests passed\n");
    return 0;
}
