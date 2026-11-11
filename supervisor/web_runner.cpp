#include "web_runner.hpp"

#include "child_process.hpp"
#include "socket_connect.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

constexpr int kReadyPollMs = 25;
constexpr int kSupervisePollMs = 50;
constexpr int kStopGraceMs = 1000;
constexpr size_t kChildCount = 3;
constexpr size_t kBrowserIndex = 2;

/* Plan order, so one index names both the command and the child running it. */
constexpr const char *kChildNames[kChildCount] = {"page server", "bridge", "browser"};

struct ChildRun
{
    const char *name;
    ChildProcess process;
    bool started = false;
};

enum class Readiness
{
    Ready,
    TimedOut,
    ChildExited
};

int remainingMs(std::chrono::steady_clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return left.count() > 0 ? static_cast<int>(left.count()) : 0;
}

/*
  True when something is listening on the loopback port. The probe closes at
  once: a real WebSocket bridge serves the next connection just the same, and a
  refused connect is the only thing this asks about.
*/
bool portAccepts(std::uint16_t port)
{
    int fd = -1;
    std::string error;
    if (!connectSocket(kWebLoopbackHost, port, fd, error)) {
        return false;
    }
    ::close(fd);
    return true;
}

std::string exitReport(const ChildRun &child)
{
    return std::string("the ") + child.name + " exited (status " +
           std::to_string(child.process.exitCode()) + ")";
}

bool startChild(ChildRun &child, const std::vector<std::string> &command)
{
    if (command.empty()) {
        std::fprintf(stderr, "sdl3mcp: web: the %s has no command\n", child.name);
        return false;
    }

    ChildCommand request;
    request.executable = command.front();
    request.arguments.assign(command.begin() + 1, command.end());
    /* Its MCP channel is the bridge, so stdio stays the terminal's. */
    request.inheritStdio = true;
    /* One signal must reach whatever the child forks as well. */
    request.newProcessGroup = true;

    std::string error;
    if (!child.process.start(request, error)) {
        std::fprintf(stderr, "sdl3mcp: web: cannot start the %s: %s\n", child.name,
                     error.c_str());
        return false;
    }
    child.started = true;
    return true;
}

/*
  SIGTERM to every group, one grace period for all of them, SIGKILL for whatever
  is still alive, then a wait for every child. Returns true when a stop had to
  escalate.
*/
bool stopChildren(std::array<ChildRun, kChildCount> &children)
{
    for (ChildRun &child : children) {
        if (child.started) {
            child.process.requestStop();
        }
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kStopGraceMs);
    for (ChildRun &child : children) {
        if (child.started) {
            (void)child.process.waitForExit(remainingMs(deadline));
        }
    }

    bool escalated = false;
    for (ChildRun &child : children) {
        if (child.started && !child.process.hasExited()) {
            child.process.forceStop();
            escalated = true;
        }
    }
    for (ChildRun &child : children) {
        if (child.started) {
            child.process.finish();
        }
    }
    return escalated;
}

std::string removalFailure(const std::string &path, int errorCode)
{
    return "cannot remove " + path + ": " + std::strerror(errorCode);
}

/* A browser leaves thousands of files, so the profile needs a whole tree gone. */
bool removeTree(const std::string &path, std::string &error)
{
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        const int lookupError = errno;
        if (lookupError == ENOENT) {
            return true;
        }
        error = removalFailure(path, lookupError);
        return false;
    }
    if (!S_ISDIR(status.st_mode)) {
        if (::unlink(path.c_str()) == 0) {
            return true;
        }
        error = removalFailure(path, errno);
        return false;
    }

    DIR *directory = ::opendir(path.c_str());
    if (directory == nullptr) {
        error = removalFailure(path, errno);
        return false;
    }

    bool removed = true;
    std::string firstFailure;
    while (const dirent *entry = ::readdir(directory)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        std::string entryFailure;
        if (!removeTree(path + "/" + name, entryFailure) && removed) {
            removed = false;
            firstFailure = std::move(entryFailure);
        }
    }
    ::closedir(directory);

    if (!removed) {
        error = std::move(firstFailure);
        return false;
    }
    if (::rmdir(path.c_str()) != 0) {
        error = removalFailure(path, errno);
        return false;
    }
    return true;
}

/* Stops everything, then takes the throwaway profile with it. */
WebRunOutcome finishRun(std::array<ChildRun, kChildCount> &children,
                        WebRunOutcome outcome,
                        const WebAppOptions &options)
{
    outcome.escalated = stopChildren(children);
    if (outcome.escalated) {
        std::fprintf(stderr, "sdl3mcp: web: a process outlived the grace period and was killed\n");
    }

    if (!options.profileDir.empty() && !removeTree(options.profileDir, outcome.cleanupError)) {
        std::fprintf(stderr, "sdl3mcp: web: %s\n", outcome.cleanupError.c_str());
    }
    return outcome;
}

/*
  Waits no longer than the ready window for both ports, and reports a child that
  died while it waited. A window that expires without an exit is not a failure:
  the caller launches the browser against a server that is still coming up.
*/
Readiness waitForPorts(std::array<ChildRun, kChildCount> &children,
                       const WebAppOptions &options,
                       size_t &exitedIndex)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kWebReadyWindowMs);
    for (;;) {
        for (size_t index = 0; index < kBrowserIndex; ++index) {
            if (children[index].process.hasExited()) {
                exitedIndex = index;
                return Readiness::ChildExited;
            }
        }
        if (portAccepts(options.pagePort) && portAccepts(options.bridgePort)) {
            return Readiness::Ready;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Readiness::TimedOut;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kReadyPollMs));
    }
}

} // namespace

WebRunOutcome runWebApp(const WebAppOptions &options,
                        const std::vector<std::vector<std::string>> &plan,
                        const std::atomic<bool> &stopRequested)
{
    WebRunOutcome outcome;

    std::array<ChildRun, kChildCount> children{};
    for (size_t index = 0; index < kChildCount; ++index) {
        children[index].name = kChildNames[index];
    }

    if (plan.size() != kChildCount) {
        outcome.exitCode = 1;
        outcome.failure = "the launch plan does not hold three commands";
        std::fprintf(stderr, "sdl3mcp: web: %s\n", outcome.failure.c_str());
        return finishRun(children, outcome, options);
    }

    for (size_t index = 0; index < kBrowserIndex; ++index) {
        if (!startChild(children[index], plan[index])) {
            outcome.exitCode = 1;
            outcome.failure = std::string("the ") + children[index].name +
                              " could not be started";
            return finishRun(children, outcome, options);
        }
    }

    size_t exitedIndex = 0;
    const Readiness readiness = waitForPorts(children, options, exitedIndex);
    if (readiness == Readiness::ChildExited) {
        outcome.exitCode = 1;
        outcome.failure = exitReport(children[exitedIndex]);
        std::fprintf(stderr, "sdl3mcp: web: %s during startup\n", outcome.failure.c_str());
        return finishRun(children, outcome, options);
    }
    if (readiness == Readiness::TimedOut) {
        std::fprintf(stderr,
                     "sdl3mcp: web: the page server and bridge are not accepting "
                     "connections yet; starting the browser anyway\n");
    }

    if (!startChild(children[kBrowserIndex], plan[kBrowserIndex])) {
        outcome.exitCode = 1;
        outcome.failure = "the browser could not be started";
        return finishRun(children, outcome, options);
    }

    for (;;) {
        if (stopRequested.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "sdl3mcp: web: stopping on request\n");
            break;
        }

        bool ended = false;
        for (size_t index = 0; index < kChildCount; ++index) {
            ChildRun &child = children[index];
            if (!child.process.hasExited()) {
                continue;
            }
            ended = true;
            if (index == kBrowserIndex && child.process.exitCode() == 0) {
                /*
                  The page closed itself after the app shut down. That is the
                  normal end of a run, so the rest is stopped and the caller
                  sees a clean outcome rather than a failure.
                */
                std::fprintf(stderr, "sdl3mcp: web: the browser closed; stopping the rest\n");
                break;
            }
            outcome.exitCode = 1;
            outcome.failure = exitReport(child);
            std::fprintf(stderr, "sdl3mcp: web: %s; stopping the rest\n",
                         outcome.failure.c_str());
            break;
        }
        if (ended) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kSupervisePollMs));
    }

    return finishRun(children, outcome, options);
}

} // namespace sdl3mcp
