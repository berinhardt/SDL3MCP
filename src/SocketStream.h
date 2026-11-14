#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "Stream.h"

#ifdef __EMSCRIPTEN__
/* Named without the browser header so the native arm can include this too. */
struct EmscriptenWebSocketOpenEvent;
struct EmscriptenWebSocketMessageEvent;
struct EmscriptenWebSocketCloseEvent;
struct EmscriptenWebSocketErrorEvent;
#endif

namespace sdl3mcp {

/*
  One dialed stream: the library's whole transport.

  Desktop dials a TCP socket, the browser dials a WebSocket, and both present
  the same Stream. The library only ever dials. connect() makes one attempt over
  every address the resolver returns, bounded by connect_timeout_ms, and a
  failure is final: there is no retry and no fallback to "no session", because
  an unreachable debug endpoint must surface as a startup failure.

  The socket is non-blocking and the class is single-threaded: pump() reads what
  is available and delivers complete lines through StreamBase, while writes are
  appended to an outbound buffer and flushed from both sendText/sendBinary and
  pump(). A short write is not a failure; only a real send error is.

  The platform arm is PIMPL so the Emscripten implementation can hold its own
  state in WebSocketStream.cpp without changing this header. There the dial is
  asynchronous: connect() creates the WebSocket and registers callbacks, and
  the open, messages, close and error arrive on the page's own thread.
*/
class SocketStream : public StreamBase {
  public:
   SocketStream();
   ~SocketStream() override;

   SocketStream(const SocketStream&) = delete;
   SocketStream& operator=(const SocketStream&) = delete;

   /*
     Dials host:port once. The native arm completes the connection inside the
     call, so on success the handler has seen onOpen and the stream is ready.
     The browser arm creates the WebSocket and registers its callbacks, so a
     success means the dial is under way and onOpen arrives later; a failure the
     browser reports afterwards reaches the handler as onError/onClose. Either
     way a false return leaves nothing open and `error` names the endpoint plus
     the resolver, timeout, or connect error.
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
   struct Native;
   std::unique_ptr<Native> native_;

#ifdef __EMSCRIPTEN__
   static bool onOpen(int eventType,
                      const EmscriptenWebSocketOpenEvent* event,
                      void* userData);
   static bool onMessage(int eventType,
                         const EmscriptenWebSocketMessageEvent* event,
                         void* userData);
   static bool onClose(int eventType,
                       const EmscriptenWebSocketCloseEvent* event,
                       void* userData);
   static bool onError(int eventType,
                       const EmscriptenWebSocketErrorEvent* event,
                       void* userData);

   /* Copies a message it was handed; the browser owns that memory only until
      the callback returns. */
   void appendBytes(const std::uint8_t* data, std::size_t size, bool isText);
#endif
};

}  // namespace sdl3mcp
