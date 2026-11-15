#pragma once

/*
  The native arm's own header, shared by its two units: the dial and close
  (SocketStream.cpp), the writes, reads and pump (SocketStream-IO.cpp).

  SocketStream declares `struct Native;` and this header defines it. One
  definition in one place, because a nested type cannot be defined by both
  translation units; the type stays out of SocketStream.h's interface.
*/

#include <string>

#include "SocketStream.h"

namespace sdl3mcp {

struct SocketStream::Native {
   int fd = -1;
   std::string endpoint;
   std::string outbound;

   /* Writes as much of the outbound buffer as the socket accepts. */
   bool flush(SocketStream& stream);
   void reportWriteFailure(SocketStream& stream, int error);
};

}  // namespace sdl3mcp
