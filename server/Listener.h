/*
  The downstream listener: one TCP socket the Handler accepts applications on.

  The socket is non-blocking, so accept() in the single-threaded loop never
  waits. Port 0 asks the kernel for an ephemeral port and boundPort() reports
  the one it chose, which is what the `listening on` line prints.
*/

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sdl3mcp {

class Listener {
  public:
   Listener() = default;
   ~Listener();

   Listener(const Listener&) = delete;
   Listener& operator=(const Listener&) = delete;

   /* Binds and listens; a failure is a startup error naming the endpoint. */
   bool listen(std::string_view host, std::uint16_t port, std::string& error);

   int fd() const noexcept { return fd_; }
   std::uint16_t boundPort() const noexcept { return boundPort_; }

   /*
     Accepts one pending connection. Returns the non-blocking fd, or -1 with
     `error` empty when nothing is pending (EAGAIN), and -1 with `error` set on
     a real accept failure.
   */
   int accept(std::string& error);

   void close();

  private:
   int fd_ = -1;
   std::uint16_t boundPort_ = 0;
};

}  // namespace sdl3mcp
