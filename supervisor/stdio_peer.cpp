#include "stdio_peer.hpp"

namespace sdl3mcp
{

StdioPeer::StdioPeer(int readFd, int writeFd) : transport_(readFd, writeFd)
{
}

ReadStatus StdioPeer::readLine(std::string &line, std::string &error, int timeoutMs)
{
    return transport_.readLine(line, error, timeoutMs);
}

bool StdioPeer::readBytes(size_t count,
                          std::vector<uint8_t> &out,
                          std::string &error,
                          int timeoutMs)
{
    return transport_.readBytes(count, out, error, timeoutMs);
}

bool StdioPeer::writeLine(std::string_view line)
{
    return transport_.writeLine(line);
}

bool StdioPeer::writeBytes(const uint8_t *data, size_t size)
{
    return transport_.writeBytes(data, size);
}

void StdioPeer::stop()
{
    transport_.stop();
}

bool StdioPeer::closed() const
{
    /* An EOF on stdin is reported by readLine(); there is nothing to probe. */
    return false;
}

} // namespace sdl3mcp
