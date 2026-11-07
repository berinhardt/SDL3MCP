#include "child_peer.hpp"

#include <utility>

namespace sdl3mcp
{

ChildPeer::ChildPeer() = default;

ChildPeer::~ChildPeer() = default;

bool ChildPeer::start(const ChildCommand &command, std::string &error)
{
    if (!process_.start(command, error)) {
        return false;
    }

    /* Both pipe ends exist as soon as the fork returns; the transport owns neither. */
    transport_ = std::make_unique<Transport>(process_.stdoutFd(), process_.stdinFd());
    return true;
}

bool ChildPeer::waitForExec(int timeoutMs, std::string &error)
{
    return process_.waitForExec(timeoutMs, error);
}

bool ChildPeer::hasExited() const
{
    return process_.hasExited();
}

bool ChildPeer::waitForExit(int timeoutMs)
{
    return process_.waitForExit(timeoutMs);
}

void ChildPeer::waitForExit()
{
    process_.waitForExit();
}

int ChildPeer::exitCode() const
{
    return process_.exitCode();
}

std::string ChildPeer::stderrTail() const
{
    return process_.stderrTail();
}

void ChildPeer::terminate(int graceMs)
{
    process_.terminate(graceMs);
}

ReadStatus ChildPeer::readLine(std::string &line, std::string &error, int timeoutMs)
{
    if (!transport_) {
        error = "the application is not running";
        return ReadStatus::Error;
    }
    return transport_->readLine(line, error, timeoutMs);
}

bool ChildPeer::readBytes(size_t count,
                          std::vector<uint8_t> &out,
                          std::string &error,
                          int timeoutMs)
{
    if (!transport_) {
        error = "the application is not running";
        out.clear();
        return false;
    }
    return transport_->readBytes(count, out, error, timeoutMs);
}

bool ChildPeer::writeLine(std::string_view line)
{
    return transport_ != nullptr && transport_->writeLine(line);
}

bool ChildPeer::writeBytes(const uint8_t *data, size_t size)
{
    return transport_ != nullptr && transport_->writeBytes(data, size);
}

void ChildPeer::stop()
{
    if (transport_) {
        transport_->stop();
    }
}

} // namespace sdl3mcp
