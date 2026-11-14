/*
  The Emscripten SocketStream, driven from Node.js against a local WebSocket
  server.

  Emscripten has no second thread and no blocking read, so the transport is the
  callbacks: open, messages, close and error arrive on the page's thread and
  pump() hands the queued bytes to StreamBase. The suite proves that path end to
  end - the dial adds ws://, a line split across two WebSocket messages is one
  line, CRLF is framing, the raw payload that follows a line is exactly its
  byteLength, a write reaches the peer, and a server close reaches onClose.

  The server is `run_node_websocket_test.js`, which starts it on an ephemeral
  port and passes the port in SDL3MCP_TEST_WS_PORT. `emscripten_sleep` is what
  lets the browser's event loop run between pumps, so this executable links
  with ASYNCIFY; the library itself needs none of that.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "SocketStream.h"

namespace {

/* The port the runner's server listens on, or 0 when it was not provided. */
int serverPort() {
   return EM_ASM_INT(
       { return parseInt(process.env['SDL3MCP_TEST_WS_PORT'] || '0', 10); });
}

bool contains(const std::vector<std::string>& lines, std::string_view line) {
   return std::find(lines.begin(), lines.end(), line) != lines.end();
}

/*
  Records what StreamBase delivered. The payload line is recognized here and
  expectPayload() is called from onMessage, exactly as the Handler does when it
  reads a raw frame result.
*/
class RecordingHandler : public sdl3mcp::StreamHandler {
  public:
   void onOpen() override { opened = true; }

   void onMessage(std::string_view line) override {
      lines.emplace_back(line);
      if (line.find("\"byteLength\":4") != std::string_view::npos) {
         stream->expectPayload(4);
      }
   }

   void onPayload(std::vector<std::uint8_t> bytes) override {
      payload = std::move(bytes);
   }

   void onError(std::string_view error) override {
      errors.emplace_back(error);
   }

   void onClose() override { closed = true; }

   sdl3mcp::SocketStream* stream = nullptr;
   bool opened = false;
   bool closed = false;
   std::vector<std::string> lines;
   std::vector<std::uint8_t> payload;
   std::vector<std::string> errors;
};

/* Pumps until `done` holds, letting the browser event loop run between pumps. */
bool pumpUntil(sdl3mcp::SocketStream& stream,
               const std::function<bool()>& done,
               int timeoutMs = 5000) {
   constexpr int kStepMs = 5;
   for (int elapsed = 0; elapsed < timeoutMs; elapsed += kStepMs) {
      stream.pump();
      if (done()) {
         return true;
      }
      emscripten_sleep(kStepMs);
   }
   stream.pump();
   return done();
}

}  // namespace

TEST_CASE("the browser dial frames lines, payloads and a close") {
   const int port = serverPort();
   REQUIRE(port > 0);

   RecordingHandler handler;
   sdl3mcp::SocketStream stream;
   stream.setHandler(&handler);
   handler.stream = &stream;

   std::string error;
   REQUIRE(stream.connect("127.0.0.1", static_cast<std::uint16_t>(port), 1000,
                          error));
   CHECK(error.empty());

   CHECK(pumpUntil(stream, [&handler] { return handler.opened; }));
   CHECK(handler.errors.empty());

   /* The greeting, the line split across two messages, and the CRLF line. */
   CHECK(pumpUntil(stream, [&handler] {
      return contains(handler.lines, "hello") &&
             contains(handler.lines, "partial") &&
             contains(handler.lines, "crlf");
   }));
   CHECK(handler.errors.empty());

   /* A write reaches the server, which answers with a line and a raw payload. */
   REQUIRE(stream.sendText("payload"));
   CHECK(pumpUntil(stream, [&handler] { return handler.payload.size() == 4; }));
   CHECK(handler.payload == std::vector<std::uint8_t>{1, 2, 3, 4});
   CHECK(contains(handler.lines,
                  "{\"format\":\"rgba8888\",\"width\":1,\"height\":1,"
                  "\"byteLength\":4,\"frame\":7}"));

   /* And a text write is answered too, which proves both directions. */
   REQUIRE(stream.sendText("ping"));
   CHECK(pumpUntil(
       stream, [&handler] { return contains(handler.lines, "{\"echo\":\"ping\"}"); }));

   /* The server closes: the close reaches onClose and no error is invented. */
   REQUIRE(stream.sendText("close"));
   CHECK(pumpUntil(stream, [&handler] { return handler.closed; }));
   CHECK(handler.errors.empty());

   stream.close();
   CHECK(!stream.sendText("after close"));
}

TEST_CASE("a dial without a host or port fails before it touches the browser") {
   sdl3mcp::SocketStream stream;
   std::string error;

   CHECK(!stream.connect("", 7777, 1000, error));
   CHECK(error.find("empty host") != std::string::npos);

   CHECK(!stream.connect("127.0.0.1", 0, 1000, error));
   CHECK(error.find("port 0") != std::string::npos);

   CHECK(!stream.sendText("unconnected"));
   CHECK(!stream.sendBinary(nullptr, 4));
   stream.pump();
   stream.close();
}
