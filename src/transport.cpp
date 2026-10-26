// TODO(windows): this transport uses POSIX poll/read/write and relies on the
// server thread blocking SIGPIPE. Windows needs binary stdio (_setmode with
// _O_BINARY) and a PeekNamedPipe/WaitForSingleObject reader wake-up. See
// "Platforms" in README.md; CMake rejects a WIN32 configure until this lands.
#include "transport.hpp"

#include <cerrno>
#include <cstring>
#include <string_view>

#include <poll.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

/* Short enough that stop() is observed promptly, long enough to idle cheaply. */
constexpr int kPollTimeoutMs = 50;

constexpr size_t kReadChunkBytes = 4096;

std::string describe(std::string_view context, int errorCode)
{
    return std::string(context) + ": " + std::strerror(errorCode);
}

} // namespace

Transport::Transport(int readFd, int writeFd, size_t maxLineBytes)
    : readFd_(readFd), writeFd_(writeFd), maxLineBytes_(maxLineBytes)
{
}

std::string Transport::lineLimitError() const
{
    return "input line exceeds the " + std::to_string(maxLineBytes_) + " byte limit";
}

ReadStatus Transport::readLine(std::string &line, std::string &error)
{
    error.clear();

    for (;;) {
        const size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            if (newline > maxLineBytes_) {
                error = lineLimitError();
                buffer_.clear();
                return ReadStatus::Error;
            }
            line.assign(buffer_, 0, newline);
            buffer_.erase(0, newline + 1);
            /* One trailing '\r' is framing, not content: accept CRLF clients. */
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return ReadStatus::Line;
        }
        if (buffer_.size() > maxLineBytes_) {
            error = lineLimitError();
            buffer_.clear();
            return ReadStatus::Error;
        }
        if (stopped_.load(std::memory_order_relaxed)) {
            return ReadStatus::Stopped;
        }

        pollfd descriptor{};
        descriptor.fd = readFd_;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, kPollTimeoutMs);
        if (ready < 0) {
            const int errorCode = errno;
            if (errorCode == EINTR) {
                continue;
            }
            error = describe("poll failed", errorCode);
            return ReadStatus::Error;
        }
        /* A timeout only means "nothing yet"; the loop re-checks stop(). */
        if (ready == 0) {
            continue;
        }

        char chunk[kReadChunkBytes];
        const ssize_t count = ::read(readFd_, chunk, sizeof chunk);
        if (count < 0) {
            const int errorCode = errno;
            if (errorCode == EINTR || errorCode == EAGAIN || errorCode == EWOULDBLOCK) {
                continue;
            }
            error = describe("read failed", errorCode);
            return ReadStatus::Error;
        }
        if (count == 0) {
            if (buffer_.empty()) {
                return ReadStatus::Eof;
            }
            error = "the client closed the connection in the middle of a line";
            return ReadStatus::Error;
        }
        buffer_.append(chunk, static_cast<size_t>(count));
    }
}

bool Transport::writeLine(std::string_view line)
{
    std::string framed;
    framed.reserve(line.size() + 1);
    framed.append(line);
    framed.push_back('\n');

    size_t written = 0;
    while (written < framed.size()) {
        const ssize_t count =
            ::write(writeFd_, framed.data() + written, framed.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            /* A zero-byte write cannot make progress; report it as an I/O error. */
            errno = EIO;
            return false;
        }
        written += static_cast<size_t>(count);
    }
    return true;
}

void Transport::stop()
{
    stopped_.store(true, std::memory_order_relaxed);
}

bool Transport::stopped() const
{
    return stopped_.load(std::memory_order_relaxed);
}

} // namespace sdl3mcp
