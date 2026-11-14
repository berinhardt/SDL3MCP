#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "Stream.h"

namespace sdl3mcp {

/*
  One native TCP stream: the library's whole transport.

  The library only ever dials. connect() makes exactly one attempt over every
  address the resolver returns, bounded by connect_timeout_ms, and a failure is
  final: there is no retry and no fallback to "no session", because an
  unreachable debug endpoint must surface as a startup failure.

  The socket is non-blocking and the class is single-threaded: pump() reads what
  is available and delivers complete lines through StreamBase, while writes are
  appended to an outbound buffer and flushed from both sendText/sendBinary and
  pump(). A short write is not a failure; only a real send error is.

  The platform arm is PIMPL so the Emscripten implementation can hold its own
  state in WebSocketStream.cpp without changing this header.
*/
class SocketStream : public StreamBase {
  public:
   SocketStream();
   ~SocketStream() override;

   SocketStream(const SocketStream&) = delete;
   SocketStream& operator=(const SocketStream&) = delete;

   /*
     Dials host:port once. On success the stream's handler has seen onOpen and
     the stream is ready for sendText/sendBinary/pump; on failure nothing is
     left open and `error` names the endpoint plus the resolver, timeout, or
     connect error.
   */
   bool connect(std::string_view host,
                std::uint16_t port,
                std::uint32_t timeoutMs,
                std::string& error);

   bool sendText(std::string_view text) override;
   bool sendBinary(const std::uint8_t* data, std::size_t size) override;
   void close() override;
   void pump() override;

  private:
   /* Writes as much of the outbound buffer as the socket accepts. */
   bool flush();
   void reportWriteFailure(int error);

   struct Native;
   std::unique_ptr<Native> native_;
};

}  // namespace sdl3mcp
