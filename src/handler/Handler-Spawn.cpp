#include "Handler.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Configuration.h"
#include "Posix.h"
#include "ToolArguments.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

void Handler::handleSpawn(const ToolOrder& order,
                          ToolCompletionDelegate& completion,
                          const SpawnTarget& target) {
   if (appPeer_ != nullptr || appReady_ || pendingSpawn_.has_value() ||
       webRun_ != nullptr) {
      completion.complete(ToolResult::text(
          "an application is already attached; one spawn at a time", true));
      return;
   }

   const Result<std::int64_t, ProtocolError> timeout = readTimeoutMs(
       order.arguments, options_.timeoutMs, "app_spawn_" + target.alias);
   if (!timeout) {
      completion.complete(ToolResult::text(timeout.error().message, true));
      return;
   }
   const std::int64_t timeoutMs = *timeout;

   /*
     The listener is bound before the loop accepts tool calls, so its endpoint is
     the one the app is told to dial. A bound port is never zero here.
   */
   const std::string endpoint =
       endpointText(options_.listenHost, listener_.boundPort());

   const Result<void, TransportError> started =
       startTarget(target, endpoint, timeoutMs, completion);
   if (!started) {
      completion.complete(ToolResult::text(
          "app_spawn_" + target.alias + ": " + started.error().message, true));
   }
}

Result<void, TransportError> Handler::startTarget(
    const SpawnTarget& target, std::string_view endpoint,
    std::int64_t timeoutMs, ToolCompletionDelegate& completion) {
   std::vector<std::string> command = targetCommand(target, endpoint);
   std::vector<char*> argv;
   argv.reserve(command.size() + 1);
   for (std::string& argument : command) {
      argv.push_back(const_cast<char*>(argument.c_str()));
   }
   argv.push_back(nullptr);

   int execPipe[2] = {-1, -1};
   if (::pipe(execPipe) != 0) {
      return TransportError{std::string("cannot create the exec channel: ") +
                            std::strerror(errno)};
   }
   for (const int fd : execPipe) {
      const int flags = ::fcntl(fd, F_GETFD);
      if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
         const std::string message =
             std::string("cannot make the exec channel close-on-exec: ") +
             std::strerror(errno);
         ::close(execPipe[0]);
         ::close(execPipe[1]);
         return TransportError{message};
      }
   }
   const int readFlags = ::fcntl(execPipe[0], F_GETFL);
   if (readFlags >= 0) {
      (void)::fcntl(execPipe[0], F_SETFL, readFlags | O_NONBLOCK);
   }

   const std::string execFailure = "cannot execute " + command.front();

   const pid_t pid = ::fork();
   if (pid < 0) {
      const std::string message =
          std::string("cannot fork the application: ") + std::strerror(errno);
      ::close(execPipe[0]);
      ::close(execPipe[1]);
      return TransportError{message};
   }

   if (pid == 0) {
      /* Child: only async-signal-safe calls from here to execvp. */
      sigset_t empty;
      sigemptyset(&empty);
      ::sigprocmask(SIG_SETMASK, &empty, nullptr);

      /* The Handler's SIGCHLD handling is its own; the app gets the default. */
      struct sigaction action{};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      ::sigaction(SIGCHLD, &action, nullptr);
      /* The Handler ignores SIGPIPE for its own streams; an app gets the default. */
      ::sigaction(SIGPIPE, &action, nullptr);

      /* The app's stdout must not reach the Handler's MCP stream. */
      const int devNull = ::open("/dev/null", O_RDONLY);
      if (devNull >= 0) {
         ::dup2(devNull, STDIN_FILENO);
         if (devNull != STDIN_FILENO) {
            ::close(devNull);
         }
      }
      ::dup2(STDERR_FILENO, STDOUT_FILENO);

      /* Keep the exec channel on fd 3; everything else above 2 is not the app's. */
      ::dup2(execPipe[1], 3);
      (void)::fcntl(3, F_SETFD, 0);
      closeDescriptorsFrom(4);

      ::execvp(argv[0], argv.data());

      const ssize_t ignored =
          ::write(3, execFailure.data(), execFailure.size());
      (void)ignored;
      ::_exit(127);
   }

   ::close(execPipe[1]);

   PendingSpawn pending;
   pending.alias = target.alias;
   pending.pid = pid;
   pending.errorFd = execPipe[0];
   pending.completion = &completion;
   pending.deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
   pending.timeoutMs = timeoutMs;
   pendingSpawn_ = std::move(pending);
   return {};
}

/*
  The exec channel is close-on-exec: EOF means execvp succeeded and the process
  is running, while any bytes are the reason it could not exec. A failed exec is
  the only spawn outcome visible without waiting for the child, which
  spawn-and-forget forbids.
*/
void Handler::serviceSpawnExec() {
   if (!pendingSpawn_.has_value() || pendingSpawn_->errorFd < 0) {
      return;
   }

   char buffer[512];
   for (;;) {
      const ssize_t count = ::read(pendingSpawn_->errorFd, buffer, sizeof buffer);
      if (count > 0) {
         pendingSpawn_->execError.append(buffer, static_cast<std::size_t>(count));
         continue;
      }
      if (count == 0) {
         /* EOF with no message: exec succeeded, so wait for the handshake. */
         ::close(pendingSpawn_->errorFd);
         pendingSpawn_->errorFd = -1;
         if (!pendingSpawn_->execError.empty()) {
            completeSpawn("app_spawn_" + pendingSpawn_->alias +
                              " failed: " + pendingSpawn_->execError +
                              " (pid " + std::to_string(pendingSpawn_->pid) + ")",
                          true);
         }
         return;
      }
      if (errno == EINTR) {
         continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
         return;
      }

      /* A read failure is not an exec failure; let the handshake deadline decide. */
      ::close(pendingSpawn_->errorFd);
      pendingSpawn_->errorFd = -1;
      return;
   }
}

}  // namespace sdl3mcp
