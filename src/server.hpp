/*
  The server thread: the bridge between the validated JSON layer and the app
  thread.

  It owns a Transport and a Session and nothing else. Each loop iteration reads
  one line, parses it, lets the session decide, and either writes the response or
  hands a ToolCall to the injected handler. That means the server can be run
  against pipes in a test exactly as it runs against stdin/stdout in the app, and
  no tool execution lives here.

  The session is passed by reference because it is per connection and its
  lifetime belongs to whoever created both; the handler is a value so the app can
  capture its queues. Neither may touch SDL from this thread: the handler is
  expected to marshal to the app thread and return.

  SIGPIPE is blocked before the first write. A client that closes its stdout
  while we are answering must turn into a failed() server, never a dead process,
  which is also why write errors are recorded rather than thrown.

  failed() and lastError() are the only members read from another thread, so only
  they take the mutex. Both are cheap enough to poll from SDL_AppIterate.

  EOF means the client is gone and is a clean exit; a read error, a write error,
  or an exception escaping a handler is fatal and sets failed(). Notifications
  produce no response and do not stop the loop; parse and invalid-request errors
  are answered and the session continues.

  POSIX only for now. Windows binary stdio and a wake-up handle arrive with the
  platform step; stop() currently relies on the 50 ms poll tick.
*/

#ifndef SDL3MCP_SERVER_HPP
#define SDL3MCP_SERVER_HPP

#include "json_rpc.hpp"
#include "session.hpp"
#include "transport.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace sdl3mcp
{

/* What a tool handler returns: MCP content blocks plus the error flag. */
struct ToolResult
{
    nlohmann::json content;
    bool isError = false;
};

using ToolHandler = std::function<ToolResult(const ToolCall &)>;

class Server
{
public:
    /* The session outlives the server; both must outlive the join. */
    Server(int readFd,
           int writeFd,
           Session &session,
           ToolHandler handler,
           size_t maxLineBytes = 1024 * 1024);
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    /* Starts the thread. False with a message when the thread cannot be created. */
    bool start(std::string &error);

    /* Asks the reader to quit. Returns at once; join() waits for the thread. */
    void stop();

    /* Waits for the thread to exit, at most one poll interval after stop(). */
    void join();

    /* True once the loop stopped because of a read, write, or handler failure. */
    bool failed() const;

    /* The failure that set failed(); empty otherwise. */
    std::string lastError() const;

private:
    void run();
    void blockSigpipe();
    bool handleLine(std::string_view line);
    bool writeResponse(const nlohmann::json &response);
    bool writeToolCall(const ToolCall &call);
    void setFailure(std::string message);

    Transport transport_;
    Session &session_;
    ToolHandler handler_;

    mutable std::mutex mutex_;
    std::string lastError_;
    bool failed_ = false;

    std::thread thread_;
    bool started_ = false;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_SERVER_HPP */
