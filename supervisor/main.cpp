/*
  The supervisor executable: parse our own options, then hand the upstream MCP
  endpoint to the Supervisor. With --target the app is a child behind private
  pipes; with --listen the app dials in; with --connect --targets the supervisor
  dials a relay and spawns the configured applications on demand; with
  --web-root it resolves the browser, seeds the throwaway profile a run owns,
  and launches the page server, the WebSocket bridge and an isolated browser for
  a browser-hosted build. --print-plan prints that plan, and the profile path it
  would seed, instead of creating or starting anything.

  SIGINT and SIGTERM stop either half the same way the client leaving does: the
  run loop returns and shutdown terminates any child with SIGTERM, a grace
  period, and SIGKILL if it is still alive. A dedicated thread parks in sigwait()
  because the signal handler cannot do the mutex-protected work itself.
*/

#include "cli.hpp"
#include "socket_connect.hpp"
#include "socket_peer.hpp"
#include "stdio_peer.hpp"
#include "supervisor.hpp"
#include "web_app.hpp"
#include "web_runner.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
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
  The relay the bridge dials when --connect is not given: the documented local
  relay from the repository's opencode.json. The page and bridge ports come from
  --web-port/--bridge-port, defaulting to the documented pair.
*/
constexpr const char *kDefaultWebRelayHost = "127.0.0.1";
constexpr std::uint16_t kDefaultWebRelayPort = 7777;

bool isShellSafe(char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           character == '_' || character == '@' || character == '%' ||
           character == '+' || character == '=' || character == ':' ||
           character == ',' || character == '.' || character == '/' ||
           character == '-';
}

/* Single-quote anything a shell would otherwise split, glob or expand. */
std::string shellQuote(const std::string &argument)
{
    bool safe = !argument.empty();
    for (const char character : argument) {
        safe = safe && isShellSafe(character);
    }
    if (safe) {
        return argument;
    }

    std::string quoted = "'";
    for (const char character : argument) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
}

/* The plan a launch would use: the resolved inputs, then the three commands. */
void printLaunchPlan(const sdl3mcp::WebAppOptions &web,
                     const std::vector<std::vector<std::string>> &plan)
{
    std::printf("browser=%s\nprofile=%s\n", web.browser.c_str(),
                web.profileDir.c_str());
    for (const std::vector<std::string> &command : plan) {
        std::fputs("command:", stdout);
        for (const std::string &argument : command) {
            std::fputc(' ', stdout);
            std::fputs(shellQuote(argument).c_str(), stdout);
        }
        std::fputc('\n', stdout);
    }
}

sdl3mcp::WebAppOptions webOptions(const sdl3mcp::SupervisorOptions &options)
{
    sdl3mcp::WebAppOptions web;
    web.pageRoot = options.webRoot;
    web.entry = options.webEntry;
    const bool hasRelay = !options.connectHost.empty();
    web.relayHost = hasRelay ? options.connectHost : kDefaultWebRelayHost;
    web.relayPort = hasRelay ? options.connectPort : kDefaultWebRelayPort;
    web.pagePort = options.webPort;
    web.bridgePort = options.bridgePort;
    return web;
}

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

    if (parsed.options.web()) {
        sdl3mcp::WebAppOptions web = webOptions(parsed.options);
        std::string webError;
        /* Preflight first: a missing tool must not leave a half-launched stack. */
        if (!sdl3mcp::preflightWebLaunch(web, webError) ||
            !sdl3mcp::resolveBrowser(web.browser, webError)) {
            std::fprintf(stderr, "sdl3mcp: --web-root: %s\n", webError.c_str());
            return 2;
        }

        /*
          A plan print names the profile a launch would seed and creates
          nothing: the directory belongs to the run that starts children, and a
          printing process has none to remove it.
        */
        if (parsed.options.printPlan) {
            web.profileDir = sdl3mcp::plannedProfilePath();
        } else if (!sdl3mcp::seedProfile(web.profileDir, webError)) {
            std::fprintf(stderr, "sdl3mcp: --web-root: %s\n", webError.c_str());
            return 2;
        }

        std::vector<std::vector<std::string>> plan;
        if (!sdl3mcp::makeLaunchPlan(web, plan, webError)) {
            std::fprintf(stderr, "sdl3mcp: --web-root: %s\n", webError.c_str());
            return 2;
        }

        if (parsed.options.printPlan) {
            printLaunchPlan(web, plan);
            return 0;
        }

        /*
          Block before the launcher forks so every child inherits a mask that
          only the stopper thread empties, and stop() becomes a flag the
          supervisor half polls instead of a handler it cannot install.
        */
        sigset_t webStopSignals;
        if (!blockStopSignals(webStopSignals)) {
            std::fprintf(stderr, "sdl3mcp: cannot block stop signals\n");
            return 1;
        }

        std::atomic<bool> stopRequested{false};
        std::thread webStopper([&stopRequested, &webStopSignals] {
            int received = 0;
            if (sigwait(&webStopSignals, &received) == 0) {
                stopRequested.store(true, std::memory_order_relaxed);
            }
        });

        const sdl3mcp::WebRunOutcome outcome =
            sdl3mcp::runWebApp(web, plan, stopRequested);

        /* Wake a stopper still parked in sigwait(): the signal is blocked, so it
           stays pending until the thread consumes it, then the process exits. */
        ::kill(::getpid(), SIGTERM);
        webStopper.join();

        return outcome.exitCode;
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
