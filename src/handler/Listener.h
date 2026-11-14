/*
  The downstream listener: one TCP socket the Handler accepts applications on.

  The socket is non-blocking, so accept() in the single-threaded loop never
  waits. Port 0 asks the kernel for an ephemeral port and boundPort() reports
  the one it chose, which is what the `listening on` line prints.
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Result.h"

namespace sdl3mcp {

class Listener {
  public:
   Listener() = default;
   ~Listener();

   Listener(const Listener&) = delete;
   Listener& operator=(const Listener&) = delete;

   /* Binds and listens; a failure is a startup error naming the endpoint. */
   Result<void, TransportError> listen(std::string_view host, std::uint16_t port);

   int fd() const noexcept { return fd_; }
   std::uint16_t boundPort() const noexcept { return boundPort_; }

   /*
     Accepts one pending connection. Success with a value is the non-blocking
     fd; success with no value means nothing is pending (EAGAIN); an error is a
     real accept failure.
   */
   Result<std::optional<int>, TransportError> accept();

   void close();

  private:
   int fd_ = -1;
   std::uint16_t boundPort_ = 0;
};

}  // namespace sdl3mcp
