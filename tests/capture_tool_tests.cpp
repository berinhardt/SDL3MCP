/*
  The capture path end to end through the public API: JSON-RPC on pipes, the
  session, the capture state, SDL3MCP_FrameRequested/WriteFrame on the app
  thread, the library's crop, and sdl_info reporting the resulting stats.

  The dummy video driver gives us a real software renderer with no display. The
  app loop clears a known color every iteration, answers a pending request by
  reading the renderer back and converting to tightly packed RGBA8888, and then
  presents. The client runs on its own thread because sdl_capture blocks until
  the app answers.

  A capture is one JSON-RPC result line naming format/width/height/byteLength,
  immediately followed by exactly that many raw RGBA bytes. The checks here read
  the payload back for a known colour, so a wrong crop, a wrong surface
  conversion, or a stray newline cannot hide behind a valid-looking header.
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
#include <cstring>
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

void writeLine(int fd, const nlohmann::json &message)
{
    writeAll(fd, message.dump() + "\n");
}

/*
  Reads framed messages from the server: one line at a time, or exactly the
  announced number of raw frame bytes. Anything read past a newline stays in
  `pending` for the next call.
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

    void readExact(std::size_t count, std::vector<uint8_t> &out)
    {
        out.clear();
        out.reserve(count);

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
    }
};

/* One capture: the validated result line plus the bytes that followed it. */
struct CaptureResponse
{
    nlohmann::json header;
    std::vector<uint8_t> pixels;
};

CaptureResponse readCapture(MessageReader &reader, int id)
{
    const nlohmann::json message = nlohmann::json::parse(reader.readLine());
    CHECK(message["jsonrpc"] == "2.0");
    CHECK(message["id"] == id);
    CHECK(message.contains("result"));

    const nlohmann::json &result = message["result"];
    CHECK(result["format"] == "rgba8888");
    CHECK(result["width"].is_number_integer());
    CHECK(result["height"].is_number_integer());
    CHECK(result["byteLength"].is_number_integer());

    CaptureResponse capture;
    capture.header = result;
    const std::size_t byteLength = result["byteLength"].get<std::size_t>();
    CHECK(byteLength == static_cast<std::size_t>(result["width"].get<int>()) *
                            static_cast<std::size_t>(result["height"].get<int>()) * 4);
    reader.readExact(byteLength, capture.pixels);
    return capture;
}

void requestCapture(int requestsFd, int id, nlohmann::json arguments)
{
    writeLine(requestsFd, nlohmann::json{
                              {"jsonrpc", "2.0"},
                              {"id", id},
                              {"method", "tools/call"},
                              {"params", nlohmann::json{{"name", "sdl_capture"},
                                                        {"arguments", std::move(arguments)}}},
                          });
}

nlohmann::json call(int requestsFd,
                    MessageReader &reader,
                    int id,
                    const std::string &name,
                    nlohmann::json arguments)
{
    writeLine(requestsFd, nlohmann::json{
                              {"jsonrpc", "2.0"},
                              {"id", id},
                              {"method", "tools/call"},
                              {"params", nlohmann::json{{"name", name},
                                                        {"arguments", std::move(arguments)}}},
                          });
    return nlohmann::json::parse(reader.readLine());
}

/* The app clears with this colour every frame. */
constexpr std::array<uint8_t, 4> kClearColor{10, 20, 30, 255};

bool isClearColor(const std::vector<uint8_t> &pixels, std::size_t index)
{
    const std::size_t offset = index * 4;
    CHECK(offset + 4 <= pixels.size());
    return std::array<uint8_t, 4>{pixels[offset], pixels[offset + 1],
                                  pixels[offset + 2], pixels[offset + 3]} == kClearColor;
}

bool allPixelsAreClearColor(const std::vector<uint8_t> &pixels)
{
    for (std::size_t index = 0; index < pixels.size() / 4; ++index) {
        if (!isClearColor(pixels, index)) {
            return false;
        }
    }
    return true;
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

/* The client half: one ordered conversation, asserted response by response. */
void runClient(int requestsFd, MessageReader &reader)
{
    writeAll(requestsFd, R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
                         R"("params":{"protocolVersion":"2025-11-25"}})" "\n");
    const nlohmann::json initialized = nlohmann::json::parse(reader.readLine());
    CHECK(initialized["id"] == 1);
    CHECK(initialized["result"]["protocolVersion"] == "2025-11-25");

    writeAll(requestsFd, R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");

    /* A region crops in the library, so the framed dimensions are the region's. */
    nlohmann::json region;
    region["x"] = 0;
    region["y"] = 0;
    region["width"] = 10;
    region["height"] = 10;
    requestCapture(requestsFd, 2, nlohmann::json{{"region", region}});
    const CaptureResponse cropped = readCapture(reader, 2);
    CHECK(cropped.header["width"] == 10);
    CHECK(cropped.header["height"] == 10);
    CHECK(cropped.header["byteLength"] == 10 * 10 * 4);
    CHECK(cropped.pixels.size() == 10 * 10 * 4);
    CHECK(allPixelsAreClearColor(cropped.pixels));

    /* No region returns the full drawable size with exactly one payload. */
    requestCapture(requestsFd, 3, nlohmann::json::object());
    const CaptureResponse full = readCapture(reader, 3);
    CHECK(full.header["width"] == 64);
    CHECK(full.header["height"] == 64);
    CHECK(full.header["byteLength"] == 64 * 64 * 4);
    CHECK(full.pixels.size() == 64 * 64 * 4);
    CHECK(allPixelsAreClearColor(full.pixels));
    CHECK(reader.pending.empty());

    const nlohmann::json info =
        parseInfo(call(requestsFd, reader, 4, "sdl_info", nlohmann::json::object()));
    CHECK(info["capture"]["count"].get<uint64_t>() >= 2);
    CHECK(info["capture"]["lastWidth"] == 64);
    CHECK(info["capture"]["lastHeight"] == 64);
    /* lastBytes is the raw RGBA size of the last frame, not a PNG size. */
    CHECK(info["capture"]["lastBytes"].get<uint64_t>() == 64u * 64u * 4u);
    CHECK(info["capture"]["pending"] == false);

    /* Invalid arguments fail before a request is started. */
    const nlohmann::json unknown =
        call(requestsFd, reader, 5, "sdl_capture", nlohmann::json{{"bogus", 1}});
    CHECK(unknown["result"]["isError"] == true);

    nlohmann::json badRegion;
    badRegion["x"] = 0;
    badRegion["y"] = 0;
    badRegion["width"] = 10;
    const nlohmann::json missingHeight =
        call(requestsFd, reader, 6, "sdl_capture", nlohmann::json{{"region", badRegion}});
    CHECK(missingHeight["result"]["isError"] == true);

    /* max_width is supervisor policy now: a direct library client may not send it. */
    const nlohmann::json maxWidth =
        call(requestsFd, reader, 7, "sdl_capture", nlohmann::json{{"max_width", 320}});
    CHECK(maxWidth["result"]["isError"] == true);

    const nlohmann::json afterInvalid =
        parseInfo(call(requestsFd, reader, 8, "sdl_info", nlohmann::json::object()));
    CHECK(afterInvalid["capture"]["pending"] == false);
}

/* App thread: answer one pending request with tightly packed RGBA8888. */
void writeFrameIfRequested(SDL_Renderer *renderer, SDL_Window *window)
{
    if (!SDL3MCP_FrameRequested()) {
        return;
    }

    SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
    CHECK(surface != nullptr);
    /* RGBA32 is the byte-order alias for bytes R,G,B,A, which is what WriteFrame wants. */
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    CHECK(rgba != nullptr);

    int width = 0;
    int height = 0;
    CHECK(SDL_GetWindowSizeInPixels(window, &width, &height));
    CHECK(rgba->w == width);
    CHECK(rgba->h == height);

    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    const auto *source = static_cast<const uint8_t *>(rgba->pixels);
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    for (int row = 0; row < height; ++row) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * rowBytes,
                    source + static_cast<std::size_t>(row) * rgba->pitch, rowBytes);
    }
    SDL_DestroySurface(rgba);

    SDL3MCP_WriteFrame(pixels.data(), pixels.size());
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

    SDL_Window *window = SDL_CreateWindow("SDL3MCP capture", 64, 64, 0);
    CHECK(window != nullptr);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    CHECK(renderer != nullptr);

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

    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!clientDone.load() && std::chrono::steady_clock::now() < deadline) {
        CHECK(SDL_SetRenderDrawColor(renderer, kClearColor[0], kClearColor[1],
                                     kClearColor[2], kClearColor[3]));
        CHECK(SDL_RenderClear(renderer));
        writeFrameIfRequested(renderer, window);
        CHECK(SDL_RenderPresent(renderer));
        CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
        std::this_thread::sleep_for(1ms);
    }
    client.join();
    CHECK(clientDone.load());

    /* One more turn publishes the final capture statistics. */
    CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);

    SDL3MCP_AppShutdown();

    CHECK(::dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO);
    CHECK(::dup2(savedStdout, STDOUT_FILENO) == STDOUT_FILENO);
    ::close(savedStdin);
    ::close(savedStdout);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("capture_tool_tests passed\n");
    return 0;
}
