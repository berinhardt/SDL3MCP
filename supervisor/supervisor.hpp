/*
  The MCP supervisor: one stable MCP endpoint in front of a restartable SDL app.

  The supervisor speaks MCP on its own stdin/stdout and keeps a child process
  behind it. While idle it advertises only `spawn`; once the child is up it
  advertises the child's tools and forwards calls to it. That way one MCP
  connection survives an app restart without the client seeing a closed pipe.

  Every protocol decision is ordinary code: the line loop uses the library's
  Transport and json_rpc, a state enum decides the tools/list view, and the
  spawn handshake is synchronous so there is never more than one child.

  A monitor thread owns the withdrawal half of a Running -> Idle transition: when
  the child exits on its own it clears the committed tools, sends
  notifications/tools/list_changed, and puts `spawn` back. Responses and
  notifications share one writer mutex, so a monitor notification can never
  interleave with a request's response.

  sdl_terminate is forwarded and then waited on for --terminate-timeout-ms; a
  child that ignores the quit request gets SIGTERM, a short grace, and SIGKILL.
  Every exit path funnels through the same monitor thread, so Idle/spawn and the
  single list_changed are published exactly once. Stdin EOF and SIGINT/SIGTERM
  stop the child the same SIGTERM/grace/SIGKILL way.

  sdl_capture is the one proxied call the supervisor does not pass through
  verbatim: the library answers with a raw RGBA header and payload, and the
  supervisor downscales to max_width, encodes PNG, and base64s it so the
  client-facing image block stays standard. tools/list therefore advertises the
  supervisor's own sdl_capture definition while running.

  The supervisor never calls SDL. It only writes JSON to the child's pipes and
  reads its lines and raw frames.
*/

#ifndef SDL3MCP_SUPERVISOR_SUPERVISOR_HPP
#define SDL3MCP_SUPERVISOR_SUPERVISOR_HPP

#include "child_process.hpp"
#include "cli.hpp"
#include "json_rpc.hpp"
#include "transport.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace sdl3mcp
{

enum class SupervisorState
{
    Idle,
    Starting,
    Running
};

/* A tool call's MCP content plus the error flag, mirroring the server's result. */
struct SupervisorToolResult
{
    nlohmann::json content;
    bool isError = false;
};

/*
  The tools/list body for a state: `spawn` alone while idle, nothing while the
  child starts, and the committed child tools while running. While running,
  sdl_capture is replaced with the supervisor's own definition because
  downscaling is supervisor policy; every other tool keeps the child's entry
  verbatim. Exposed so the Starting view is testable without racing a live spawn.
*/
nlohmann::json supervisorToolList(SupervisorState state, const nlohmann::json &childTools);

/* The spawn tool as advertised while idle. */
nlohmann::json spawnToolDefinition();

/* The sdl_capture the supervisor advertises while running, with max_width. */
nlohmann::json supervisorCaptureToolDefinition();

class Supervisor
{
public:
    Supervisor(int readFd, int writeFd, SupervisorOptions options);
    ~Supervisor();

    Supervisor(const Supervisor &) = delete;
    Supervisor &operator=(const Supervisor &) = delete;

    /* Reads until EOF, stop(), or a fatal read error. Owns and stops the child. */
    int run();

    /* Asks run() to return at its next poll tick. Safe from another thread. */
    void stop();

    /* The current view; main-thread and monitor-thread safe. */
    SupervisorState state() const;

    /* True once a response could not be written, usually because the client left. */
    bool failed() const;

private:
    using Deadline = std::chrono::steady_clock::time_point;

    void monitorLoop();
    void shutdown();

    bool handleLine(const std::string &line);
    void handleRequest(const RpcRequest &request);

    nlohmann::json initializeResult(const nlohmann::json &id, const nlohmann::json &params);
    void handleToolsList(const nlohmann::json &id);
    void handleToolsCall(const nlohmann::json &id, const nlohmann::json &params);

    SupervisorToolResult spawn();
    bool startChild(Deadline deadline, nlohmann::json &window, std::string &failure);
    SupervisorToolResult failSpawn(const std::string &reason);
    SupervisorToolResult terminate(const nlohmann::json &arguments);
    /* Waits briefly for the exit handler to publish Idle after a child exit. */
    void awaitChildExitHandled(ChildProcess *process);

    bool requestChild(std::string_view method,
                      const nlohmann::json &params,
                      nlohmann::json &result,
                      Deadline deadline,
                      std::string &error);
    nlohmann::json forwardChildCall(const nlohmann::json &id,
                                    const std::string &name,
                                    const nlohmann::json &arguments);
    /*
      sdl_capture is forwarded specially: the child answers the raw header plus
      pixel bytes, and the supervisor downscales, PNG-encodes, and base64-encodes
      before replying with the usual image block.
    */
    nlohmann::json forwardCapture(const nlohmann::json &id,
                                  const nlohmann::json &arguments);

    bool writeJson(const nlohmann::json &message);
    void sendToolsListChanged();
    void writeToolResult(const nlohmann::json &id, const SupervisorToolResult &result);

    static void blockSigpipe();
    static int remainingMs(Deadline deadline);

    Transport transport_;
    SupervisorOptions options_;

    /* Serialises every byte written to the client, from either thread. */
    std::mutex writeMutex_;

    mutable std::mutex stateMutex_;
    std::condition_variable monitorCondition_;
    SupervisorState state_ = SupervisorState::Idle;
    nlohmann::json childTools_ = nlohmann::json::array();
    std::unique_ptr<ChildProcess> child_;
    bool stopping_ = false;

    /* Only the main (request) thread touches the members below. */
    std::unique_ptr<Transport> childTransport_;
    long long nextChildRequestId_ = 1;
    bool initialized_ = false;
    std::atomic<bool> failed_{false};

    std::thread monitor_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_SUPERVISOR_HPP */
