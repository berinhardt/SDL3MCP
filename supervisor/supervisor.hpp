/*
  The MCP supervisor: one stable MCP endpoint in front of a restartable SDL app.

  It runs in one of three modes. With --target it speaks MCP on its own
  stdin/stdout and keeps a child process behind it. With --listen=HOST:PORT it
  binds a TCP listener and waits for an application to dial in, forwarding calls
  over the socket. With --connect=HOST:PORT --targets=FILE it dials a relay and
  spawns the configured applications on demand. Either way one MCP connection
  survives an app restart without the client seeing a closed pipe.

  Every protocol decision is ordinary code: the line loop uses the library's
  Transport and json_rpc, the mode decides the tools/list base, and the session
  handshake is synchronous so there is never more than one application.

  The advertised tool list is a function of topology, never of whether an
  application is attached: spawn mode advertises `spawn` plus the application's
  four tools, relay mode `wait_session` plus those four, and spawner mode one
  `app_spawn_<alias>` per target plus those four. The supervisor merges the
  attached peer's own entries over that base by name, so a spawner behind a
  relay contributes its `app_spawn_*` entries and a bare application is an
  identity. A spawn therefore changes no list a client can observe: names that
  never change cannot arrive late behind an asynchronous list_changed refresh.

  A relay may sit in front of another supervisor (the spawner), so a genuine
  tool-list change can arrive while no call is in flight or in the middle of
  one. The supervisor never trusts a cached list across such an announcement: it
  reads the peer's list again once the current response is written, and only
  then republishes tools/list_changed upstream.

  A monitor thread owns the withdrawal half of a Running -> Idle transition: when
  the child exits on its own it clears the committed tools and puts `spawn` back;
  the advertised list does not change. In relay mode an acceptor thread owns the
  attach handshake (private initialize + tools/list) and closes a second
  concurrent connection instead of queueing it; the idle poll and a failed
  forward both detach a session that went away. Responses and notifications share
  one writer mutex, so a monitor notification can never interleave with a
  request's response.

  sdl_terminate is forwarded and then waited on for --terminate-timeout-ms; a
  child that ignores the quit request gets SIGTERM, a short grace, and SIGKILL.
  Every exit path funnels through the same monitor thread, so Idle/spawn is
  published exactly once. Stdin EOF and SIGINT/SIGTERM stop the child the same
  SIGTERM/grace/SIGKILL way. In relay mode the app owns its own process, so
  sdl_terminate is an ordinary forwarded call and the session ends when the
  socket closes.

  sdl_capture is the one proxied call the supervisor does not pass through
  verbatim: the library answers with a raw RGBA header and payload, and the
  supervisor encodes that frame as PNG at exactly the dimensions it arrived
  with, then base64s it so the client-facing image block stays standard. That
  holds for a child and for a relayed session alike, so the application tool
  block always carries the supervisor's own sdl_capture definition. Captures are
  never resized, so image pixels map 1:1 to app pixels.

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
#include "targets.hpp"

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
    /* Relay mode with no session attached: `wait_session` waits for one. */
    WaitingSession,
    /* Relay or spawner mode with a session attached: calls are forwarded. */
    PeerAttached
};

/* A tool call's MCP content plus the error flag, mirroring the server's result. */
struct SupervisorToolResult
{
    nlohmann::json content;
    bool isError = false;
};

/*
  The tools/list base for a mode, before any attached peer is merged in: `spawn`
  in child mode, `wait_session` in relay mode, one `app_spawn_<alias>` per target
  in spawner mode, and always followed by the application's four tools. It never
  depends on whether an application is attached. sdl_capture is the supervisor's
  own definition because PNG and base64 presentation are supervisor policy.
*/
nlohmann::json supervisorToolList(const SupervisorOptions &options);

/* The four tools an application provides, with sdl_capture already presented. */
const nlohmann::json &applicationToolList();

/*
  Merges the attached peer's entries over `base` by name: the peer's definition
  wins on a collision, except sdl_capture, which always keeps the base's PNG
  definition. Base entries the peer does not advertise stay advertised, and peer
  entries the base does not have are appended in the peer's order.
*/
nlohmann::json mergePeerTools(const nlohmann::json &base, const nlohmann::json &peerTools);

/* The spawn tool as advertised in child mode. */
nlohmann::json spawnToolDefinition();

/* The relay's own tool, advertised in relay mode. */
nlohmann::json waitSessionToolDefinition();

/*
  One spawner tool: app_spawn_<alias> with an optional timeout_ms (defaults to
  5000).
*/
nlohmann::json spawnerToolDefinition(const SpawnTarget &target);

/* The spawner's own tools: one per alias. */
nlohmann::json spawnerToolList(const SpawnTargets &targets);

/* The sdl_capture the supervisor always advertises: region and timeout_ms. */
nlohmann::json supervisorCaptureToolDefinition();

class Supervisor
{
public:
    /*
      `client` is the upstream MCP endpoint: stdio in the default and relay
      modes, a dialed socket in spawner mode. The supervisor owns it.
    */
    Supervisor(std::unique_ptr<Peer> client, SupervisorOptions options);
    ~Supervisor();

    Supervisor(const Supervisor &) = delete;
    Supervisor &operator=(const Supervisor &) = delete;

    /* Reads until EOF, stop(), or a fatal read error. Owns and stops the child. */
    int run();

    /* Asks run() to return at its next poll tick. Safe from another thread. */
    void stop();

    /*
      The current state; main-thread and monitor-thread safe. Attach and detach
      publish it under stateMutex_ before `wait_session` is woken.
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
    /* Spawner: terminate an app whose socket vanished, and release a stale one. */
    void pollSpawnerSession();
    /*
      Reads the attached peer's queued lines with a short bound: an application
      announces a tool-list change at a moment no call is in flight, and the
      relay has to hear it to keep its own view current.
    */
    void drainPeerNotifications();
    /* Marks the view stale when a peer message is notifications/tools/list_changed. */
    void observePeerMessage(const nlohmann::json &message);
    /*
      Takes the attached peer's tool list again after it announced a change and
      republishes it upstream. Responses are already written by the time this
      runs, so a client sees the answer before the announcement.
    */
    void refreshPeerTools();
    /* The post-response and idle half of following a peer's list changes. */
    void servicePeerSession();
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

    /* Spawner mode: bind, start the child, accept exactly one peer, hand off. */
    SupervisorToolResult spawnerSpawn(const std::string &alias,
                                      const nlohmann::json &arguments);
    SupervisorToolResult failSpawnerSpawn(const std::string &alias,
                                          const std::string &reason);

    SupervisorToolResult terminate(const nlohmann::json &arguments);
    /* Waits briefly for the exit handler to publish Idle after a child exit. */
    void awaitChildExitHandled(ChildPeer *process);

    bool requestPeer(Peer *peer,
                     std::string_view method,
                     const nlohmann::json &params,
                     nlohmann::json &result,
                     Deadline deadline,
                     std::string &error);
    /* True when the attached peer advertised `name` in its latest tools/list. */
    bool peerAdvertises(const std::string &name);
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

    std::unique_ptr<Peer> upstream_;
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
    /*
      Set when a peer announced tools/list_changed. The run thread consumes it;
      the acceptor can set it while handshaking a new session, so it is atomic.
    */
    std::atomic<bool> peerToolsDirty_{false};

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
