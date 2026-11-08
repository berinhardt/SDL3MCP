/*
  The input path end to end through the public API: JSON-RPC on pipes, the
  session, the input queue, SDL3MCP_AppIterate on the app thread, SDL's event
  queue, the app's poll, sdl_info answering from the published snapshot, and the
  frame sdl_input returns after its batch.

  The video driver is the dummy one, so the test claims no display and sees no
  real input. The client runs on its own thread because sdl_input blocks until
  the app applies the batch, confirms it at F+1, and then answers the capture
  request; the main thread is the app, iterating and polling exactly as a game
  loop would, and writing a known frame whenever one is requested.

  An input result is now two things at once: a result line carrying the text
  naming both the applied and attached frames and the raw framing header,
  followed by exactly byteLength RGBA bytes. The reader therefore consumes the
  payload before the next line, which is what proves a frame-returning call never
  desynchronises the stream.

  Text is the interesting lifetime case: SDL's text event borrows a string owned
  by the batch, which AppIterate frees one frame later. The recorder copies the
  string at poll time, which is the contract every app has to follow too.
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

constexpr int kFrameWidth = 32;
constexpr int kFrameHeight = 24;
/* Every frame this app writes uses one colour, so a wrong crop or a stray line
   cannot hide behind a valid-looking header. */
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

void writeLine(int fd, const nlohmann::json &message)
{
    writeAll(fd, message.dump() + "\n");
}

/*
  Reads framed messages from the server: one line at a time, or exactly the
  announced number of raw frame bytes. Bytes read past a newline stay buffered
  for the next call, so a payload and the line after it never get mixed up.
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

void sendCall(int requestsFd, int id, const std::string &name, nlohmann::json arguments)
{
    writeLine(requestsFd, nlohmann::json{
                              {"jsonrpc", "2.0"},
                              {"id", id},
                              {"method", "tools/call"},
                              {"params", nlohmann::json{{"name", name},
                                                        {"arguments", std::move(arguments)}}},
                          });
}

/* One input result: the header's text plus the pixels that followed it. */
struct FramedResult
{
    nlohmann::json header;
    std::vector<uint8_t> pixels;
};

FramedResult readFramed(MessageReader &reader, int id)
{
    const nlohmann::json message = nlohmann::json::parse(reader.readLine());
    CHECK(message["jsonrpc"] == "2.0");
    CHECK(message["id"] == id);
    CHECK(message.contains("result"));

    const nlohmann::json &result = message["result"];
    CHECK(result["format"] == "rgba8888");
    CHECK(result["width"].is_number_integer());
    CHECK(result["height"].is_number_integer());
    CHECK(result["text"].is_string());

    FramedResult framed;
    framed.header = result;
    const std::size_t byteLength = result["byteLength"].get<std::size_t>();
    CHECK(byteLength == static_cast<std::size_t>(result["width"].get<int>()) *
                            static_cast<std::size_t>(result["height"].get<int>()) * 4);
    reader.readExact(byteLength, framed.pixels);
    return framed;
}

FramedResult inputFramed(int requestsFd, MessageReader &reader, int id,
                         nlohmann::json arguments)
{
    sendCall(requestsFd, id, "sdl_input", std::move(arguments));
    return readFramed(reader, id);
}

nlohmann::json textOf(const nlohmann::json &response)
{
    CHECK(response.contains("result"));
    const nlohmann::json &result = response["result"];
    CHECK(result["content"].is_array());
    CHECK(result["content"].size() == 1);
    CHECK(result["content"][0]["type"] == "text");
    return result["content"][0]["text"];
}

/* The two frame indices an sdl_input answer names, parsed from its text. */
struct FrameNames
{
    std::uint64_t applied = 0;
    std::uint64_t attached = 0;
};

std::uint64_t numberAfter(const std::string &text, std::string_view marker)
{
    const std::size_t position = text.find(marker);
    CHECK(position != std::string::npos);
    const std::size_t begin = position + marker.size();
    std::size_t end = begin;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
        ++end;
    }
    CHECK(end > begin);
    return std::stoull(text.substr(begin, end - begin));
}

FrameNames frameNamesOf(const std::string &text)
{
    return FrameNames{numberAfter(text, "applied at frame "),
                      numberAfter(text, "; frame ")};
}

nlohmann::json infoOf(int requestsFd, MessageReader &reader, int id)
{
    sendCall(requestsFd, id, "sdl_info", nlohmann::json::object());
    const nlohmann::json response = nlohmann::json::parse(reader.readLine());
    CHECK(response["id"] == id);
    CHECK(response["result"]["isError"] == false);
    return nlohmann::json::parse(textOf(response).get<std::string>());
}

bool isFrameColor(const std::vector<uint8_t> &pixels, std::size_t index)
{
    const std::size_t offset = index * 4;
    CHECK(offset + 4 <= pixels.size());
    return std::array<uint8_t, 4>{pixels[offset], pixels[offset + 1],
                                  pixels[offset + 2], pixels[offset + 3]} == kFrameColor;
}

bool allPixelsAreFrameColor(const std::vector<uint8_t> &pixels)
{
    for (std::size_t index = 0; index < pixels.size() / 4; ++index) {
        if (!isFrameColor(pixels, index)) {
            return false;
        }
    }
    return true;
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
void runClient(int requestsFd, MessageReader &reader, std::atomic<bool> &answerFrames)
{
    writeAll(requestsFd, R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
                         R"("params":{"protocolVersion":"2025-11-25"}})" "\n");
    const nlohmann::json initialized = nlohmann::json::parse(reader.readLine());
    CHECK(initialized["id"] == 1);
    CHECK(initialized["result"]["protocolVersion"] == "2025-11-25");

    writeAll(requestsFd, R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");

    /* A press returns the text naming its frame plus exactly that frame. */
    const FramedResult pressed =
        inputFramed(requestsFd, reader, 2, actionsOf({{{"type", "key"}, {"key", "a"}}}));
    const std::string pressedText = pressed.header["text"].get<std::string>();
    CHECK(pressedText.find("1 actions applied at frame ") != std::string::npos);
    const FrameNames pressedFrames = frameNamesOf(pressedText);
    CHECK(pressedFrames.applied > 0);
    /*
      The app answers the capture on the first iteration after the F+1
      acknowledgement, so the stamp is exactly two frames past the applied one.
      The text must state the relation without promising this fixed delta.
    */
    CHECK(pressedFrames.attached == pressedFrames.applied + 2);
    CHECK(pressedText.find("rendered after the actions were delivered") !=
          std::string::npos);
    CHECK(pressed.header["frame"].get<std::uint64_t>() == pressedFrames.attached);
    CHECK(pressed.header["width"] == kFrameWidth);
    CHECK(pressed.header["height"] == kFrameHeight);
    CHECK(pressed.pixels.size() ==
          static_cast<std::size_t>(kFrameWidth) * kFrameHeight * 4);
    CHECK(allPixelsAreFrameColor(pressed.pixels));

    const FramedResult held =
        inputFramed(requestsFd, reader, 3,
                    actionsOf({{{"type", "key"}, {"key", "right"}, {"action", "down"}}}));
    const std::string heldText = held.header["text"].get<std::string>();
    CHECK(heldText.find("actions applied at frame ") != std::string::npos);
    /* A later service is stamped higher, so the indices really move. */
    CHECK(frameNamesOf(heldText).attached > pressedFrames.attached);

    const nlohmann::json heldInfo = infoOf(requestsFd, reader, 4);
    CHECK(heldInfo["input"]["heldKeys"].is_array());
    CHECK(containsKey(heldInfo["input"]["heldKeys"], "right"));

    const FramedResult released =
        inputFramed(requestsFd, reader, 5, actionsOf({{{"type", "release_all"}}}));
    CHECK(released.header["text"].get<std::string>().find("actions applied at frame ") !=
          std::string::npos);

    const nlohmann::json releasedInfo = infoOf(requestsFd, reader, 6);
    CHECK(releasedInfo["input"]["heldKeys"].empty());
    CHECK(releasedInfo["input"]["heldButtons"].empty());
    CHECK(releasedInfo["appliedFrame"].get<uint64_t>() > 0);

    const FramedResult typed =
        inputFramed(requestsFd, reader, 7, actionsOf({{{"type", "text"}, {"text", "hi"}}}));
    CHECK(typed.header["text"].get<std::string>().find("actions applied at frame ") !=
          std::string::npos);

    const FramedResult moved = inputFramed(
        requestsFd, reader, 8, actionsOf({{{"type", "mouse_move"}, {"x", 10}, {"y", 20}}}));
    CHECK(moved.header["text"].get<std::string>().find("actions applied at frame ") !=
          std::string::npos);

    /* A region crops the returned frame exactly like sdl_capture. */
    nlohmann::json region;
    region["x"] = 0;
    region["y"] = 0;
    region["width"] = 8;
    region["height"] = 6;
    nlohmann::json croppedArguments = actionsOf({{{"type", "release_all"}}});
    croppedArguments["region"] = region;
    const FramedResult cropped = inputFramed(requestsFd, reader, 9, croppedArguments);
    CHECK(cropped.header["width"] == 8);
    CHECK(cropped.header["height"] == 6);
    CHECK(cropped.header["byteLength"] == 8 * 6 * 4);
    CHECK(cropped.pixels.size() == 8 * 6 * 4);
    CHECK(allPixelsAreFrameColor(cropped.pixels));
    /* The crop keeps the stamp, so the text and header still name one frame. */
    const FrameNames croppedFrames =
        frameNamesOf(cropped.header["text"].get<std::string>());
    CHECK(cropped.header["frame"].get<std::uint64_t>() == croppedFrames.attached);
    CHECK(croppedFrames.attached > croppedFrames.applied);

    /* A rejected batch is an error and no image at all. */
    sendCall(requestsFd, 10, "sdl_input", nlohmann::json{{"actions", nlohmann::json::array()}});
    const nlohmann::json rejected = nlohmann::json::parse(reader.readLine());
    CHECK(rejected["id"] == 10);
    CHECK(rejected["result"]["isError"] == true);
    CHECK(rejected["result"]["content"].size() == 1);
    CHECK(textOf(rejected).get<std::string>().find("at least one action") !=
          std::string::npos);

    /*
      A frame the app never writes still reports the applied batch: the input
      did reach the app, so the result is a missing observation, not a failure.
      The short timeout keeps the check quick.
    */
    answerFrames.store(false);
    nlohmann::json timeoutArguments = actionsOf({{{"type", "release_all"}}});
    timeoutArguments["timeout_ms"] = 250;
    sendCall(requestsFd, 11, "sdl_input", timeoutArguments);
    const nlohmann::json timedOut = nlohmann::json::parse(reader.readLine());
    answerFrames.store(true);
    CHECK(timedOut["id"] == 11);
    CHECK(timedOut["result"]["isError"] == true);
    const std::string timeoutText = textOf(timedOut).get<std::string>();
    CHECK(timeoutText.find("actions applied at frame ") != std::string::npos);
    CHECK(timeoutText.find("no frame was taken") != std::string::npos);
    CHECK(timeoutText.find("no frame arrived within") != std::string::npos);

    /* The stream is still in step after two calls that carried no frame. */
    const nlohmann::json after = infoOf(requestsFd, reader, 12);
    CHECK(after["frame"].get<uint64_t>() > 0);
}

/* App thread: answer a pending request with one known tightly packed frame. */
void writeFrameIfRequested(std::atomic<bool> &answerFrames)
{
    if (!answerFrames.load() || !SDL3MCP_FrameRequested()) {
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

    MessageReader reader;
    reader.fd = responses.readFd;

    std::atomic<bool> answerFrames{true};
    std::atomic<bool> clientDone{false};
    std::thread client([&requests, &reader, &answerFrames, &clientDone] {
        runClient(requests.writeFd, reader, answerFrames);
        clientDone.store(true);
    });

    Recorder recorder;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!clientDone.load() && std::chrono::steady_clock::now() < deadline) {
        writeFrameIfRequested(answerFrames);
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
