#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Posix.h"
#include "Web-Run.h"

namespace sdl3mcp {
namespace {

/* pidfd_open(2) is what lets one poll set watch the web stack's processes. */
int openPidFd(pid_t pid) {
#ifdef SYS_pidfd_open
   return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
   errno = ENOSYS;
   return -1;
#endif
}

/* One non-blocking connect: true only when the loopback port accepts now. */
bool loopbackPortAccepts(std::uint16_t port) {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      return false;
   }
   int ignored = 0;
   (void)setNonBlocking(fd, ignored);

   sockaddr_in address{};
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = htons(port);

   bool accepts = false;
   if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) ==
       0) {
      accepts = true;
   } else if (errno == EINPROGRESS) {
      pollfd descriptor{fd, POLLOUT, 0};
      if (::poll(&descriptor, 1, 0) > 0) {
         int socketError = 0;
         socklen_t length = sizeof socketError;
         if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 &&
             socketError == 0) {
            accepts = true;
         }
      }
   }

   ::close(fd);
   return accepts;
}

}  // namespace

Result<void, TransportError> WebRun::startChild(
    Child& child, const std::vector<std::string>& command) {
   if (command.empty()) {
      return TransportError{"the web command is empty"};
   }

   std::vector<char*> argv;
   argv.reserve(command.size() + 1);
   for (const std::string& argument : command) {
      argv.push_back(const_cast<char*>(argument.c_str()));
   }
   argv.push_back(nullptr);

   const pid_t pid = ::fork();
   if (pid < 0) {
      return TransportError{std::string("cannot fork the web child: ") +
                            std::strerror(errno)};
   }

   if (pid == 0) {
      /* Only async-signal-safe calls from here to execvp. */
      sigset_t empty;
      sigemptyset(&empty);
      ::sigprocmask(SIG_SETMASK, &empty, nullptr);
      (void)::setpgid(0, 0);

      /* The Handler's SIGCHLD handling and ignored SIGPIPE are its own. */
      struct sigaction action{};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      (void)::sigaction(SIGCHLD, &action, nullptr);
      (void)::sigaction(SIGPIPE, &action, nullptr);

      /*
        The client's stdio is the MCP channel when the Handler serves it: a page
        server or a browser that read stdin would eat requests, and one that
        wrote stdout would corrupt the answers. Their output goes to the
        Handler's stderr with the rest of the web diagnostics.
      */
      const int devNull = ::open("/dev/null", O_RDONLY);
      if (devNull >= 0) {
         ::dup2(devNull, STDIN_FILENO);
         if (devNull != STDIN_FILENO) {
            ::close(devNull);
         }
      }
      ::dup2(STDERR_FILENO, STDOUT_FILENO);

      closeDescriptorsFrom(3);
      ::execvp(argv[0], argv.data());
      ::_exit(127);
   }

   /* Both sides set the group so a stop cannot race the child's own setpgid. */
   (void)::setpgid(pid, pid);

   const int pidfd = openPidFd(pid);
   if (pidfd < 0) {
      const int watchError = errno;
      (void)::kill(-pid, SIGKILL);
      (void)::kill(pid, SIGKILL);
      return TransportError{std::string("cannot watch the web child: ") +
                            std::strerror(watchError)};
   }

   child.pid = pid;
   child.pidfd = pidfd;
   child.started = true;
   child.exited = false;
   child.exitStatus = 0;
   return {};
}

Result<void, TransportError> WebRun::startBrowser() {
   const Result<std::vector<std::vector<std::string>>, TransportError> plan =
       makeWebLaunchPlan(options_);
   if (!plan) {
      return plan.error();
   }
   const Result<void, TransportError> started =
       startChild(browser_, (*plan)[2]);
   if (!started) {
      return started.error();
   }
   browserPid_ = browser_.pid;
   return {};
}

bool WebRun::bothPortsAccept() const {
   return loopbackPortAccepts(options_.pagePort) &&
          loopbackPortAccepts(options_.bridgePort);
}

}  // namespace sdl3mcp
