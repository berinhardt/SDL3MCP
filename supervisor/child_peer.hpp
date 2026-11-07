/*
  The child application as the supervisor's downstream peer.

  It owns today's ChildProcess (fork/exec, private pipes, waitpid, exit status,
  stderr tail) together with the Transport over the child's two pipe ends, so
  the proxy loop sees only the Peer interface. Lifecycle stays on this concrete
  interface for now: start, waitForExec, waitForExit, hasExited, exitCode,
  stderrTail, and terminate. The data path is the current behaviour moved
  behind the interface, unchanged.

  The transport does not own the fds: ChildProcess created them and closes them
  when it is destroyed, which happens after the transport member because the
  transport is declared second.
*/

#ifndef SDL3MCP_SUPERVISOR_CHILD_PEER_HPP
#define SDL3MCP_SUPERVISOR_CHILD_PEER_HPP

#include "child_process.hpp"
#include "peer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sdl3mcp
{

class ChildPeer : public Peer
{
public:
    ChildPeer();
    ~ChildPeer() override;

    ChildPeer(const ChildPeer &) = delete;
    ChildPeer &operator=(const ChildPeer &) = delete;

    /*
      Forks and execs the command and points the transport at the child's pipe
      ends. False with `error` only when the process could not be created at
      all; an exec failure still surfaces through waitForExec().
    */
    bool start(const ChildCommand &command, std::string &error);

    bool waitForExec(int timeoutMs, std::string &error);

    bool hasExited() const;
    bool waitForExit(int timeoutMs);
    void waitForExit();
    int exitCode() const;
    std::string stderrTail() const;
    void terminate(int graceMs = ChildProcess::kDefaultTerminateGraceMs);

    ReadStatus readLine(std::string &line, std::string &error,
                        int timeoutMs = -1) override;
    bool readBytes(size_t count,
                   std::vector<uint8_t> &out,
                   std::string &error,
                   int timeoutMs = -1) override;
    bool writeLine(std::string_view line) override;
    bool writeBytes(const uint8_t *data, size_t size) override;
    void stop() override;
    bool closed() const override;

private:
    ChildProcess process_;
    std::unique_ptr<Transport> transport_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_CHILD_PEER_HPP */
