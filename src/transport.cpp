// TODO(windows): this transport uses POSIX poll/read/write and relies on the
// server thread blocking SIGPIPE. Windows needs binary stdio (_setmode with
// _O_BINARY) and a PeekNamedPipe/WaitForSingleObject reader wake-up. See
// "Platforms" in README.md; CMake rejects a WIN32 configure until this lands.
#include "transport.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
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

std::string shortRead(size_t received, size_t expected)
{
    return "expected " + std::to_string(expected) + " bytes, got " +
           std::to_string(received);
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

ReadStatus Transport::readLine(std::string &line, std::string &error, int timeoutMs)
{
    error.clear();

    const bool bounded = timeoutMs >= 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

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

        int pollTimeoutMs = kPollTimeoutMs;
        if (bounded) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                return ReadStatus::Timeout;
            }
            pollTimeoutMs = static_cast<int>(std::min<long long>(kPollTimeoutMs, remaining));
        }

        pollfd descriptor{};
        descriptor.fd = readFd_;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, pollTimeoutMs);
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

bool Transport::readBytes(size_t count,
                          std::vector<uint8_t> &out,
                          std::string &error,
                          int timeoutMs)
{
    error.clear();
    out.clear();
    if (count == 0) {
        return true;
    }
    out.reserve(count);

    const bool bounded = timeoutMs >= 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    /*
      readLine may have buffered bytes past its newline: a peer that sent a line
      and the start of the payload in one write has them here already.
    */
    const size_t buffered = std::min(count, buffer_.size());
    out.insert(out.end(), buffer_.begin(),
               buffer_.begin() + static_cast<std::string::difference_type>(buffered));
    buffer_.erase(0, buffered);

    while (out.size() < count) {
        if (stopped_.load(std::memory_order_relaxed)) {
            error = "the transport was stopped";
            return false;
        }

        int pollTimeoutMs = kPollTimeoutMs;
        if (bounded) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                error = shortRead(out.size(), count) + " before the deadline";
                return false;
            }
            pollTimeoutMs = static_cast<int>(std::min<long long>(kPollTimeoutMs, remaining));
        }

        pollfd descriptor{};
        descriptor.fd = readFd_;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, pollTimeoutMs);
        if (ready < 0) {
            const int errorCode = errno;
            if (errorCode == EINTR) {
                continue;
            }
            error = describe("poll failed", errorCode);
            return false;
        }
        if (ready == 0) {
            /* A timeout only means "nothing yet"; the loop re-checks stop(). */
            continue;
        }

        /* Read at most what is still missing: the next line follows the payload. */
        uint8_t chunk[kReadChunkBytes];
        const size_t wanted = std::min(count - out.size(), kReadChunkBytes);
        const ssize_t received = ::read(readFd_, chunk, wanted);
        if (received < 0) {
            const int errorCode = errno;
            if (errorCode == EINTR || errorCode == EAGAIN || errorCode == EWOULDBLOCK) {
                continue;
            }
            error = describe("read failed", errorCode);
            return false;
        }
        if (received == 0) {
            error = shortRead(out.size(), count) + ": the connection closed";
            return false;
        }
        out.insert(out.end(), chunk, chunk + received);
    }
    return true;
}

bool Transport::writeLine(std::string_view line)
{
    std::string framed;
    framed.reserve(line.size() + 1);
    framed.append(line);
    framed.push_back('\n');
    return writeAll(framed.data(), framed.size());
}

bool Transport::writeBytes(const uint8_t *data, size_t size)
{
    if (size == 0) {
        return true;
    }
    if (data == nullptr) {
        errno = EINVAL;
        return false;
    }
    return writeAll(reinterpret_cast<const char *>(data), size);
}

bool Transport::writeAll(const char *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const ssize_t count = ::write(writeFd_, data + written, size - written);
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
