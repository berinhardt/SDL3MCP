/*
  The supervisor executable: parse our own options, then hand the upstream MCP
  endpoint to the Supervisor. With --target the app is a child behind private
  pipes; with --listen the app dials in; with --connect --targets the supervisor
  dials a relay and spawns the configured applications on demand.

  SIGINT and SIGTERM stop the supervisor the same way the client leaving does:
  the run loop returns and shutdown terminates any child with SIGTERM, a grace
  period, and SIGKILL if it is still alive. A dedicated thread parks in sigwait()
  because the signal handler cannot do the mutex-protected work itself.
*/

#include "cli.hpp"
#include "socket_connect.hpp"
#include "socket_peer.hpp"
#include "stdio_peer.hpp"
#include "supervisor.hpp"

#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace
{

/*
  A spawned application must not inherit the relay connection. If it does, the
  relay keeps a socket its child still holds open after this process dies, so no
  EOF ever arrives and the session looks alive to an idle client.
*/
bool makeCloseOnExec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool blockStopSignals(sigset_t &signals)
{
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
}

/*
  The client-facing side: a dialed socket in spawner mode, otherwise the parent
  process's stdin/stdout. A failed dial is fatal and names the endpoint.
*/
std::unique_ptr<sdl3mcp::Peer> openUpstream(const sdl3mcp::SupervisorOptions &options,
                                            std::string &error)
{
    if (options.spawner()) {
        int fd = -1;
        if (!sdl3mcp::connectSocket(options.connectHost.c_str(), options.connectPort, fd,
                                    error)) {
            error = "--connect: " + error;
            return nullptr;
        }
        if (!makeCloseOnExec(fd)) {
            error = "--connect: cannot make the relay connection close-on-exec";
            ::close(fd);
            return nullptr;
        }
        return std::make_unique<sdl3mcp::SocketPeer>(fd);
    }

    return std::make_unique<sdl3mcp::StdioPeer>(STDIN_FILENO, STDOUT_FILENO);
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const sdl3mcp::CliParseResult parsed = sdl3mcp::parseCommandLine(arguments);
    if (!parsed.ok) {
        std::fprintf(stderr, "sdl3mcp: %s\n%s\n", parsed.error.c_str(),
                     sdl3mcp::usageLine().c_str());
        return 2;
    }

    std::string upstreamError;
    std::unique_ptr<sdl3mcp::Peer> upstream = openUpstream(parsed.options, upstreamError);
    if (!upstream) {
        std::fprintf(stderr, "sdl3mcp: %s\n", upstreamError.c_str());
        return 1;
    }

    /*
      Block before any thread or child exists so every thread inherits the mask
      and only the sigwait() thread consumes INT/TERM. The forked child restores
      an empty mask before exec, so the app keeps default signal behaviour.
    */
    sigset_t stopSignals;
    if (!blockStopSignals(stopSignals)) {
        std::fprintf(stderr, "sdl3mcp: cannot block stop signals\n");
        return 1;
    }

    sdl3mcp::Supervisor supervisor(std::move(upstream), parsed.options);

    std::thread stopper([&supervisor, &stopSignals] {
        int received = 0;
        if (sigwait(&stopSignals, &received) == 0) {
            supervisor.stop();
        }
    });

    const int status = supervisor.run();

    /* Wake a stopper still parked in sigwait(): the signal is blocked, so it
       stays pending until the thread consumes it, then the process exits. */
    ::kill(::getpid(), SIGTERM);
    stopper.join();

    return status;
}
