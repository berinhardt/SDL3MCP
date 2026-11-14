/*
  The browser arm of SocketStream: the same class and the same StreamBase, with
  the dial done by a WebSocket.

  A page has no fd and nothing to block on. `emscripten_websocket_new` starts the
  connection and the open, message, close and error events arrive later as
  callbacks on the page's own thread. The callbacks never answer anything
  themselves: they copy the bytes into a queue and record the close or error,
  and pump() feeds the queue through StreamBase exactly as the native arm feeds
  what recv() returned. That keeps the protocol layer on one path and makes an
  event that arrives between two frames harmless.

  `ws://` is added here, never by configuration: connect_host is always
  HOST:PORT, on both platforms.
*/

#include <emscripten/websocket.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "SocketStream.h"

namespace sdl3mcp {
namespace {

constexpr unsigned short kNormalCloseCode = 1000;
constexpr unsigned short kGoingAwayCloseCode = 1001;

/* Prints IPv6 literals in brackets so the URL reads as an endpoint. */
std::string endpointText(std::string_view host, std::uint16_t port) {
   std::string text;
   if (host.find(':') != std::string_view::npos) {
      text = "[" + std::string(host) + "]";
   } else {
      text = std::string(host);
   }
   text += ":" + std::to_string(port);
   return text;
}

std::string withResult(std::string_view action, int result) {
   return std::string(action) + " (result " + std::to_string(result) + ")";
}

}  // namespace

struct SocketStream::Native {
   EMSCRIPTEN_WEBSOCKET_T socket = 0;
   std::string endpoint;

   /*
     Bytes received but not yet handed to StreamBase. One WebSocket message may
     hold a whole line, a fragment of one, or the raw payload that follows a
     line, so the queue is a byte stream, not a message queue.
   */
   std::string inbound;

   /*
     The connection ended between two pumps. The close is delivered after the
     bytes queued before it, so the last requests a client sent are answered
     before the session retires.
   */
   bool closePending = false;
   unsigned short closeCode = 0;
   std::string closeReason;
   bool errorPending = false;
   /* The specific reason a queued error reports; empty means the generic one. */
   std::string errorMessage;
};

SocketStream::SocketStream() : native_(std::make_unique<Native>()) {
}

SocketStream::~SocketStream() {
   close();
}

bool SocketStream::connect(std::string_view host,
                           std::uint16_t port,
                           std::uint32_t timeoutMs,
                           std::string& error) {
   error.clear();
   close();

   if (host.empty()) {
      error = "cannot connect to an empty host";
      return false;
   }
   if (port == 0) {
      error = "cannot connect to port 0";
      return false;
   }

   /*
     The dial is asynchronous, so this budget cannot bound it: a connection that
     never opens reports through the close or error callback, which the runtime
     turns into a quit. The parameter stays in the shared signature because the
     native arm does bound its dial with it.
   */
   (void)timeoutMs;

   if (!emscripten_websocket_is_supported()) {
      error = "this browser does not support WebSockets";
      return false;
   }

   std::string url = "ws://" + endpointText(host, port);

   EmscriptenWebSocketCreateAttributes attributes;
   emscripten_websocket_init_create_attributes(&attributes);
   /* The URL is read during the call itself; the std::string outlives it. */
   attributes.url = url.c_str();
   /*
     The creating thread is the page's main thread, so the socket stays on it:
     the callbacks and pump() share that thread and the queue needs no lock.
   */
   attributes.createOnMainThread = false;

   const EMSCRIPTEN_WEBSOCKET_T socket = emscripten_websocket_new(&attributes);
   if (socket <= 0) {
      error = withResult("the WebSocket for '" + url + "' could not be created",
                         socket);
      return false;
   }

   native_->socket = socket;
   native_->endpoint = url;
   native_->inbound.clear();
   native_->closePending = false;
   native_->closeCode = 0;
   native_->closeReason.clear();
   native_->errorPending = false;
   native_->errorMessage.clear();

   EMSCRIPTEN_RESULT registration = emscripten_websocket_set_onopen_callback(
       socket, this, &SocketStream::onOpen);
   if (registration == EMSCRIPTEN_RESULT_SUCCESS) {
      registration = emscripten_websocket_set_onmessage_callback(
          socket, this, &SocketStream::onMessage);
   }
   if (registration == EMSCRIPTEN_RESULT_SUCCESS) {
      registration = emscripten_websocket_set_onclose_callback(
          socket, this, &SocketStream::onClose);
   }
   if (registration == EMSCRIPTEN_RESULT_SUCCESS) {
      registration = emscripten_websocket_set_onerror_callback(
          socket, this, &SocketStream::onError);
   }
   if (registration != EMSCRIPTEN_RESULT_SUCCESS) {
      /* No callback has run yet, so releasing the socket here is safe. */
      close();
      error = withResult("the WebSocket callbacks could not be registered",
                         static_cast<int>(registration));
      return false;
   }

   return true;
}

bool SocketStream::sendText(std::string_view text) {
   std::string line(text);
   line.push_back('\n');
   return sendBinary(reinterpret_cast<const std::uint8_t*>(line.data()),
                     line.size());
}

bool SocketStream::sendBinary(const std::uint8_t* data, std::size_t size) {
   if (size == 0) {
      return true;
   }
   if (data == nullptr) {
      return false;
   }
   if (native_->socket == 0 || finished()) {
      return false;
   }

   /* The browser copies the bytes into its own buffer before the call returns. */
   const EMSCRIPTEN_RESULT result = emscripten_websocket_send_binary(
       native_->socket, const_cast<std::uint8_t*>(data),
       static_cast<std::uint32_t>(size));
   if (result != EMSCRIPTEN_RESULT_SUCCESS) {
      fail(withResult("the WebSocket send to " + native_->endpoint + " failed",
                      static_cast<int>(result)));
      return false;
   }
   return true;
}

void SocketStream::close() {
   if (!native_ || native_->socket == 0) {
      return;
   }
   /*
     close() lets the browser flush what it already queued; delete() detaches
     the callbacks first, so nothing reaches this stream after close().
   */
   emscripten_websocket_close(native_->socket, kNormalCloseCode, "");
   emscripten_websocket_delete(native_->socket);
   native_->socket = 0;
   native_->inbound.clear();
   native_->closePending = false;
   native_->errorPending = false;
   native_->errorMessage.clear();
}

void SocketStream::pump() {
   if (!native_ || finished()) {
      return;
   }

   if (!native_->inbound.empty()) {
      std::string inbound = std::move(native_->inbound);
      native_->inbound.clear();
      feed(inbound);
      /* A handler may close the stream while answering a line. */
      if (finished()) {
         return;
      }
   }

   if (native_->errorPending) {
      native_->errorPending = false;
      fail(native_->errorMessage.empty() ? "the WebSocket reported an error"
                                         : native_->errorMessage);
      return;
   }

   if (native_->closePending) {
      native_->closePending = false;
      if (native_->closeCode == 0 ||
          native_->closeCode == kNormalCloseCode ||
          native_->closeCode == kGoingAwayCloseCode) {
         finish();
         return;
      }
      std::string message = "the WebSocket closed with code " +
                            std::to_string(native_->closeCode);
      if (!native_->closeReason.empty()) {
         message += ": " + native_->closeReason;
      }
      fail(message);
   }
}

bool SocketStream::onOpen(int,
                          const EmscriptenWebSocketOpenEvent*,
                          void* userData) {
   auto* stream = static_cast<SocketStream*>(userData);
   if (stream != nullptr) {
      stream->open();
   }
   /* The browser ignores the return value; the stream's state carries the verdict. */
   return true;
}

bool SocketStream::onMessage(int,
                             const EmscriptenWebSocketMessageEvent* event,
                             void* userData) {
   auto* stream = static_cast<SocketStream*>(userData);
   if (stream != nullptr && event != nullptr) {
      stream->appendBytes(event->data, event->numBytes, event->isText != 0);
   }
   return true;
}

bool SocketStream::onClose(int,
                           const EmscriptenWebSocketCloseEvent* event,
                           void* userData) {
   auto* stream = static_cast<SocketStream*>(userData);
   if (stream == nullptr || stream->finished()) {
      return true;
   }

   stream->native_->closePending = true;
   if (event != nullptr) {
      stream->native_->closeCode = event->code;
      /*
        Emscripten truncates the reason into a fixed char[512] without promising
        a terminator, so the read is bounded by the field, not by a search.
      */
      std::size_t reasonLength = 0;
      while (reasonLength < sizeof event->reason &&
             event->reason[reasonLength] != '\0') {
         ++reasonLength;
      }
      stream->native_->closeReason.assign(event->reason, reasonLength);
   }
   return true;
}

bool SocketStream::onError(int,
                           const EmscriptenWebSocketErrorEvent*,
                           void* userData) {
   auto* stream = static_cast<SocketStream*>(userData);
   if (stream != nullptr && !stream->finished()) {
      stream->native_->errorPending = true;
   }
   return true;
}

void SocketStream::appendBytes(const std::uint8_t* data,
                               std::size_t size,
                               bool isText) {
   if (native_ == nullptr || finished() || size == 0 || data == nullptr) {
      return;
   }
   /*
     Emscripten's text path counts the terminating NUL in numBytes, and a text
     frame still carries the same UTF-8 line stream: drop the NUL, not the data.
   */
   if (isText && data[size - 1] == '\0') {
      --size;
   }
   native_->inbound.append(reinterpret_cast<const char*>(data), size);

   /*
     pump() runs once per frame, so a peer that never sends a newline could pile
     frames up between two calls: fail as soon as the pending fragment is over
     the cap instead of buffering without a bound.
   */
   const std::size_t lastNewline = native_->inbound.rfind('\n');
   const std::size_t pending = lastNewline == std::string::npos
                                   ? native_->inbound.size()
                                   : native_->inbound.size() - lastNewline - 1;
   if (pending > StreamBase::kMaxLineBytes) {
      native_->inbound.clear();
      native_->errorMessage = "input line exceeds the " +
                              std::to_string(StreamBase::kMaxLineBytes) +
                              " byte limit";
      native_->errorPending = true;
   }
}

}  // namespace sdl3mcp
