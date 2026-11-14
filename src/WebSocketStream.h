#pragma once

/*
  The browser arm's own header, shared by its two units: the dial, the writes
  and pump() read the socket and drain the queue (WebSocketStream.cpp), the
  callbacks fill the queue and record the close or error
  (WebSocketStream-Events.cpp).

  SocketStream declares `struct Native;` and this header defines it. One
  definition in one place, because a nested type cannot be defined by both
  translation units; the type stays out of SocketStream.h's interface. The
  close codes live here too, beside the state that records them.
*/

#include <emscripten/websocket.h>

#include <string>

#include "SocketStream.h"

namespace sdl3mcp {

/* Normal closure, and the peer going away; both end a session cleanly. */
constexpr unsigned short kNormalCloseCode = 1000;
constexpr unsigned short kGoingAwayCloseCode = 1001;

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

}  // namespace sdl3mcp
