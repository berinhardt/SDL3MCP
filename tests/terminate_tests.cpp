/*
  The terminate path end to end through the public API: JSON-RPC on pipes, the
  session, the sdl_terminate handler storing a flag, SDL3MCP_AppIterate on the
  app thread turning that flag into one SDL_EVENT_QUIT, and the app's own poll.

  The video driver is the dummy one. sdl_terminate does not block, so this test
  has no client thread: it writes a request and reads its response on the main
  thread, then iterates exactly as a game loop would.
*/

#include <SDL3MCP/SDL3MCP.h>

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

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

void writeLine(int fd, std::string_view line)
{
    writeAll(fd, std::string(line) + "\n");
}

nlohmann::json request(int requestsFd, int responsesFd, std::string_view line)
{
    writeLine(requestsFd, line);
    return nlohmann::json::parse(readLine(responsesFd));
}

void initialize(int requestsFd, int responsesFd)
{
    const nlohmann::json response = request(
        requestsFd, responsesFd,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    CHECK(response["id"] == 1);
    CHECK(response["result"]["protocolVersion"] == "2025-11-25");
    writeLine(requestsFd, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
}

bool listsTerminate(int requestsFd, int responsesFd)
{
    const nlohmann::json response =
        request(requestsFd, responsesFd, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    CHECK(response["id"] == 2);
    for (const nlohmann::json &tool : response["result"]["tools"]) {
        if (tool["name"] == "sdl_terminate") {
            return true;
        }
    }
    return false;
}

/* Drains SDL's queue and counts the quit events it contained. */
int quitEvents()
{
    int count = 0;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            ++count;
        }
    }
    return count;
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

    CHECK(SDL3MCP_AppInit(&configuration, 0, nullptr) == SDL_APP_CONTINUE);

    initialize(requests.writeFd, responses.readFd);
    CHECK(listsTerminate(requests.writeFd, responses.readFd));

    const nlohmann::json terminated = request(
        requests.writeFd, responses.readFd,
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"sdl_terminate","arguments":{}}})");
    CHECK(terminated["id"] == 3);
    CHECK(terminated["result"]["isError"] == false);
    const nlohmann::json &content = terminated["result"]["content"];
    CHECK(content.is_array());
    CHECK(content.size() == 1);
    CHECK(content[0]["type"] == "text");
    CHECK(content[0]["text"] == "termination requested");

    /* The request is turned into exactly one quit event on the app thread. */
    CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
    CHECK(quitEvents() == 1);

    /* The flag was exchanged, so a second frame must not repeat the push. */
    CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
    CHECK(quitEvents() == 0);

    SDL3MCP_AppShutdown();

    CHECK(::dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO);
    CHECK(::dup2(savedStdout, STDOUT_FILENO) == STDOUT_FILENO);
    ::close(savedStdin);
    ::close(savedStdout);

    SDL_Quit();

    std::printf("terminate_tests passed\n");
    return 0;
}
