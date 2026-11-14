/*
  The Handler's client-facing stdio stream.

  stdin is the client's request channel and stdout the answer channel; stderr
  stays free for diagnostics. The stream is non-blocking on the write side and
  buffers what stdout will not take, so a client that stops reading cannot wedge
  the single-threaded loop. Reads happen only when poll() has already said stdin
  is ready, so stdin may stay blocking.

  The fds belong to the process, not the stream: close() stops the stream but
  never closes stdin or stdout, and the destructor restores the stdout flags it
  changed.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "Result.h"
#include "Stream.h"

namespace sdl3mcp {

class StdioStream : public StreamBase {
  public:
   StdioStream();
   ~StdioStream() override;

   StdioStream(const StdioStream&) = delete;
   StdioStream& operator=(const StdioStream&) = delete;

   /* Makes stdout non-blocking and reports onOpen. Idempotent. */
   Result<void, TransportError> start();

   bool sendText(std::string_view text) override;
   bool sendBinary(const std::uint8_t* data, std::size_t size) override;
   void close() override;
   void pump() override;

   /* The descriptor a poll loop watches for requests. */
   int inputFd() const noexcept { return inputFd_; }

  private:
   bool flush();
   bool append(const char* data, std::size_t size);

   int inputFd_;
   int outputFd_;
   int savedOutputFlags_ = -1;
   std::string outbound_;
   bool started_ = false;
   bool closed_ = false;
};

}  // namespace sdl3mcp
