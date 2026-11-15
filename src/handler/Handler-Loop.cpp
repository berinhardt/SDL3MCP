#include "Handler.h"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "Configuration.h"
#include "SocketStream.h"
#include "StdioStream.h"

namespace sdl3mcp {
namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

Handler::Handler(HandlerOptions options) : options_(std::move(options)) {
}

Handler::~Handler() {
   shutdown();
}

int Handler::run() {
   /*
     Writes to a closed pipe or socket raise SIGPIPE; the stream reports the
     failure as a value instead, so the default termination is never useful.
   */
   ::signal(SIGPIPE, SIG_IGN);

   const Result<void, ProtocolError> sources = loadSources();
   if (!sources) {
      std::fprintf(stderr, "sdl3mcp: %s\n", sources.error().message.c_str());
      std::fflush(stderr);
      return 2;
   }
   if (options_.hasDumpTools) {
      return printToolCatalog();
   }
   if (options_.dryRun()) {
      return printDryRun();
   }

   buildBaseTools();
   rebuildCatalog();

   const Result<void, TransportError> client = startClient();
   if (!client) {
      std::fprintf(stderr, "sdl3mcp: %s\n", client.error().message.c_str());
      std::fflush(stderr);
      return 1;
   }
   const Result<void, TransportError> listener = startListener();
   if (!listener) {
      std::fprintf(stderr, "sdl3mcp: %s\n", listener.error().message.c_str());
      std::fflush(stderr);
      shutdown();
      return 1;
   }
   std::fprintf(stderr, "sdl3mcp: listening on %s\n",
                endpointText(options_.listenHost, listener_.boundPort()).c_str());
   std::fflush(stderr);

   startSignalFd();
   if (signalFd_ < 0) {
      std::fprintf(stderr, "sdl3mcp: cannot install the signal handler: %s\n",
                   std::strerror(errno));
      std::fflush(stderr);
      shutdown();
      return 1;
   }

   while (running_) {
      std::vector<pollfd> descriptors;
      descriptors.reserve(8);

      descriptors.push_back(pollfd{clientFd_, POLLIN, 0});
      descriptors.push_back(pollfd{listener_.fd(), POLLIN, 0});
      descriptors.push_back(pollfd{appFd_, POLLIN, 0});
      if (appPeer_ && appPeer_->wantsWrite()) {
         descriptors[2].events |= POLLOUT;
      }
      descriptors.push_back(pollfd{signalFd_, POLLIN, 0});
      descriptors.push_back(
          pollfd{pendingSpawn_.has_value() ? pendingSpawn_->errorFd : -1, POLLIN, 0});

      /*
        A stdio client reads stdin and writes stdout, so the pending-write
        descriptor is not the one polled above; a socket client answers with the
        same fd and is watched once. Without this entry a response larger than
        the pipe buffer stalls after the first partial write.
      */
      std::size_t clientWriteIndex = 0;
      if (clientSession_ != nullptr && clientSession_->stream().wantsWrite()) {
         const int outputFd = clientSession_->stream().outputFd();
         if (outputFd < 0) {
            /* Nothing to watch; the stream reports the failure itself. */
         } else if (outputFd == clientFd_) {
            descriptors[0].events |= POLLOUT;
         } else {
            descriptors.push_back(pollfd{outputFd, POLLOUT, 0});
            clientWriteIndex = descriptors.size() - 1;
         }
      }

      const std::size_t webBase = descriptors.size();
      if (webRun_) {
         webRun_->appendPollFds(descriptors);
      }

      const auto beforePoll = Clock::now();
      const int ready = ::poll(descriptors.data(), descriptors.size(),
                               nextPollTimeoutMs(beforePoll));
      if (ready < 0) {
         if (errno == EINTR) {
            continue;
         }
         std::fprintf(stderr, "sdl3mcp: poll failed: %s\n", std::strerror(errno));
         std::fflush(stderr);
         break;
      }

      const auto now = Clock::now();

      if ((descriptors[3].revents & POLLIN) != 0) {
         bool childExited = false;
         signalfd_siginfo info{};
         while (::read(signalFd_, &info, sizeof info) > 0) {
            if (info.ssi_signo == SIGCHLD) {
               childExited = true;
            } else {
               running_ = false;
            }
         }
         if (childExited) {
            reapChildren();
         }
      }

      if (running_ && (descriptors[1].revents & POLLIN) != 0) {
         acceptApplication();
      }

      if (running_ && clientSession_ != nullptr &&
          ((descriptors[0].revents &
            (POLLIN | POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0 ||
           (clientWriteIndex != 0 &&
            (descriptors[clientWriteIndex].revents &
             (POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0))) {
         clientSession_->pump();
         if (!clientSession_->isOpen()) {
            running_ = false;
         }
      }

      if (appPeer_ != nullptr &&
          (descriptors[2].revents &
           (POLLIN | POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0) {
         appPeer_->pump();
      }

      if (pendingSpawn_.has_value() &&
          (descriptors[4].revents &
           (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
         serviceSpawnExec();
      }

      if (running_ && webRun_) {
         serviceWeb(now, descriptors.data() + webBase,
                    descriptors.size() - webBase);
      }

      if (running_) {
         expireWaiters(now);
         expireForwards(now);
         expireSpawn(now);
         if (appPeer_ != nullptr && !appPeer_->isReady() &&
             !appPeer_->isClosed() && now >= appPeer_->handshakeDeadline()) {
            detachApp("the application did not complete the MCP handshake in time");
         }
      }

      if (appPeer_ != nullptr && appPeer_->isClosed()) {
         appPeer_.reset();
         appFd_ = -1;
         appReady_ = false;
      }

      if (clientSession_ != nullptr && !clientSession_->isOpen()) {
         /* The client went away; its completions are already cancelled. */
         waiters_.clear();
         forwards_.clear();
         cancelSpawn();
      }
   }

   shutdown();
   return 0;
}

Result<void, TransportError> Handler::startClient() {
   if (options_.relay()) {
      auto socket = std::make_unique<SocketStream>();
      SocketStream* raw = socket.get();
      auto session = std::make_unique<Session>(catalog_, std::move(socket));

      const Result<void, TransportError> connected = raw->connect(
          options_.connectHost, options_.connectPort,
          static_cast<std::uint32_t>(options_.timeoutMs));
      if (!connected) {
         return TransportError{"--connect: " + connected.error().message};
      }

      clientSession_ = std::move(session);
      clientFd_ = raw->fd();
      return {};
   }

   auto stdio = std::make_unique<StdioStream>();
   StdioStream* raw = stdio.get();
   auto session = std::make_unique<Session>(catalog_, std::move(stdio));
   const Result<void, TransportError> started = raw->start();
   if (!started) {
      return started.error();
   }

   clientSession_ = std::move(session);
   clientFd_ = raw->inputFd();
   return {};
}

Result<void, TransportError> Handler::startListener() {
   return listener_.listen(options_.listenHost, options_.listenPort);
}

void Handler::startSignalFd() {
   sigset_t mask;
   sigemptyset(&mask);
   sigaddset(&mask, SIGINT);
   sigaddset(&mask, SIGTERM);
   /* SIGCHLD is the reaper's wake-up; a spawned app is still never waited for. */
   sigaddset(&mask, SIGCHLD);
   if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
      signalFd_ = -1;
      return;
   }
   signalFd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

void Handler::stopWebRun() {
   if (!webRun_) {
      return;
   }
   webRun_->stop();
   webRun_.reset();
}

void Handler::shutdown() {
   waiters_.clear();
   forwards_.clear();
   /* Spawn-and-forget: drop the bookkeeping, never the process. */
   cancelSpawn();
   /* The web stack is Handler-owned infrastructure and never outlives it. */
   stopWebRun();

   if (appPeer_ != nullptr) {
      std::unique_ptr<AppPeer> peer = std::move(appPeer_);
      appFd_ = -1;
      appReady_ = false;
      peer->close();
   }

   if (clientSession_ != nullptr) {
      clientSession_->close();
      clientSession_.reset();
   }
   clientFd_ = -1;

   listener_.close();
   if (signalFd_ >= 0) {
      ::close(signalFd_);
      signalFd_ = -1;
   }
}

}  // namespace sdl3mcp
