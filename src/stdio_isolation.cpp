// TODO(windows): fd isolation uses POSIX dup/dup2 and /dev/null. Windows needs
// _dup/_dup2 with the NUL device and binary stdio (_setmode with _O_BINARY).
// See "Platforms" in README.md; CMake rejects a WIN32 configure until then.
#include "stdio_isolation.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

std::string describe(std::string_view context, int errorCode)
{
    return std::string(context) + ": " + std::strerror(errorCode);
}

int duplicate(int fd, std::string &error)
{
    const int copy = ::dup(fd);
    if (copy < 0) {
        error = describe("cannot duplicate file descriptor " + std::to_string(fd), errno);
    }
    return copy;
}

} // namespace

StdioIsolation::~StdioIsolation()
{
    restore();
}

bool StdioIsolation::engage(std::string &error)
{
    error.clear();
    if (engaged_) {
        error = "stdin/stdout isolation is already engaged";
        return false;
    }

    mcpReadFd_ = duplicate(STDIN_FILENO, error);
    if (mcpReadFd_ < 0) {
        closeAll();
        return false;
    }

    savedStdin_ = duplicate(STDIN_FILENO, error);
    if (savedStdin_ < 0) {
        closeAll();
        return false;
    }

    mcpWriteFd_ = duplicate(STDOUT_FILENO, error);
    if (mcpWriteFd_ < 0) {
        closeAll();
        return false;
    }

    savedStdout_ = duplicate(STDOUT_FILENO, error);
    if (savedStdout_ < 0) {
        closeAll();
        return false;
    }

    const int devNull = ::open("/dev/null", O_RDONLY);
    if (devNull < 0) {
        error = describe("cannot open /dev/null", errno);
        closeAll();
        return false;
    }

    const bool stdinRedirected = ::dup2(devNull, STDIN_FILENO) >= 0;
    const int stdinError = errno;
    (void)::close(devNull);
    if (!stdinRedirected) {
        error = describe("cannot redirect stdin to /dev/null", stdinError);
        closeAll();
        return false;
    }

    if (::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        error = describe("cannot redirect stdout to stderr", errno);
        /* Undo the redirect that already happened before dropping the copies. */
        (void)::dup2(savedStdin_, STDIN_FILENO);
        closeAll();
        return false;
    }

    engaged_ = true;
    return true;
}

void StdioIsolation::restore()
{
    if (savedStdin_ >= 0) {
        (void)::dup2(savedStdin_, STDIN_FILENO);
    }
    if (savedStdout_ >= 0) {
        (void)::dup2(savedStdout_, STDOUT_FILENO);
    }
    closeAll();
}

void StdioIsolation::closeAll() noexcept
{
    if (mcpReadFd_ >= 0) {
        (void)::close(mcpReadFd_);
        mcpReadFd_ = -1;
    }
    if (mcpWriteFd_ >= 0) {
        (void)::close(mcpWriteFd_);
        mcpWriteFd_ = -1;
    }
    if (savedStdin_ >= 0) {
        (void)::close(savedStdin_);
        savedStdin_ = -1;
    }
    if (savedStdout_ >= 0) {
        (void)::close(savedStdout_);
        savedStdout_ = -1;
    }
    engaged_ = false;
}

} // namespace sdl3mcp
