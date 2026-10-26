/*
  Process-level stdin/stdout isolation for the stdio transport.

  MCP speaks JSON-RPC on the descriptors the process was launched with, so the
  host must stop owning them the moment the server starts. engage() duplicates
  stdin and stdout twice: one pair is handed to the server thread, the other is
  kept so restore() can put the originals back. The process itself is then moved
  out of the way: stdin becomes /dev/null and stdout becomes stderr.

  That split is the whole safety argument. A stray printf still goes somewhere
  useful (stderr), a console read gets EOF instead of stealing a JSON-RPC
  request, and neither can corrupt the client's stream. The library never
  flushes or purges the host's stdio buffers: the contract is that the host
  writes nothing to stdout before SDL3MCP_AppInit.

  restore(), and the destructor that calls it, are idempotent. engage() undoes
  every partial change when it fails, so a failed startup leaves the process
  exactly as it found it.
*/

#ifndef SDL3MCP_STDIO_ISOLATION_HPP
#define SDL3MCP_STDIO_ISOLATION_HPP

#include <string>

namespace sdl3mcp
{

class StdioIsolation
{
public:
    StdioIsolation() noexcept = default;
    ~StdioIsolation();

    StdioIsolation(const StdioIsolation &) = delete;
    StdioIsolation &operator=(const StdioIsolation &) = delete;

    /*
      Duplicates the original stdin/stdout for the server and for restore, then
      redirects the process stdin to /dev/null and stdout to stderr. False with
      a message on failure, with every descriptor and redirection undone.
    */
    bool engage(std::string &error);

    /* Puts the saved stdin/stdout back, closes every descriptor, and goes inert. */
    void restore();

    /* The server's own stdin duplicate; -1 unless engaged. */
    int mcpReadFd() const noexcept { return mcpReadFd_; }

    /* The server's own stdout duplicate; -1 unless engaged. */
    int mcpWriteFd() const noexcept { return mcpWriteFd_; }

    bool engaged() const noexcept { return engaged_; }

private:
    void closeAll() noexcept;

    int savedStdin_ = -1;
    int savedStdout_ = -1;
    int mcpReadFd_ = -1;
    int mcpWriteFd_ = -1;
    bool engaged_ = false;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_STDIO_ISOLATION_HPP */
