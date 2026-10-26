/*
  The supervisor executable: parse our own options, then hand stdin/stdout to
  the Supervisor. Everything after --target belongs to the app, and the app's
  stdio is a private pipe pair, so the client keeps talking to this process.

  SIGINT and SIGTERM stop the supervisor the same way stdin EOF does: the run
  loop returns and shutdown terminates the child with SIGTERM, a grace period,
  and SIGKILL if it is still alive. A dedicated thread parks in sigwait()
  because the signal handler cannot do the mutex-protected work itself.
*/

#include "cli.hpp"
#include "supervisor.hpp"

#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <unistd.h>

namespace
{

bool blockStopSignals(sigset_t &signals)
{
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
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
        std::fprintf(stderr, "sdl3mcp-supervisor: %s\n%s\n", parsed.error.c_str(),
                     sdl3mcp::usageLine().c_str());
        return 2;
    }

    /*
      Block before any thread or child exists so every thread inherits the mask
      and only the sigwait() thread consumes INT/TERM. The forked child restores
      an empty mask before exec, so the app keeps default signal behaviour.
    */
    sigset_t stopSignals;
    if (!blockStopSignals(stopSignals)) {
        std::fprintf(stderr, "sdl3mcp-supervisor: cannot block stop signals\n");
        return 1;
    }

    sdl3mcp::Supervisor supervisor(STDIN_FILENO, STDOUT_FILENO, parsed.options);

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
