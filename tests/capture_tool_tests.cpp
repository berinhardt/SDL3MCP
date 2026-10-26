/*
  The capture path end to end through the public API: JSON-RPC on pipes, the
  session, the capture state, SDL3MCP_FrameRequested/WriteFrame on the app
  thread, the pure frame pipeline, and sdl_info reporting the resulting stats.

  The dummy video driver gives us a real software renderer with no display. The
  app loop clears a known color every iteration, answers a pending request by
  reading the renderer back and converting to tightly packed RGBA8888, and then
  presents. The client runs on its own thread because sdl_capture blocks until
  the app answers.

  The two successful captures prove the two interesting shapes: a region is
  cropped before any downscale, and no region returns the full drawable size.
  The PNG header is checked directly, so a wrong crop or a wrong surface
  conversion cannot hide behind a valid image block.
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

int base64Value(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

std::vector<uint8_t> base64Decode(std::string_view text)
{
    std::vector<uint8_t> decoded;
    uint32_t buffer = 0;
    int bits = 0;

    for (const char character : text) {
        if (character == '=') {
            break;
        }
        const int value = base64Value(character);
        CHECK(value >= 0);
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFFu));
            buffer &= (1u << bits) - 1u;
        }
    }
    return decoded;
}

uint32_t readBigEndian32(const std::vector<uint8_t> &data, std::size_t offset)
{
    CHECK(offset + 4 <= data.size());
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           static_cast<uint32_t>(data[offset + 3]);
}

bool isPngWithSize(const std::vector<uint8_t> &png, uint32_t width, uint32_t height)
{
    constexpr std::array<uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 33) {
        return false;
    }
    if (!std::equal(signature.begin(), signature.end(), png.begin())) {
        return false;
    }
    if (png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R') {
        return false;
    }
    return readBigEndian32(png, 16) == width && readBigEndian32(png, 20) == height;
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
void runClient(int requestsFd, int responsesFd)
{
    writeAll(requestsFd, R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
                         R"("params":{"protocolVersion":"2025-11-25"}})" "\n");
    const nlohmann::json initialized = nlohmann::json::parse(readLine(responsesFd));
    CHECK(initialized["id"] == 1);
    CHECK(initialized["result"]["protocolVersion"] == "2025-11-25");

    writeAll(requestsFd, R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");

    nlohmann::json region;
    region["x"] = 0;
    region["y"] = 0;
    region["width"] = 10;
    region["height"] = 10;
    nlohmann::json regionArguments;
    regionArguments["region"] = region;

    const nlohmann::json cropped =
        call(requestsFd, responsesFd, 2, "sdl_capture", regionArguments);
    CHECK(cropped["result"]["isError"] == false);
    const nlohmann::json &croppedContent = cropped["result"]["content"];
    CHECK(croppedContent.is_array());
    CHECK(croppedContent.size() == 2);
    CHECK(croppedContent[0]["type"] == "text");
    CHECK(croppedContent[1]["type"] == "image");
    CHECK(croppedContent[1]["mimeType"] == "image/png");

    const std::string croppedText = croppedContent[0]["text"].get<std::string>();
    CHECK(croppedText.find("10x10") != std::string::npos);
    CHECK(croppedText.find("from 10x10 at 0,0") != std::string::npos);
    CHECK(isPngWithSize(
        base64Decode(croppedContent[1]["data"].get<std::string>()), 10, 10));

    const nlohmann::json full =
        call(requestsFd, responsesFd, 3, "sdl_capture", nlohmann::json::object());
    CHECK(full["result"]["isError"] == false);
    const nlohmann::json &fullContent = full["result"]["content"];
    CHECK(fullContent.is_array());
    CHECK(fullContent.size() == 2);
    CHECK(fullContent[0]["type"] == "text");
    CHECK(fullContent[1]["type"] == "image");
    CHECK(fullContent[1]["mimeType"] == "image/png");

    const std::string fullText = fullContent[0]["text"].get<std::string>();
    CHECK(fullText.rfind("64x64 PNG", 0) == 0);
    CHECK(isPngWithSize(base64Decode(fullContent[1]["data"].get<std::string>()), 64, 64));

    const nlohmann::json info =
        parseInfo(call(requestsFd, responsesFd, 4, "sdl_info", nlohmann::json::object()));
    CHECK(info["capture"]["count"].get<uint64_t>() >= 2);
    CHECK(info["capture"]["lastWidth"] == 64);
    CHECK(info["capture"]["lastHeight"] == 64);
    CHECK(info["capture"]["lastBytes"].get<uint64_t>() > 0);
    CHECK(info["capture"]["pending"] == false);

    /* Invalid arguments fail before a request is started. */
    const nlohmann::json unknown =
        call(requestsFd, responsesFd, 5, "sdl_capture", nlohmann::json{{"bogus", 1}});
    CHECK(unknown["result"]["isError"] == true);

    nlohmann::json badRegion;
    badRegion["x"] = 0;
    badRegion["y"] = 0;
    badRegion["width"] = 10;
    const nlohmann::json missingHeight = call(requestsFd, responsesFd, 6, "sdl_capture",
                                              nlohmann::json{{"region", badRegion}});
    CHECK(missingHeight["result"]["isError"] == true);

    const nlohmann::json tooNarrow =
        call(requestsFd, responsesFd, 7, "sdl_capture", nlohmann::json{{"max_width", 15}});
    CHECK(tooNarrow["result"]["isError"] == true);

    const nlohmann::json afterInvalid =
        parseInfo(call(requestsFd, responsesFd, 8, "sdl_info", nlohmann::json::object()));
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
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
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

    std::atomic<bool> clientDone{false};
    std::thread client([&requests, &responses, &clientDone] {
        runClient(requests.writeFd, responses.readFd);
        clientDone.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!clientDone.load() && std::chrono::steady_clock::now() < deadline) {
        CHECK(SDL_SetRenderDrawColor(renderer, 10, 20, 30, 255));
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
