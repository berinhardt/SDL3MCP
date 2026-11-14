/*
  The transport seam and the framing both native arms share.

  A Stream is data only: four callbacks, two write shapes, close, and a pump.
  It names no socket, file descriptor, browser API, or SDL type, so a session
  answers the same way over a dialed socket, a pipe, or a WebSocket. A concrete
  transport owns the connection and calls StreamBase with the bytes it reads;
  StreamBase owns the line buffer, the 1 MiB inbound cap, CRLF stripping, and
  the raw-payload state that follows a line announcing a frame.

  The payload callback is a continuation of the line before it, never a message
  of its own: a peer sends one result line and then exactly `byteLength` bytes
  with no newline. The handler asks for those bytes from onMessage by calling
  expectPayload(), and StreamBase routes whatever arrives next into the payload
  buffer, including bytes already in the same read.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sdl3mcp {

class StreamHandler {
  public:
   virtual ~StreamHandler() = default;

   virtual void onOpen() = 0;
   /* One complete line, with the newline and one trailing CR removed. */
   virtual void onMessage(std::string_view line) = 0;
   /* The raw bytes a preceding line announced through expectPayload(). */
   virtual void onPayload(std::vector<uint8_t> payload) = 0;
   virtual void onError(std::string_view error) = 0;
   virtual void onClose() = 0;
};

class Stream {
  public:
   virtual ~Stream() = default;

   /* One JSON line; the transport appends its own terminator. */
   virtual bool sendText(std::string_view text) = 0;
   /* Exactly `size` bytes with nothing appended. */
   virtual bool sendBinary(const uint8_t* data, size_t size) = 0;
   /*
      The next `size` bytes belong to the line just delivered, not to a new
      line. A peer's raw frame result is one header line followed by exactly
      byteLength bytes; the handler asks for them from onMessage.
   */
   virtual void expectPayload(size_t size) = 0;
   /*
      Consumes and drops the next `size` bytes without buffering them. A rejected
      raw frame still has its payload on the wire, so skipping it keeps the next
      line a line.
   */
   virtual void discardPayload(size_t size) = 0;
   virtual void close() = 0;
   /* Reads and delivers whatever the transport has available; a no-op where
      the platform pushes messages itself. */
   virtual void pump() = 0;
   /*
      True while the transport holds bytes it could not write yet. A poll loop
      adds the stream to its POLLOUT set only then, so a writable socket does
      not spin the loop.
   */
   virtual bool wantsWrite() const { return false; }

   void setHandler(StreamHandler* handler) noexcept { handler_ = handler; }
   StreamHandler* handler() const noexcept { return handler_; }

  protected:
   StreamHandler* handler_ = nullptr;
};

class StreamBase : public Stream {
  public:
   /* An inbound line longer than this is a transport error, not a message. */
   static constexpr size_t kMaxLineBytes = 1024 * 1024;

   /* The concrete transport reports the connection it established. */
   void open();

   /* Appends bytes read from the peer and delivers every complete line. */
   void feed(std::string_view text);
   void feed(const uint8_t* data, size_t size);

   /* The peer ended the connection cleanly. */
   void finish();
   /* The transport failed; reported once, then the stream is closed. */
   void fail(std::string_view error);

   /*
     The next `size` bytes belong to the line just delivered, not to a new
     line. Called from onMessage when that line announced a raw payload.
   */
   void expectPayload(size_t size) override;
   /*
      Consumes `size` bytes and drops them without allocating or calling
      onPayload. A rejected raw frame still has a payload on the wire; skipping
      it keeps the stream aligned so the session stays attached.
   */
   void discardPayload(size_t size) override;
   bool awaitingPayload() const noexcept {
      return payloadExpected_ > payloadReceived_;
   }

   bool wantsWrite() const override { return wantsWrite_; }

  protected:
   bool finished() const noexcept { return finished_; }
   void setWantsWrite(bool value) noexcept { wantsWrite_ = value; }

  private:
   void failLineTooLong();

   std::string line_;
   std::vector<uint8_t> payload_;
   size_t payloadExpected_ = 0;
   size_t payloadReceived_ = 0;
   bool discardingPayload_ = false;
   bool finished_ = false;
   bool wantsWrite_ = false;
};

}  // namespace sdl3mcp
