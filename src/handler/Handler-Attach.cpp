#include "Handler.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Configuration.h"
#include "SocketStream.h"

namespace sdl3mcp {

void Handler::acceptApplication() {
   const Result<std::optional<int>, TransportError> accepted =
       listener_.accept();
   if (!accepted) {
      std::fprintf(stderr, "sdl3mcp: %s\n", accepted.error().message.c_str());
      std::fflush(stderr);
      return;
   }
   if (!accepted->has_value()) {
      return;
   }
   const int fd = **accepted;

   /*
     Exactly one application session. The listener keeps accepting only so a
     second connection is closed at once instead of waiting in the backlog.
   */
   if (appPeer_ != nullptr) {
      ::close(fd);
      return;
   }

   auto socket = std::make_unique<SocketStream>();
   const Result<void, TransportError> adopted = socket->adopt(
       fd, endpointText(options_.listenHost, listener_.boundPort()));
   if (!adopted) {
      std::fprintf(stderr, "sdl3mcp: %s\n", adopted.error().message.c_str());
      std::fflush(stderr);
      return;
   }

   auto peer = std::make_unique<AppPeer>(
       std::move(socket), *this, static_cast<std::uint32_t>(options_.timeoutMs));
   const Result<void, TransportError> started = peer->start();
   if (!started) {
      std::fprintf(stderr, "sdl3mcp: %s\n", started.error().message.c_str());
      std::fflush(stderr);
      return;
   }

   appPeer_ = std::move(peer);
   appFd_ = fd;
}

void Handler::onAppAttached(std::vector<nlohmann::json> tools) {
   peerTools_ = std::move(tools);
   appReady_ = true;
   rebuildCatalog();
   /* A relay appends its app_spawn_* names, which the client must learn about. */
   publishToolsIfChanged(true);
   completeWaiters("an application session attached");

   if (pendingSpawn_.has_value()) {
      if (pendingSpawn_->isWeb() &&
          pendingSpawn_->pid < 0 && webRun_) {
         pendingSpawn_->pid = webRun_->browserPid();
      }
      const bool isWeb = pendingSpawn_->isWeb();
      completeSpawn("spawned " + pendingSpawn_->alias + " (" +
                        (isWeb ? "browser pid " : "pid ") +
                        std::to_string(pendingSpawn_->pid) + ")",
                    false);
   }
}

void Handler::onAppToolsChanged(std::vector<nlohmann::json> tools) {
   peerTools_ = std::move(tools);
   rebuildCatalog();
   publishToolsIfChanged(true);
}

void Handler::onAppDetached(std::string reason) {
   appReady_ = false;
   peerTools_.clear();
   rebuildCatalog();
   publishToolsIfChanged(true);
   failPendingForwards(reason + " during ", "");
}

}  // namespace sdl3mcp
