/*
  Regression: the app, not the window, decides a frame's dimensions.

  SDL3MCP_WriteFrame takes width and height, so a frame whose size differs from
  the bound window's drawable pixels is valid. Here a 64x64 window is bound and
  the app answers sdl_capture with an 8x8 frame; the framing header and
  sdl_info's capture.lastWidth/lastHeight must both report 8x8. Before the
  dimensions were explicit, this capture was rejected with "size ... != 64*64*4"
  and the client timed out.
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
#include <vector>

namespace
{

using namespace std::chrono_literals;

constexpr int kWindowSide = 64;
constexpr int kFrameSide = 8;
constexpr std::array<uint8_t, 4> kFrameColor{10, 20, 30, 255};

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

/* Reads JSON-RPC lines and then the exact raw byte count the header announced. */
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

    std::vector<uint8_t> readExact(std::size_t count)
    {
        std::vector<uint8_t> out;

        const std::size_t buffered = std::min(count, pending.size());
        out.insert(out.end(), pending.begin(),
                   pending.begin() + static_cast<std::ptrdiff_t>(buffered));
        pending.erase(0, buffered);

        const auto deadline = std::chrono::steady_clock::now() + 10000ms;
        while (out.size() < count) {
            if (pending.empty()) {
                fill(deadline);
            }
            const std::size_t take = std::min(count - out.size(), pending.size());
            out.insert(out.end(), pending.begin(),
                       pending.begin() + static_cast<std::ptrdiff_t>(take));
            pending.erase(0, take);
        }
        return out;
    }
};

std::vector<uint8_t> framePixels(int width, int height)
{
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        pixels[offset] = kFrameColor[0];
        pixels[offset + 1] = kFrameColor[1];
        pixels[offset + 2] = kFrameColor[2];
        pixels[offset + 3] = kFrameColor[3];
    }
    return pixels;
}

nlohmann::json parseInfo(const nlohmann::json &response)
{
    CHECK(response["result"]["isError"] == false);
    const nlohmann::json &content = response["result"]["content"];
    CHECK(content.is_array());
    CHECK(content.size() == 1);
    CHECK(content[0]["type"] == "text");
    return nlohmann::json::parse(content[0]["text"].get<std::string>());
}

/* The client half: one 8x8 capture in a 64x64 window, then the statistics. */
void runClient(int requestsFd, MessageReader &reader)
{
    writeAll(requestsFd, R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
                         R"("params":{"protocolVersion":"2025-11-25"}})" "\n");
    const nlohmann::json initialized = nlohmann::json::parse(reader.readLine());
    CHECK(initialized["id"] == 1);
    CHECK(initialized["result"]["protocolVersion"] == "2025-11-25");
    writeAll(requestsFd, R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");

    writeAll(requestsFd, R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
                         R"("params":{"name":"sdl_capture","arguments":{}}})" "\n");
    const nlohmann::json captured = nlohmann::json::parse(reader.readLine());
    CHECK(captured["id"] == 2);
    const nlohmann::json &frame = captured["result"];
    CHECK(frame["format"] == "rgba8888");
    CHECK(frame["width"] == kFrameSide);
    CHECK(frame["height"] == kFrameSide);
    CHECK(frame["byteLength"] == kFrameSide * kFrameSide * 4);

    const std::vector<uint8_t> pixels =
        reader.readExact(static_cast<std::size_t>(frame["byteLength"].get<int>()));
    CHECK(pixels.size() ==
          static_cast<std::size_t>(kFrameSide) * kFrameSide * 4u);
    CHECK(pixels[0] == kFrameColor[0]);
    CHECK(pixels[3] == kFrameColor[3]);

    writeAll(requestsFd, R"({"jsonrpc":"2.0","id":3,"method":"tools/call",)"
                         R"("params":{"name":"sdl_info","arguments":{}}})" "\n");
    const nlohmann::json info = parseInfo(nlohmann::json::parse(reader.readLine()));
    CHECK(info["window"]["pixelWidth"] == kWindowSide);
    CHECK(info["window"]["pixelHeight"] == kWindowSide);
    CHECK(info["capture"]["count"].get<uint64_t>() >= 1);
    CHECK(info["capture"]["lastWidth"] == kFrameSide);
    CHECK(info["capture"]["lastHeight"] == kFrameSide);
    CHECK(info["capture"]["lastBytes"].get<uint64_t>() ==
          static_cast<uint64_t>(kFrameSide) * kFrameSide * 4u);
    CHECK(info["capture"]["pending"] == false);
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

    SDL_Window *window = SDL_CreateWindow("SDL3MCP frame size", kWindowSide,
                                          kWindowSide, 0);
    CHECK(window != nullptr);

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
    configuration.frame_timeout_ms = 5000;
    configuration.warp_cursor = false;

    CHECK(SDL3MCP_AppInit(&configuration, 0, nullptr) == SDL_APP_CONTINUE);
    CHECK(SDL3MCP_SetWindow(window) == SDL_APP_CONTINUE);

    MessageReader reader;
    reader.fd = responses.readFd;

    std::atomic<bool> clientDone{false};
    std::thread client([&requests, &reader, &clientDone] {
        runClient(requests.writeFd, reader);
        clientDone.store(true);
    });

    /* Every pending request is answered with an 8x8 frame, not the window size. */
    const std::vector<uint8_t> pixels = framePixels(kFrameSide, kFrameSide);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!clientDone.load() && std::chrono::steady_clock::now() < deadline) {
        if (SDL3MCP_FrameRequested()) {
            SDL3MCP_WriteFrame(pixels.data(), pixels.size(), kFrameSide, kFrameSide);
        }
        CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
        std::this_thread::sleep_for(1ms);
    }
    client.join();
    CHECK(clientDone.load());

    SDL3MCP_AppShutdown();

    CHECK(::dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO);
    CHECK(::dup2(savedStdout, STDOUT_FILENO) == STDOUT_FILENO);
    ::close(savedStdin);
    ::close(savedStdout);

    SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("frame_size_tests passed\n");
    return 0;
}
