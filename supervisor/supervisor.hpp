/*
  The MCP supervisor: one stable MCP endpoint in front of a restartable SDL app.

  It runs in one of two modes. With --target it speaks MCP on its own stdin/stdout
  and keeps a child process behind it: while idle it advertises only `spawn`,
  once the child is up it advertises the child's tools and forwards calls to it.
  With --listen=HOST:PORT it instead binds a TCP listener and waits for an
  application to dial in: before a session attaches it advertises only
  `wait_session`, and once one is attached it advertises that app's tools and
  forwards calls over the socket. Either way one MCP connection survives an app
  restart without the client seeing a closed pipe.

  Every protocol decision is ordinary code: the line loop uses the library's
  Transport and json_rpc, a state enum decides the tools/list view, and the
  session handshake is synchronous so there is never more than one application.

  A monitor thread owns the withdrawal half of a Running -> Idle transition: when
  the child exits on its own it clears the committed tools, sends
  notifications/tools/list_changed, and puts `spawn` back. In relay mode an
  acceptor thread owns the attach handshake (private initialize + tools/list)
  and closes a second concurrent connection instead of queueing it; the idle
  poll and a failed forward both detach a session that went away and restore
  `wait_session`. Responses and notifications share one writer mutex, so a
  monitor notification can never interleave with a request's response.

  sdl_terminate is forwarded and then waited on for --terminate-timeout-ms; a
  child that ignores the quit request gets SIGTERM, a short grace, and SIGKILL.
  Every exit path funnels through the same monitor thread, so Idle/spawn and the
  single list_changed are published exactly once. Stdin EOF and SIGINT/SIGTERM
  stop the child the same SIGTERM/grace/SIGKILL way. In relay mode the app owns
  its own process, so sdl_terminate is an ordinary forwarded call and the
  session ends when the socket closes.

  sdl_capture is the one proxied call the supervisor does not pass through
  verbatim: the library answers with a raw RGBA header and payload, and the
  supervisor encodes that frame as PNG at exactly the dimensions it arrived
  with, then base64s it so the client-facing image block stays standard. That
  holds for a child and for a relayed session alike. tools/list therefore
  advertises the supervisor's own sdl_capture definition while an app is
  attached. Captures are never resized, so image pixels map 1:1 to app pixels.

  The supervisor never calls SDL. It only writes JSON to the application and
  reads its lines and raw frames.
*/

#ifndef SDL3MCP_SUPERVISOR_SUPERVISOR_HPP
#define SDL3MCP_SUPERVISOR_SUPERVISOR_HPP

#include "child_peer.hpp"
#include "cli.hpp"
#include "json_rpc.hpp"
#include "peer.hpp"
#include "socket_peer.hpp"
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
    Running,
    /* Relay mode with no session attached: `wait_session` is the only tool. */
    WaitingSession,
    /* Relay mode with a session attached: its tools are advertised. */
    PeerAttached
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
  sdl_capture is replaced with the supervisor's own definition because PNG and
  base64 presentation are supervisor policy; every other tool keeps the child's
  entry verbatim. Exposed so the Starting view is testable without racing a live
  spawn.
*/
nlohmann::json supervisorToolList(SupervisorState state, const nlohmann::json &childTools);

/* The spawn tool as advertised while idle. */
nlohmann::json spawnToolDefinition();

/* The relay's only tool before a session attaches. */
nlohmann::json waitSessionToolDefinition();

/* The sdl_capture the supervisor advertises while an app is attached: region and timeout_ms. */
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

    /*
      The current view; main-thread and monitor-thread safe. The value is stable
      by the time a relay client can observe it: attach and detach publish under
      stateMutex_ before the matching tools/list_changed is written.
    */
    SupervisorState state() const;

    /* True once a response could not be written, usually because the client left. */
    bool failed() const;

private:
    using Deadline = std::chrono::steady_clock::time_point;

    void monitorLoop();
    void acceptLoop();
    /* Binds the relay listener, prints the endpoint, and starts the acceptor thread. */
    bool listen(std::string &error);
    void attachPeer(int fd);
    bool initializePeer(Peer *peer, Deadline deadline, nlohmann::json &tools,
                        std::string &failure);
    /* Ends an attached relay session once. */
    void detachPeer(Peer *peer);
    void pollRelayPeer();
    SupervisorToolResult waitSession(const nlohmann::json &arguments);
    /* The peer the proxy is talking to right now, or nullptr when there is none. */
    Peer *dataPath();
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
    void awaitChildExitHandled(ChildPeer *process);

    bool requestPeer(Peer *peer,
                     std::string_view method,
                     const nlohmann::json &params,
                     nlohmann::json &result,
                     Deadline deadline,
                     std::string &error);
    nlohmann::json forwardChildCall(const nlohmann::json &id,
                                    const std::string &name,
                                    const nlohmann::json &arguments);
    /*
      sdl_capture is forwarded specially: the app answers the raw header plus
      pixel bytes, and the supervisor PNG-encodes the frame at the dimensions it
      arrived with and base64-encodes it before replying with the usual image
      block.
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
    nlohmann::json peerTools_ = nlohmann::json::array();
    std::unique_ptr<ChildPeer> child_;
    bool stopping_ = false;
    /* Set by stop(); a wait_session parked on the condition notices it. */
    bool stopRequested_ = false;

    /* The data path the proxy loop uses, swapped under stateMutex_. A child peer
       is owned by child_; a relay peer by relayPeer_. */
    Peer *peer_ = nullptr;
    std::unique_ptr<Peer> relayPeer_;
    /* The peer whose handshake is in progress, so shutdown can unblock it. */
    Peer *pendingPeer_ = nullptr;
    std::atomic<long long> nextRequestId_{1};
    bool initialized_ = false;
    std::atomic<bool> failed_{false};

    std::thread monitor_;
    std::thread acceptor_;
    int listenerFd_ = -1;
    std::uint16_t boundPort_ = 0;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_SUPERVISOR_HPP */
