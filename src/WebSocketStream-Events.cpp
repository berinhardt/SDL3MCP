/*
  The browser arm's inbound half: the four WebSocket callbacks and the byte
  queue they fill.

  A callback is the page's code path, not the loop's. It copies the bytes it was
  handed (the browser owns that buffer only until the callback returns) and
  records the close or error; pump(), in WebSocketStream.cpp, delivers both, in
  the order they arrived. Nothing here answers a line, so the protocol layer
  keeps the one path StreamBase gives it.

  The type and its header are the dial's: this unit declares nothing of its own.
*/

#include "SocketStream.h"

#include <emscripten/websocket.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "WebSocketStream-Native.h"

namespace sdl3mcp {

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
