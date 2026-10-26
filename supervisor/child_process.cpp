#include "child_process.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

constexpr int kStderrPollMs = 50;
constexpr size_t kStderrChunkBytes = 4096;

std::string describe(std::string_view context, int errorCode)
{
    return std::string(context) + ": " + std::strerror(errorCode);
}

/* One pipe with both ends close-on-exec, so neither leaks into the child. */
bool createPipe(int descriptors[2], std::string &error)
{
    if (::pipe(descriptors) != 0) {
        error = describe("cannot create a pipe", errno);
        return false;
    }

    for (int index = 0; index < 2; ++index) {
        const int flags = ::fcntl(descriptors[index], F_GETFD);
        if (flags < 0 || ::fcntl(descriptors[index], F_SETFD, flags | FD_CLOEXEC) < 0) {
            error = describe("cannot make a pipe close-on-exec", errno);
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            descriptors[0] = -1;
            descriptors[1] = -1;
            return false;
        }
    }
    return true;
}

void closePipe(int descriptors[2])
{
    ::close(descriptors[0]);
    ::close(descriptors[1]);
}

int decodeStatus(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 0;
}

} // namespace

ChildProcess::ChildProcess() = default;

ChildProcess::~ChildProcess()
{
    terminate();
    closeDescriptors();
}

bool ChildProcess::start(const ChildCommand &command, std::string &error)
{
    error.clear();
    if (pid_ > 0) {
        error = "the child is already started";
        return false;
    }
    if (command.executable.empty()) {
        error = "no executable was given";
        return false;
    }

    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};

    if (!createPipe(stdinPipe, error) || !createPipe(stdoutPipe, error) ||
        !createPipe(stderrPipe, error) || !createPipe(errorPipe, error)) {
        /* createPipe closes its own failure; close the ones already made. */
        if (stdinPipe[0] >= 0) {
            closePipe(stdinPipe);
        }
        if (stdoutPipe[0] >= 0) {
            closePipe(stdoutPipe);
        }
        if (stderrPipe[0] >= 0) {
            closePipe(stderrPipe);
        }
        if (errorPipe[0] >= 0) {
            closePipe(errorPipe);
        }
        return false;
    }

    /* argv storage must be built before fork: the child cannot allocate. */
    std::vector<std::string> storage;
    storage.reserve(command.arguments.size() + 1);
    storage.push_back(command.executable);
    for (const std::string &argument : command.arguments) {
        storage.push_back(argument);
    }

    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (std::string &argument : storage) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const std::string execFailure = "cannot execute " + command.executable;

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = describe("cannot fork the child", errno);
        closePipe(stdinPipe);
        closePipe(stdoutPipe);
        closePipe(stderrPipe);
        closePipe(errorPipe);
        return false;
    }

    if (pid == 0) {
        /* Child: only async-signal-safe calls from here to execvp. */
        ::dup2(stdinPipe[0], STDIN_FILENO);
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::dup2(stderrPipe[1], STDERR_FILENO);

        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]);
        ::close(stdoutPipe[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        ::close(errorPipe[0]);

        /* A blocked SIGPIPE is per process image; the child wants the default. */
        sigset_t empty;
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);

        ::execvp(argv[0], argv.data());

        const ssize_t ignored =
            ::write(errorPipe[1], execFailure.data(), execFailure.size());
        (void)ignored;
        ::_exit(127);
    }

    pid_ = pid;
    stdinFd_ = stdinPipe[1];
    stdoutFd_ = stdoutPipe[0];
    stderrFd_ = stderrPipe[0];
    errorFd_ = errorPipe[0];

    ::close(stdinPipe[0]);
    ::close(stdoutPipe[1]);
    ::close(stderrPipe[1]);
    ::close(errorPipe[1]);

    try {
        monitor_ = std::thread(&ChildProcess::monitorLoop, this);
        stderrReader_ = std::thread(&ChildProcess::stderrLoop, this);
    } catch (const std::system_error &exception) {
        error = std::string("cannot start the child monitor: ") + exception.what();
        ::kill(pid_, SIGKILL);
        if (monitor_.joinable()) {
            /* The monitor is the waitpid caller; let it reap this child. */
            monitor_.join();
        } else {
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        pid_ = -1;
        stderrStop_.store(true, std::memory_order_relaxed);
        joinThreads();
        closeDescriptors();
        return false;
    }

    return true;
}

bool ChildProcess::waitForExec(int timeoutMs, std::string &error)
{
    error.clear();
    if (errorFd_ < 0) {
        error = "the child has no exec channel";
        return false;
    }

    pollfd descriptor{};
    descriptor.fd = errorFd_;
    descriptor.events = POLLIN;

    int ready = 0;
    do {
        ready = ::poll(&descriptor, 1, timeoutMs);
    } while (ready < 0 && errno == EINTR);

    if (ready < 0) {
        error = describe("cannot wait for the child to start", errno);
        closeIfOpen(errorFd_);
        return false;
    }
    if (ready == 0) {
        error = "the target did not report a successful start within " +
                std::to_string(timeoutMs) + " ms";
        closeIfOpen(errorFd_);
        return false;
    }

    char buffer[512];
    ssize_t count = 0;
    do {
        count = ::read(errorFd_, buffer, sizeof buffer - 1);
    } while (count < 0 && errno == EINTR);
    closeIfOpen(errorFd_);

    if (count <= 0) {
        /* The pipe closed at exec: the child is running. */
        return true;
    }

    std::string message(buffer, static_cast<size_t>(count));
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    error = message.empty() ? std::string("the target could not be executed") : message;
    return false;
}

bool ChildProcess::hasExited() const
{
    const std::lock_guard<std::mutex> lock(exitMutex_);
    return exited_;
}

bool ChildProcess::waitForExit(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(exitMutex_);
    return exitCondition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [this] { return exited_; });
}

void ChildProcess::waitForExit()
{
    std::unique_lock<std::mutex> lock(exitMutex_);
    exitCondition_.wait(lock, [this] { return exited_; });
}

int ChildProcess::exitCode() const
{
    const std::lock_guard<std::mutex> lock(exitMutex_);
    return exitCode_;
}

std::string ChildProcess::stderrTail() const
{
    const std::lock_guard<std::mutex> lock(stderrMutex_);

    std::string text;
    for (const std::string &line : stderrLines_) {
        if (!text.empty()) {
            text.push_back('\n');
        }
        text += line;
    }
    return text;
}

void ChildProcess::terminate(int graceMs)
{
    if (pid_ > 0 && !hasExited()) {
        ::kill(pid_, SIGTERM);
        if (!waitForExit(graceMs)) {
            ::kill(pid_, SIGKILL);
            /* A SIGKILLed child is reaped immediately; the bound only guards a
               monitor thread that cannot make progress. */
            (void)waitForExit(5000);
        }
    }

    stderrStop_.store(true, std::memory_order_relaxed);
    joinThreads();
    closeIfOpen(errorFd_);
}

void ChildProcess::monitorLoop()
{
    int status = 0;
    pid_t result = 0;
    do {
        result = ::waitpid(pid_, &status, 0);
    } while (result < 0 && errno == EINTR);

    {
        const std::lock_guard<std::mutex> lock(exitMutex_);
        exited_ = true;
        exitCode_ = result > 0 ? decodeStatus(status) : 0;
        if (result < 0) {
            exitCode_ = -1;
        }
    }
    exitCondition_.notify_all();
}

void ChildProcess::stderrLoop()
{
    std::string pending;

    for (;;) {
        if (stderrStop_.load(std::memory_order_relaxed)) {
            break;
        }

        pollfd descriptor{};
        descriptor.fd = stderrFd_;
        descriptor.events = POLLIN;

        const int ready = ::poll(&descriptor, 1, kStderrPollMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }

        char chunk[kStderrChunkBytes];
        const ssize_t count = ::read(stderrFd_, chunk, sizeof chunk);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
        if (count == 0) {
            break;
        }

        pending.append(chunk, static_cast<size_t>(count));
        size_t newline = pending.find('\n');
        while (newline != std::string::npos) {
            appendStderrLine(pending.substr(0, newline));
            pending.erase(0, newline + 1);
            newline = pending.find('\n');
        }
        /* A child that never writes a newline must not grow our memory. */
        if (pending.size() > kStderrPendingLimit) {
            appendStderrLine(std::move(pending));
            pending.clear();
        }
    }

    if (!pending.empty()) {
        appendStderrLine(std::move(pending));
    }
}

void ChildProcess::appendStderrLine(std::string line)
{
    const std::lock_guard<std::mutex> lock(stderrMutex_);
    stderrLines_.push_back(std::move(line));
    while (stderrLines_.size() > kStderrLineLimit) {
        stderrLines_.pop_front();
    }
}

void ChildProcess::joinThreads()
{
    if (stderrReader_.joinable()) {
        stderrReader_.join();
    }
    if (monitor_.joinable()) {
        monitor_.join();
    }
}

void ChildProcess::closeDescriptors()
{
    closeIfOpen(stdinFd_);
    closeIfOpen(stdoutFd_);
    closeIfOpen(stderrFd_);
    closeIfOpen(errorFd_);
}

void ChildProcess::closeIfOpen(int &fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace sdl3mcp
