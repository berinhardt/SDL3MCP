/*
  The public lifecycle, one scenario per process.

  The interesting half is the stdio transport: it replaces the process's own
  stdin/stdout, so it can only be exercised against real descriptors. The
  stdio_pipes scenario installs its own pipes, starts MCP with STDIO, and proves
  that host stdout writes never reach the JSON-RPC stream, host stdin reads get
  EOF, and shutdown puts the original descriptors back.

  Each scenario exits the way it was asked, so CMake registers every one as a
  separate CTest and no scenario can leak global state into another.
*/

#include <SDL3MCP/SDL3MCP.h>

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

struct FdIdentity
{
    dev_t device = 0;
    ino_t inode = 0;
};

FdIdentity identityOf(int fd)
{
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    return FdIdentity{status.st_dev, status.st_ino};
}

bool sameFile(const FdIdentity &left, const FdIdentity &right)
{
    return left.device == right.device && left.inode == right.inode;
}

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
std::string readLine(int fd, std::chrono::milliseconds timeout = 5000ms)
{
    std::string pending;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        const std::size_t newline = pending.find('\n');
        if (newline != std::string::npos) {
            return pending.substr(0, newline);
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
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

bool hasSdlError()
{
    const char *error = SDL_GetError();
    return error != nullptr && error[0] != '\0';
}

/* Defaults plus the given arguments, without touching argv values. */
SDL_AppResult initWithArguments(SDL3MCP_Configuration *configuration,
                                std::string_view argument)
{
    char program[] = "lifecycle_tests";
    std::string storage(argument);
    char *argv[] = {program, storage.data(), nullptr};
    return SDL3MCP_AppInit(configuration, 2, argv);
}

void testNone()
{
    const FdIdentity beforeStdin = identityOf(STDIN_FILENO);
    const FdIdentity beforeStdout = identityOf(STDOUT_FILENO);

    CHECK(SDL3MCP_AppInit(nullptr, 0, nullptr) == SDL_APP_CONTINUE);

    /* NONE must not duplicate, redirect, read, or write any descriptor. */
    CHECK(sameFile(identityOf(STDIN_FILENO), beforeStdin));
    CHECK(sameFile(identityOf(STDOUT_FILENO), beforeStdout));

    CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
    CHECK(!SDL3MCP_FrameRequested());
    SDL3MCP_WriteFrame(nullptr, 0);
    CHECK(SDL3MCP_SetWindow(nullptr) == SDL_APP_CONTINUE);

    SDL_ClearError();
    CHECK(SDL3MCP_AppInit(nullptr, 0, nullptr) == SDL_APP_FAILURE);
    CHECK(hasSdlError());

    SDL3MCP_AppShutdown();
    SDL3MCP_AppShutdown();
}

void testHelp()
{
    const FdIdentity beforeStdin = identityOf(STDIN_FILENO);
    const FdIdentity beforeStdout = identityOf(STDOUT_FILENO);

    CHECK(initWithArguments(nullptr, "--mcp-help") == SDL_APP_SUCCESS);

    CHECK(sameFile(identityOf(STDIN_FILENO), beforeStdin));
    CHECK(sameFile(identityOf(STDOUT_FILENO), beforeStdout));

    SDL3MCP_AppShutdown();
}

void testVersion()
{
    const FdIdentity beforeStdin = identityOf(STDIN_FILENO);
    const FdIdentity beforeStdout = identityOf(STDOUT_FILENO);

    CHECK(initWithArguments(nullptr, "--mcp-version") == SDL_APP_SUCCESS);

    CHECK(sameFile(identityOf(STDIN_FILENO), beforeStdin));
    CHECK(sameFile(identityOf(STDOUT_FILENO), beforeStdout));

    SDL3MCP_AppShutdown();
}

void testError()
{
    const FdIdentity beforeStdin = identityOf(STDIN_FILENO);
    const FdIdentity beforeStdout = identityOf(STDOUT_FILENO);

    SDL_ClearError();
    CHECK(initWithArguments(nullptr, "--mcp-bogus") == SDL_APP_FAILURE);
    CHECK(hasSdlError());

    /* A rejected command line is decided before any descriptor changes. */
    CHECK(sameFile(identityOf(STDIN_FILENO), beforeStdin));
    CHECK(sameFile(identityOf(STDOUT_FILENO), beforeStdout));

    SDL3MCP_AppShutdown();
}

void testUninitialized()
{
    SDL_ClearError();
    CHECK(SDL3MCP_SetWindow(nullptr) == SDL_APP_FAILURE);
    CHECK(hasSdlError());

    CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
    CHECK(!SDL3MCP_FrameRequested());
    SDL3MCP_WriteFrame(nullptr, 0);

    SDL3MCP_AppShutdown();
}

void testStdioPipes()
{
    const int savedStdin = ::dup(STDIN_FILENO);
    const int savedStdout = ::dup(STDOUT_FILENO);
    CHECK(savedStdin >= 0);
    CHECK(savedStdout >= 0);

    Pipe input;
    Pipe output;
    CHECK(::dup2(input.readFd, STDIN_FILENO) == STDIN_FILENO);
    CHECK(::dup2(output.writeFd, STDOUT_FILENO) == STDOUT_FILENO);

    SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();
    configuration.transport = SDL3MCP_TRANSPORT_STDIO;

    CHECK(SDL3MCP_AppInit(&configuration, 0, nullptr) == SDL_APP_CONTINUE);

    /* Host stdin is /dev/null, so even a blocking read returns EOF at once. */
    struct stat devNullStatus {};
    CHECK(::stat("/dev/null", &devNullStatus) == 0);
    CHECK(sameFile(identityOf(STDIN_FILENO),
                   FdIdentity{devNullStatus.st_dev, devNullStatus.st_ino}));

    char byte = 'x';
    CHECK(::read(STDIN_FILENO, &byte, 1) == 0);

    /* Host stdout is stderr: fd 1 and fd 2 now name the same open file. */
    CHECK(sameFile(identityOf(STDOUT_FILENO), identityOf(STDERR_FILENO)));

    constexpr std::string_view noise = "noise\n";
    writeAll(STDOUT_FILENO, noise);

    constexpr std::string_view request =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
        R"("params":{"protocolVersion":"2025-11-25"}})";
    writeAll(input.writeFd, std::string(request) + "\n");

    /* The next response is pure JSON-RPC, never the host's noise line. */
    const std::string line = readLine(output.readFd);
    CHECK(line != noise.substr(0, noise.size() - 1));
    const nlohmann::json response = nlohmann::json::parse(line);
    CHECK(response["jsonrpc"] == "2.0");
    CHECK(response["id"] == 1);
    CHECK(response["result"]["protocolVersion"] == "2025-11-25");

    SDL3MCP_AppShutdown();

    /* Shutdown restored the descriptors AppInit found, not /dev/null and stderr. */
    struct stat currentDevNull {};
    CHECK(::stat("/dev/null", &currentDevNull) == 0);
    CHECK(!sameFile(identityOf(STDIN_FILENO),
                    FdIdentity{currentDevNull.st_dev, currentDevNull.st_ino}));
    CHECK(!sameFile(identityOf(STDOUT_FILENO), identityOf(STDERR_FILENO)));

    /* Put the process's true originals back before this test process exits. */
    struct stat originalStdin {};
    struct stat originalStdout {};
    CHECK(::fstat(savedStdin, &originalStdin) == 0);
    CHECK(::fstat(savedStdout, &originalStdout) == 0);
    CHECK(::dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO);
    CHECK(::dup2(savedStdout, STDOUT_FILENO) == STDOUT_FILENO);
    CHECK(sameFile(identityOf(STDIN_FILENO),
                   FdIdentity{originalStdin.st_dev, originalStdin.st_ino}));
    CHECK(sameFile(identityOf(STDOUT_FILENO),
                   FdIdentity{originalStdout.st_dev, originalStdout.st_ino}));

    ::close(savedStdin);
    ::close(savedStdout);
}

} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const std::string_view scenario(argv[1]);

    if (scenario == "none") {
        testNone();
    } else if (scenario == "help") {
        testHelp();
    } else if (scenario == "version") {
        testVersion();
    } else if (scenario == "error") {
        testError();
    } else if (scenario == "uninitialized") {
        testUninitialized();
    } else if (scenario == "stdio_pipes") {
        testStdioPipes();
    } else {
        std::fprintf(stderr, "unknown lifecycle scenario '%s'\n", argv[1]);
        return 1;
    }

    std::fprintf(stderr, "lifecycle %s passed\n", argv[1]);
    return 0;
}
