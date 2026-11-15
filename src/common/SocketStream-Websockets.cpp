/*
  The browser arm of SocketStream: the same class and the same StreamBase, with
  the dial done by a WebSocket. This unit is the loop's side of it — the dial,
  the writes, close and pump.

  A page has no fd and nothing to block on. `emscripten_websocket_new` starts the
  connection and the open, message, close and error events arrive later as
  callbacks on the page's own thread (SocketStream-WSEvents.cpp). The callbacks
  never answer anything themselves: they copy the bytes into a queue and record
  the close or error, and pump() feeds the queue through StreamBase exactly as
  the native arm feeds what recv() returned. That keeps the protocol layer on one
  path and makes an event that arrives between two frames harmless.

  `ws://` is added here, never by configuration: connect_host is always
  HOST:PORT, on both platforms.
*/

#include "SocketStream.h"

#include <emscripten/websocket.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "Configuration.h"
#include "SocketStream-Websockets.h"

namespace sdl3mcp {
namespace {

std::string withResult(std::string_view action, int result) {
   return std::string(action) + " (result " + std::to_string(result) + ")";
}

}  // namespace

SocketStream::SocketStream() : native_(std::make_unique<Native>()) {
}

SocketStream::~SocketStream() {
   close();
}

Result<void, TransportError> SocketStream::connect(std::string_view host,
                                                   std::uint16_t port,
                                                   std::uint32_t timeoutMs) {
   close();

   if (host.empty()) {
      return TransportError{"cannot connect to an empty host"};
   }
   if (port == 0) {
      return TransportError{"cannot connect to port 0"};
   }

   /*
     The dial is asynchronous, so this budget cannot bound it: a connection that
     never opens reports through the close or error callback, which the runtime
     turns into a quit. The parameter stays in the shared signature because the
     native arm does bound its dial with it.
   */
   (void)timeoutMs;

   if (!emscripten_websocket_is_supported()) {
      return TransportError{"this browser does not support WebSockets"};
   }

   std::string url = "ws://" + endpointText(host, port);

   EmscriptenWebSocketCreateAttributes attributes;
   emscripten_websocket_init_create_attributes(&attributes);
   /* The URL is read during the call itself; the std::string outlives it. */
   attributes.url = url.c_str();
   /*
     Emscripten ignores this field: its WebSocket library creates every
     connection on the main thread and marks createOnMainThread a TODO, so the
     setting is documentation rather than a choice. The callbacks and pump()
     still share that one thread - under Node there is only one thread at all -
     which is why the queue needs no lock.
   */
   attributes.createOnMainThread = false;

   const EMSCRIPTEN_WEBSOCKET_T socket = emscripten_websocket_new(&attributes);
   if (socket <= 0) {
      return TransportError{withResult(
          "the WebSocket for '" + url + "' could not be created", socket)};
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
      return TransportError{withResult(
          "the WebSocket callbacks could not be registered",
          static_cast<int>(registration))};
   }

   return {};
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

int SocketStream::outputFd() const noexcept {
   /* The browser pushes its writes; there is no descriptor to poll. */
   return -1;
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

}  // namespace sdl3mcp
