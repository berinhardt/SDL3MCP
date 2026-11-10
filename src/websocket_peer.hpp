/*
  The browser transport: the same bytes, delivered by WebSocket callbacks.

  A browser page has no fd and nothing to block on. The relay link is a
  WebSocket: the browser hands each frame to a callback on the page's main
  thread, and the app's frame loop collects it. This peer is the whole
  adaptation. The callback copies every frame into a queue, pump() hands
  complete lines to the Responder, and ByteSink carries the answers back through
  emscripten_websocket_send_binary - so the protocol layer above sees exactly
  the byte stream it sees on a desktop pipe.

  open(), pump(), every write and every callback run on the browser's main
  thread; there is no server thread here. That is what createOnMainThread =
  false records: the socket belongs to the thread that created it, and that
  thread is the one running the event loop. One thread means the queue needs no
  lock, and a message callback can never interrupt a running pump().

  This is a private header for the Emscripten build. CMake compiles the
  translation unit only when EMSCRIPTEN is set; a desktop build uses the fd
  Transport instead.
*/

#ifndef SDL3MCP_WEBSOCKET_PEER_HPP
#define SDL3MCP_WEBSOCKET_PEER_HPP

#include "byte_sink.hpp"

#include <emscripten/websocket.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sdl3mcp
{

class Responder;

class WebSocketPeer : public ByteSink
{
public:
    WebSocketPeer() = default;
    /* Closes the socket if it is still open, which releases the browser object. */
    ~WebSocketPeer() override;

    WebSocketPeer(const WebSocketPeer &) = delete;
    WebSocketPeer &operator=(const WebSocketPeer &) = delete;

    /*
      Dials ws://host:port. It returns as soon as the browser has the WebSocket
      object: the connection itself completes asynchronously, so a dial that
      fails is reported by failed() and lastError() after the browser calls
      back. `error` is filled in only for an immediate failure.
    */
    bool open(std::string_view host, std::uint16_t port, std::string &error);

    /*
      Hands every queued complete line to the responder. A partial line stays
      buffered for the next call. False when the peer cannot continue - the
      socket closed or errored, a line exceeded the cap, or a write failed -
      with `error` naming it; `error` is cleared otherwise.
    */
    bool pump(Responder &responder, std::string &error);

    /* Appends '\n' and sends every byte. False on a write error, such as EPIPE. */
    bool writeLine(std::string_view line) override;

    /* Sends exactly `size` bytes with nothing appended. False on a write error. */
    bool writeBytes(const uint8_t *data, size_t size) override;

    /* Closes and releases the socket. Idempotent. */
    void stop();

    /* True once the socket closed or errored, framing broke, or a write failed. */
    bool failed() const;

    /* The first failure, which is the one worth reporting; empty until then. */
    std::string lastError() const;

private:
    static bool onMessage(int eventType,
                          const EmscriptenWebSocketMessageEvent *event,
                          void *userData);
    static bool onClose(int eventType,
                        const EmscriptenWebSocketCloseEvent *event,
                        void *userData);
    static bool onError(int eventType,
                        const EmscriptenWebSocketErrorEvent *event,
                        void *userData);

    /* Copies the frame it was handed; the browser owns that memory only until
       the callback returns. */
    void appendBytes(const uint8_t *data, size_t size, bool isText);

    /* Records the first failure and returns false, so callers can `return fail()`. */
    bool fail(std::string message);
    /* A failed send: the same failure, plus the EPIPE responders report. */
    bool failWrite(std::string message);
    /* An over-cap line: the framing is gone, so the buffer goes too. */
    bool failLineLimit(std::string &error);
    void releaseSocket();
    std::string lineLimitError() const;

    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    bool failed_ = false;
    std::string lastError_;
    /* Bytes received but not yet returned as lines. */
    std::string incoming_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_WEBSOCKET_PEER_HPP */
