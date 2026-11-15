#include "Handler.h"

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <utility>

#include "SocketStream.h"
#include "StdioStream.h"

namespace sdl3mcp {

Handler::Handler(HandlerOptions options) : options_(std::move(options)) {
}

Handler::~Handler() {
   shutdown();
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

void Handler::detachApp(std::string reason) {
   if (appPeer_ == nullptr) {
      return;
   }
   std::unique_ptr<AppPeer> peer = std::move(appPeer_);
   appFd_ = -1;
   appReady_ = false;
   /* close() reports onAppDetached once, which fails the remaining forwards. */
   peer->close(std::move(reason));
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
