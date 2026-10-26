/*
  The child application as a forked process with private pipes.

  The supervisor owns exactly one child: its stdin, stdout, and stderr are
  dedicated pipes, so the supervisor's own stdin/stdout stay with the MCP client.
  Closing the parent copies of the child ends on exec keeps every descriptor out
  of any later child, which matters once the app itself starts processes.

  Exec failures cannot be seen by the child (it is already gone), so the child
  writes a message into a close-on-exec error pipe before _exit(127). The pipe
  closes on a successful exec, so reading EOF from it is the success signal.

  A monitor thread is the only waitpid() caller, so exit is a value the
  supervisor can poll or wait for instead of reaping in more than one place. A
  second thread drains stderr into a bounded ring of the last lines, which both
  keeps the child from blocking on a full pipe and gives a startup failure
  something human to report.

  terminate() is SIGTERM first, then SIGKILL after the grace period, and it
  joins both threads, so the stderr tail is complete when it returns. No SDL and
  no shell are involved. POSIX only, matching the rest of the project.
*/

#ifndef SDL3MCP_SUPERVISOR_CHILD_PROCESS_HPP
#define SDL3MCP_SUPERVISOR_CHILD_PROCESS_HPP

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sdl3mcp
{

struct ChildCommand
{
    /* Executable path or name; resolved with execvp, never through a shell. */
    std::string executable;
    /* argv after argv[0]. */
    std::vector<std::string> arguments;
};

class ChildProcess
{
public:
    static constexpr int kDefaultTerminateGraceMs = 1000;

    ChildProcess();
    ~ChildProcess();

    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    /*
      Forks and execs. False with `error` only when the process could not be
      created at all; an exec failure surfaces through waitForExec() and the
      child's exit status.
    */
    bool start(const ChildCommand &command, std::string &error);

    /* The parent ends. Valid after a successful start() and owned by this object. */
    int stdinFd() const noexcept { return stdinFd_; }
    int stdoutFd() const noexcept { return stdoutFd_; }

    /*
      Waits up to timeoutMs for the exec to either close the error pipe (true) or
      report a failure (false, `error` carries the reported text).
    */
    bool waitForExec(int timeoutMs, std::string &error);

    bool hasExited() const;
    /* True when the child was reaped within timeoutMs. */
    bool waitForExit(int timeoutMs);
    /* Blocks until the child is reaped. */
    void waitForExit();

    /* The exit status, or 128 + signal. Meaningful once hasExited(). */
    int exitCode() const;

    /* The last lines the child wrote to stderr, without a trailing newline. */
    std::string stderrTail() const;

    /*
      SIGTERM, wait up to graceMs, then SIGKILL. Idempotent; joins the monitor
      and stderr threads so the stderr tail is final when it returns.
    */
    void terminate(int graceMs = kDefaultTerminateGraceMs);

private:
    static constexpr size_t kStderrLineLimit = 50;
    static constexpr size_t kStderrPendingLimit = 64 * 1024;

    void monitorLoop();
    void stderrLoop();
    void appendStderrLine(std::string line);
    void joinThreads();
    void closeDescriptors();
    void closeIfOpen(int &fd);

    pid_t pid_ = -1;
    int stdinFd_ = -1;
    int stdoutFd_ = -1;
    int stderrFd_ = -1;
    int errorFd_ = -1;

    std::thread monitor_;
    std::thread stderrReader_;

    mutable std::mutex exitMutex_;
    std::condition_variable exitCondition_;
    bool exited_ = false;
    int exitCode_ = 0;

    mutable std::mutex stderrMutex_;
    std::deque<std::string> stderrLines_;

    std::atomic<bool> stderrStop_{false};
};

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_CHILD_PROCESS_HPP */
