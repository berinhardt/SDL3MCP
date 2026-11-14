/*
  The 1.0 protocol core: JSON-RPC framing, the line/payload stream, tool
  results, and the session state machine. No SDL, no window, no transport: a
  fake stream records what the session writes and feeds what a client would
  send, so every protocol decision is exercised as ordinary code.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <SDL3MCP_version.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "JsonRpc.h"
#include "Session.h"
#include "Stream.h"
#include "Tool.h"
#include "ToolCatalog.h"
#include "ToolResult.h"

namespace {

using sdl3mcp::RpcErrorCode;
using sdl3mcp::RpcMessage;

nlohmann::json errorCodeOf(const nlohmann::json& response) {
   return response["error"]["code"];
}

/* A successful parse never leaves a stale error behind. */
void expectParsed(std::string_view line, RpcMessage& message) {
   nlohmann::json errorResponse =
       sdl3mcp::makeError(nullptr, RpcErrorCode::InternalError, "stale");
   CHECK(sdl3mcp::parseMessage(line, message, errorResponse));
   CHECK(errorResponse.is_null());
}

void expectParseError(std::string_view line,
                      RpcErrorCode code,
                      const nlohmann::json& id) {
   RpcMessage message;
   nlohmann::json errorResponse;
   CHECK(!sdl3mcp::parseMessage(line, message, errorResponse));
   CHECK(errorResponse.is_object());
   CHECK(errorResponse["jsonrpc"] == "2.0");
   CHECK(errorResponse.contains("error"));
   CHECK(!errorResponse.contains("result"));
   CHECK(errorCodeOf(errorResponse).get<int>() == static_cast<int>(code));
   CHECK(errorResponse["error"]["message"].is_string());
   CHECK(errorResponse["id"] == id);
}

/*
  Records every callback the stream makes. The line and payload vectors keep
  their arrival order, which is what makes the raw-frame test meaningful.
*/
class RecordingHandler : public sdl3mcp::StreamHandler {
  public:
   void onOpen() override { opens++; }

   void onMessage(std::string_view line) override {
      lines.emplace_back(line);
   }

   void onPayload(std::vector<uint8_t> payload) override {
      payloads.push_back(std::move(payload));
   }

   void onError(std::string_view error) override {
      errors.emplace_back(error);
   }

   void onClose() override { closes++; }

   int opens = 0;
   int closes = 0;
   std::vector<std::string> lines;
   std::vector<std::vector<uint8_t>> payloads;
   std::vector<std::string> errors;
};

class TestStream : public sdl3mcp::StreamBase {
  public:
   bool sendText(std::string_view text) override {
      if (writeFails) {
         return false;
      }
      sent.emplace_back(text);
      return true;
   }

   bool sendBinary(const uint8_t* data, std::size_t size) override {
      if (writeFails) {
         return false;
      }
      binary.insert(binary.end(), data, data + size);
      return true;
   }

   void close() override { closeCount++; }
   void pump() override { pumpCount++; }

   std::vector<std::string> sent;
   std::vector<uint8_t> binary;
   int closeCount = 0;
   int pumpCount = 0;
   bool writeFails = false;
};

class RecordingCompletion : public sdl3mcp::ToolCompletionDelegate {
  public:
   void complete(sdl3mcp::ToolResult result) override {
      completed.push_back(std::move(result));
   }

   void cancelled() override { cancellations++; }

   std::vector<sdl3mcp::ToolResult> completed;
   int cancellations = 0;
};

sdl3mcp::Tool makeTool(
    std::string name,
    std::string description,
    std::function<void(sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate&)>
        invoke) {
   return sdl3mcp::Tool{
       std::move(name),
       std::move(description),
       nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
       nlohmann::json{{"readOnlyHint", true}},
       std::move(invoke),
   };
}

/*
  A session over a fake stream. The catalog is a member so a test can add tools
  after construction; the session holds the stream and the catalog by reference.
*/
class SessionFixture {
  public:
   SessionFixture() {
      auto owned = std::make_unique<TestStream>();
      stream = owned.get();
      session = std::make_unique<sdl3mcp::Session>(catalog, std::move(owned));
      stream->open();
   }

   void feed(std::string_view line) {
      if (!line.empty() && line.back() == '\n') {
         stream->feed(line);
         return;
      }
      std::string framed(line);
      framed.push_back('\n');
      stream->feed(framed);
   }

   nlohmann::json answer(std::size_t index = 0) const {
      return nlohmann::json::parse(stream->sent.at(index));
   }

   void initialize() {
      feed(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
      REQUIRE(stream->sent.size() == 1);
   }

   sdl3mcp::ToolCatalog catalog;
   TestStream* stream = nullptr;
   std::unique_ptr<sdl3mcp::Session> session;
};

std::string callLine(const nlohmann::json& id, std::string_view name) {
   return nlohmann::json{
       {"jsonrpc", "2.0"},
       {"id", id},
       {"method", "tools/call"},
       {"params", nlohmann::json{{"name", name}}},
   }
       .dump();
}

std::string callLine(const nlohmann::json& id,
                     std::string_view name,
                     nlohmann::json arguments) {
   return nlohmann::json{
       {"jsonrpc", "2.0"},
       {"id", id},
       {"method", "tools/call"},
       {"params",
        nlohmann::json{{"name", name}, {"arguments", std::move(arguments)}}},
   }
       .dump();
}

}  // namespace

TEST_CASE("json-rpc error codes keep their wire values") {
   CHECK(static_cast<int>(RpcErrorCode::ParseError) == -32700);
   CHECK(static_cast<int>(RpcErrorCode::InvalidRequest) == -32600);
   CHECK(static_cast<int>(RpcErrorCode::MethodNotFound) == -32601);
   CHECK(static_cast<int>(RpcErrorCode::InvalidParams) == -32602);
   CHECK(static_cast<int>(RpcErrorCode::InternalError) == -32603);
}

TEST_CASE("json-rpc parses requests and notifications") {
   {
      RpcMessage message;
      expectParsed(
          R"({"jsonrpc":"2.0","id":"abc","method":"ping","params":{"verbose":true}})",
          message);
      CHECK(message.kind == RpcMessage::Kind::Request);
      CHECK(message.request.id.is_string());
      CHECK(message.request.id.get<std::string>() == "abc");
      CHECK(message.request.method == "ping");
      CHECK(message.request.params["verbose"] == true);
   }
   {
      RpcMessage message;
      expectParsed(R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})", message);
      CHECK(message.kind == RpcMessage::Kind::Request);
      CHECK(message.request.id == 7);
      CHECK(message.request.method == "tools/list");
      /* Absent params becomes an empty object, never null. */
      CHECK(message.request.params.is_object());
      CHECK(message.request.params.empty());
   }
   {
      /* The id keeps its JSON type, including values beyond 32 bits. */
      RpcMessage message;
      expectParsed(
          R"({"jsonrpc":"2.0","id":9007199254740993,"method":"ping"})", message);
      CHECK(message.request.id.get<std::int64_t>() == 9007199254740993LL);
   }
   {
      RpcMessage message;
      expectParsed(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                   message);
      CHECK(message.kind == RpcMessage::Kind::Notification);
      CHECK(message.notification.method == "notifications/initialized");
      CHECK(message.notification.params.is_object());
      CHECK(message.notification.params.empty());
   }
}

TEST_CASE("json-rpc parse and request errors") {
   expectParseError("{", RpcErrorCode::ParseError, nullptr);
   expectParseError("not json", RpcErrorCode::ParseError, nullptr);
   expectParseError("", RpcErrorCode::ParseError, nullptr);
   expectParseError("   ", RpcErrorCode::ParseError, nullptr);

   /* MCP does not allow batches. */
   expectParseError("[]", RpcErrorCode::InvalidRequest, nullptr);
   expectParseError(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])",
                    RpcErrorCode::InvalidRequest, nullptr);
   expectParseError("42", RpcErrorCode::InvalidRequest, nullptr);
   expectParseError("null", RpcErrorCode::InvalidRequest, nullptr);

   /* A valid id is echoed even when another field is wrong. */
   expectParseError(R"({"id":1,"method":"ping"})", RpcErrorCode::InvalidRequest, 1);
   expectParseError(R"({"jsonrpc":"1.0","id":"a","method":"ping"})",
                    RpcErrorCode::InvalidRequest, "a");
   expectParseError(R"({"jsonrpc":"2.0","id":2})", RpcErrorCode::InvalidRequest, 2);
   expectParseError(R"({"jsonrpc":"2.0","id":2,"method":5})",
                    RpcErrorCode::InvalidRequest, 2);
   expectParseError(R"({"jsonrpc":"2.0","id":2,"method":"ping","params":[]})",
                    RpcErrorCode::InvalidRequest, 2);
   expectParseError(R"({"jsonrpc":"2.0","id":2,"method":"ping","params":null})",
                    RpcErrorCode::InvalidRequest, 2);

   /* An id that cannot be echoed makes the error id null. */
   expectParseError(R"({"jsonrpc":"2.0","id":null,"method":"ping"})",
                    RpcErrorCode::InvalidRequest, nullptr);
   expectParseError(R"({"jsonrpc":"2.0","id":false,"method":"ping"})",
                    RpcErrorCode::InvalidRequest, nullptr);
   expectParseError(R"({"jsonrpc":"2.0","id":[1],"method":"ping"})",
                    RpcErrorCode::InvalidRequest, nullptr);
   expectParseError(R"({"jsonrpc":"2.0","id":{"n":1},"method":"ping"})",
                    RpcErrorCode::InvalidRequest, nullptr);
}

TEST_CASE("json-rpc serializes results and errors") {
   {
      const nlohmann::json response =
          sdl3mcp::makeResult(42, nlohmann::json{{"ok", true}});
      CHECK(response["jsonrpc"] == "2.0");
      CHECK(response["id"] == 42);
      CHECK(response["result"]["ok"] == true);
      CHECK(!response.contains("error"));
   }
   {
      const nlohmann::json response =
          sdl3mcp::makeError("abc", RpcErrorCode::MethodNotFound, "no such method");
      CHECK(response["jsonrpc"] == "2.0");
      CHECK(response["id"] == "abc");
      CHECK(!response.contains("result"));
      CHECK(errorCodeOf(response).get<int>() == -32601);
      CHECK(response["error"]["message"] == "no such method");
   }
}

TEST_CASE("stream delivers complete lines and strips one CR") {
   RecordingHandler handler;
   TestStream stream;
   stream.setHandler(&handler);
   stream.open();
   CHECK(handler.opens == 1);

   stream.feed("one\ntwo\r\nthree");
   REQUIRE(handler.lines.size() == 2);
   CHECK(handler.lines[0] == "one");
   CHECK(handler.lines[1] == "two");

   stream.feed("\n");
   REQUIRE(handler.lines.size() == 3);
   CHECK(handler.lines[2] == "three");
}

TEST_CASE("stream delivers a blank line as empty input") {
   RecordingHandler handler;
   TestStream stream;
   stream.setHandler(&handler);

   stream.feed("\n");
   REQUIRE(handler.lines.size() == 1);
   CHECK(handler.lines[0].empty());
}

TEST_CASE("stream caps an inbound line at one mebibyte") {
   RecordingHandler handler;
   TestStream stream;
   stream.setHandler(&handler);

   const std::string longest(sdl3mcp::StreamBase::kMaxLineBytes, 'x');
   stream.feed(longest);
   stream.feed("\n");
   REQUIRE(handler.lines.size() == 1);
   CHECK(handler.lines[0].size() == longest.size());
   CHECK(handler.errors.empty());

   RecordingHandler tooLongHandler;
   TestStream tooLongStream;
   tooLongStream.setHandler(&tooLongHandler);
   const std::string overlong(sdl3mcp::StreamBase::kMaxLineBytes + 1, 'x');
   tooLongStream.feed(overlong);
   CHECK(tooLongHandler.lines.empty());
   REQUIRE(tooLongHandler.errors.size() == 1);
   CHECK(tooLongHandler.errors[0].find("1048576") != std::string::npos);
   CHECK(tooLongHandler.closes == 1);
}

TEST_CASE("stream reads the raw payload a line announces") {
   class PayloadHandler : public sdl3mcp::StreamHandler {
     public:
      explicit PayloadHandler(sdl3mcp::StreamBase& stream) : stream_(stream) {}

      void onOpen() override {}
      void onMessage(std::string_view line) override {
         lines.emplace_back(line);
         if (line == "frame") {
            stream_.expectPayload(4);
         }
      }
      void onPayload(std::vector<uint8_t> payload) override {
         payloads.push_back(std::move(payload));
      }
      void onError(std::string_view) override {}
      void onClose() override {}

      std::vector<std::string> lines;
      std::vector<std::vector<uint8_t>> payloads;

     private:
      sdl3mcp::StreamBase& stream_;
   };

   TestStream stream;
   PayloadHandler handler(stream);
   stream.setHandler(&handler);

   const uint8_t bytes[] = {'f', 'r', 'a', 'm', 'e', '\n', 0x0A, 0x0D, 0x00, 0xFF,
                            'n', 'e', 'x', 't', '\n'};
   stream.feed(bytes, sizeof bytes);

   REQUIRE(handler.lines.size() == 2);
   CHECK(handler.lines[0] == "frame");
   CHECK(handler.lines[1] == "next");
   REQUIRE(handler.payloads.size() == 1);
   /* Payload bytes are opaque: a newline or a CR inside them is data. */
   CHECK(handler.payloads[0] == std::vector<uint8_t>{0x0A, 0x0D, 0x00, 0xFF});
}

TEST_CASE("stream reports an error and close exactly once") {
   RecordingHandler handler;
   TestStream stream;
   stream.setHandler(&handler);

   stream.fail("boom");
   REQUIRE(handler.errors.size() == 1);
   CHECK(handler.errors[0] == "boom");
   CHECK(handler.closes == 1);

   stream.fail("again");
   stream.finish();
   CHECK(handler.errors.size() == 1);
   CHECK(handler.closes == 1);
}

TEST_CASE("tool catalog is a map by name") {
   sdl3mcp::ToolCatalog catalog;
   CHECK(catalog.empty());
   CHECK(catalog.find("alpha") == nullptr);

   catalog.add(makeTool("beta", "second", [](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate&) {}));
   catalog.add(makeTool("alpha", "first", [](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate&) {}));
   CHECK(catalog.size() == 2);

   const sdl3mcp::Tool* alpha = catalog.find("alpha");
   REQUIRE(alpha != nullptr);
   CHECK(alpha->description == "first");
   CHECK(alpha->inputSchema["type"] == "object");
   CHECK(alpha->annotations["readOnlyHint"] == true);
   CHECK(catalog.find("nope") == nullptr);

   /* A second add with the same name replaces it: the Handler merge relies on
      the peer winning a collision. */
   catalog.add(makeTool("alpha", "replaced", [](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate&) {}));
   CHECK(catalog.size() == 2);
   CHECK(catalog.find("alpha")->description == "replaced");

   std::vector<std::string> names;
   for (const auto& entry : catalog.tools()) {
      names.push_back(entry.first);
   }
   CHECK(names == std::vector<std::string>{"alpha", "beta"});
}

TEST_CASE("a tool hands its order to its invoke function") {
   sdl3mcp::Tool tool = makeTool(
       "echo", "answers with its arguments",
       [](sdl3mcp::ToolOrder order, sdl3mcp::ToolCompletionDelegate& completion) {
          completion.complete(sdl3mcp::ToolResult::text(order.arguments.dump()));
       });

   RecordingCompletion completion;
   tool.invoke(sdl3mcp::ToolOrder{7, "echo", nlohmann::json{{"x", 1}}}, completion);

   REQUIRE(completion.completed.size() == 1);
   REQUIRE(completion.completed[0].content.is_array());
   CHECK(completion.completed[0].content[0]["text"] == R"({"x":1})");
   CHECK(!completion.completed[0].isError);
}

TEST_CASE("a simple tool result encodes as a content array") {
   TestStream stream;
   const sdl3mcp::ToolResult result = sdl3mcp::ToolResult::text("hello");
   CHECK(!result.isRawFrame());
   REQUIRE(result.encode(stream, 5));

   REQUIRE(stream.sent.size() == 1);
   CHECK(stream.binary.empty());
   const nlohmann::json response = nlohmann::json::parse(stream.sent[0]);
   CHECK(response["jsonrpc"] == "2.0");
   CHECK(response["id"] == 5);
   CHECK(response["result"]["content"] == result.content);
   CHECK(response["result"]["content"][0]["type"] == "text");
   CHECK(response["result"]["content"][0]["text"] == "hello");
   CHECK(response["result"]["isError"] == false);
}

TEST_CASE("an error tool result keeps its flag") {
   TestStream stream;
   const sdl3mcp::ToolResult result = sdl3mcp::ToolResult::text("boom", true);
   REQUIRE(result.encode(stream, "id"));
   REQUIRE(stream.sent.size() == 1);
   const nlohmann::json response = nlohmann::json::parse(stream.sent[0]);
   CHECK(response["id"] == "id");
   CHECK(response["result"]["isError"] == true);
}

TEST_CASE("a raw tool result writes its header line and then the pixels") {
   TestStream stream;
   const std::vector<uint8_t> pixels{1, 2, 3, 4, 5, 6, 7, 8};
   const sdl3mcp::ToolResult result =
       sdl3mcp::ToolResult::rawFrame(2, 1, 42, pixels, "frame 42");
   CHECK(result.isRawFrame());
   CHECK(result.payload == pixels);
   REQUIRE(result.encode(stream, 7));

   REQUIRE(stream.sent.size() == 1);
   const nlohmann::json header = nlohmann::json::parse(stream.sent[0]);
   CHECK(header["jsonrpc"] == "2.0");
   CHECK(header["id"] == 7);
   CHECK(header["result"]["format"] == "rgba8888");
   CHECK(header["result"]["width"] == 2);
   CHECK(header["result"]["height"] == 1);
   CHECK(header["result"]["byteLength"] == pixels.size());
   CHECK(header["result"]["frame"] == 42);
   CHECK(header["result"]["text"] == "frame 42");
   /* The newline ends the header line only: the pixels follow it exactly. */
   CHECK(stream.binary == pixels);
}

TEST_CASE("a raw tool result without text omits the field") {
   TestStream stream;
   const sdl3mcp::ToolResult result =
       sdl3mcp::ToolResult::rawFrame(1, 1, 3, {9, 8, 7, 6});
   REQUIRE(result.encode(stream, 1));
   const nlohmann::json header = nlohmann::json::parse(stream.sent[0]);
   CHECK(!header["result"].contains("text"));
   CHECK(header["result"]["byteLength"] == 4);
}

TEST_CASE("an empty payload selects a simple result") {
   TestStream stream;
   sdl3mcp::ToolResult result;
   result.content = nlohmann::json::array({{{"type", "text"}, {"text", "none"}}});
   CHECK(!result.isRawFrame());
   REQUIRE(result.encode(stream, 1));
   REQUIRE(stream.sent.size() == 1);
   CHECK(stream.binary.empty());
   const nlohmann::json response = nlohmann::json::parse(stream.sent[0]);
   CHECK(response["result"].contains("content"));
   CHECK(!response["result"].contains("format"));
}

TEST_CASE("a write failure stops the encoding") {
   TestStream stream;
   stream.writeFails = true;
   const sdl3mcp::ToolResult result =
       sdl3mcp::ToolResult::rawFrame(1, 1, 1, {1, 2, 3, 4});
   CHECK(!result.encode(stream, 1));
   CHECK(stream.sent.empty());
   CHECK(stream.binary.empty());
}

TEST_CASE("a simple result decodes from a peer") {
   const nlohmann::json result{
       {"content", nlohmann::json::array({{{"type", "text"}, {"text", "ok"}}})},
       {"isError", true},
   };
   const auto decoded = sdl3mcp::ToolResult::decode(result, {});
   REQUIRE(decoded.hasValue());
   CHECK(decoded->content == result["content"]);
   CHECK(decoded->payload.empty());
   CHECK(decoded->isError);
}

TEST_CASE("a raw frame decodes from a peer") {
   const nlohmann::json header{
       {"format", "rgba8888"},
       {"width", 2},
       {"height", 1},
       {"byteLength", 8},
       {"frame", 9},
   };
   const std::vector<uint8_t> pixels{1, 2, 3, 4, 5, 6, 7, 8};
   const auto decoded = sdl3mcp::ToolResult::decode(header, pixels);
   REQUIRE(decoded.hasValue());
   CHECK(decoded->content == header);
   CHECK(decoded->payload == pixels);
   CHECK(!decoded->isError);
   CHECK(decoded->isRawFrame());
}

TEST_CASE("a malformed raw frame does not decode") {
   const std::vector<uint8_t> pixels{1, 2, 3, 4, 5, 6, 7, 8};

   {
      const nlohmann::json header{{"format", "rgba8888"},
                                  {"width", 2},
                                  {"height", 1},
                                  {"byteLength", 4}};
      const auto decoded = sdl3mcp::ToolResult::decode(header, pixels);
      CHECK(!decoded);
      CHECK(!decoded.error().message.empty());
   }
   {
      const nlohmann::json header{{"format", "rgb8888"},
                                  {"width", 2},
                                  {"height", 1},
                                  {"byteLength", 8}};
      const auto decoded = sdl3mcp::ToolResult::decode(header, pixels);
      CHECK(!decoded);
   }
   {
      const nlohmann::json header{{"format", "rgba8888"},
                                  {"width", 0},
                                  {"height", 1},
                                  {"byteLength", 0}};
      const auto decoded = sdl3mcp::ToolResult::decode(header, {});
      CHECK(!decoded);
   }
}

TEST_CASE("a result with neither shape does not decode") {
   const auto empty = sdl3mcp::ToolResult::decode(nlohmann::json::object(), {});
   CHECK(!empty);
   CHECK(!empty.error().message.empty());

   const nlohmann::json badContent{{"content", "not an array"}};
   CHECK(!sdl3mcp::ToolResult::decode(badContent, {}));
}

TEST_CASE("session initialize answers with its own revision") {
   SessionFixture fixture;
   fixture.feed(
       R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2026-07-28"}})");

   REQUIRE(fixture.stream->sent.size() == 1);
   const nlohmann::json response = fixture.answer();
   CHECK(response["jsonrpc"] == "2.0");
   CHECK(response["id"] == 1);
   CHECK(response["result"]["protocolVersion"] == "2025-11-25");
   CHECK(response["result"]["capabilities"].is_object());
   CHECK(response["result"]["capabilities"].size() == 1);
   CHECK(response["result"]["capabilities"]["tools"].is_object());
   CHECK(response["result"]["serverInfo"]["name"] == "sdl3mcp");
   CHECK(response["result"]["serverInfo"]["version"] == SDL3MCP_VERSION_STRING);
   CHECK(fixture.session->initialized());
   CHECK(fixture.session->isOpen());
}

TEST_CASE("session initialize requires a string protocol version") {
   {
      SessionFixture fixture;
      fixture.feed(R"({"jsonrpc":"2.0","id":2,"method":"initialize"})");
      REQUIRE(fixture.stream->sent.size() == 1);
      const nlohmann::json response = fixture.answer();
      CHECK(response["error"]["code"] == -32602);
      CHECK(response["error"]["message"] ==
            "initialize requires a string protocolVersion");
      CHECK(!fixture.session->initialized());
   }
   {
      SessionFixture fixture;
      fixture.feed(
          R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":20260728}})");
      REQUIRE(fixture.stream->sent.size() == 1);
      CHECK(fixture.answer()["error"]["code"] == -32602);
      CHECK(!fixture.session->initialized());

      /* A rejected initialize does not consume the one allowed call. */
      fixture.feed(
          R"({"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
      REQUIRE(fixture.stream->sent.size() == 2);
      CHECK(fixture.answer(1)["id"] == 4);
      CHECK(fixture.session->initialized());
   }
}

TEST_CASE("session rejects a second initialize") {
   SessionFixture fixture;
   fixture.initialize();
   fixture.feed(
       R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
   REQUIRE(fixture.stream->sent.size() == 2);
   const nlohmann::json response = fixture.answer(1);
   CHECK(response["error"]["code"] == -32600);
   CHECK(response["error"]["message"] == "initialize was already called");
   CHECK(response["id"] == 2);
}

TEST_CASE("session answers ping at any time") {
   {
      SessionFixture fixture;
      fixture.feed(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
      REQUIRE(fixture.stream->sent.size() == 1);
      const nlohmann::json response = fixture.answer();
      CHECK(response["id"] == 1);
      CHECK(response["result"].is_object());
      CHECK(response["result"].empty());
      CHECK(!fixture.session->initialized());
   }
   {
      SessionFixture fixture;
      fixture.initialize();
      fixture.feed(R"({"jsonrpc":"2.0","id":"pong","method":"ping"})");
      REQUIRE(fixture.stream->sent.size() == 2);
      CHECK(fixture.answer(1)["id"] == "pong");
   }
}

TEST_CASE("session refuses tools before initialize") {
   SessionFixture fixture;
   fixture.feed(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
   REQUIRE(fixture.stream->sent.size() == 1);
   nlohmann::json response = fixture.answer();
   CHECK(response["error"]["code"] == -32600);
   CHECK(response["error"]["message"] == "tools are unavailable before initialize");

   fixture.feed(callLine(2, "sdl_info"));
   REQUIRE(fixture.stream->sent.size() == 2);
   response = fixture.answer(1);
   CHECK(response["error"]["code"] == -32600);
   CHECK(response["id"] == 2);
}

TEST_CASE("session lists the catalog after initialize") {
   SessionFixture fixture;
   fixture.catalog.add(
       makeTool("alpha", "first", [](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate&) {}));
   fixture.catalog.add(
       makeTool("beta", "second", [](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate&) {}));
   fixture.initialize();
   fixture.feed(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");

   REQUIRE(fixture.stream->sent.size() == 2);
   const nlohmann::json response = fixture.answer(1);
   CHECK(response["id"] == 2);
   const nlohmann::json& tools = response["result"]["tools"];
   REQUIRE(tools.is_array());
   REQUIRE(tools.size() == 2);
   CHECK(tools[0]["name"] == "alpha");
   CHECK(tools[0]["description"] == "first");
   CHECK(tools[0]["inputSchema"].is_object());
   CHECK(tools[0]["annotations"].is_object());
   CHECK(tools[1]["name"] == "beta");
}

TEST_CASE("session answers an unknown method") {
   SessionFixture fixture;
   fixture.initialize();
   fixture.feed(R"({"jsonrpc":"2.0","id":3,"method":"resources/list"})");
   REQUIRE(fixture.stream->sent.size() == 2);
   const nlohmann::json response = fixture.answer(1);
   CHECK(response["error"]["code"] == -32601);
   CHECK(response["error"]["message"] == "Method not found: resources/list");
   CHECK(response["id"] == 3);
}

TEST_CASE("session notifications produce no output and no state change") {
   SessionFixture fixture;
   fixture.feed(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
   CHECK(fixture.stream->sent.empty());
   CHECK(!fixture.session->initialized());

   fixture.initialize();
   fixture.feed(
       R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})");
   CHECK(fixture.stream->sent.size() == 1);
   CHECK(fixture.session->initialized());
}

TEST_CASE("session answers a malformed line and keeps going") {
   SessionFixture fixture;
   fixture.feed("{");
   REQUIRE(fixture.stream->sent.size() == 1);
   const nlohmann::json response = fixture.answer();
   CHECK(response["id"].is_null());
   CHECK(response["error"]["code"] == -32700);
   CHECK(fixture.session->isOpen());

   fixture.feed(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
   REQUIRE(fixture.stream->sent.size() == 2);
   CHECK(fixture.answer(1)["id"] == 1);
}

TEST_CASE("session accepts a CRLF line") {
   SessionFixture fixture;
   fixture.feed("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\r\n");
   REQUIRE(fixture.stream->sent.size() == 1);
   CHECK(fixture.answer()["id"] == 1);
}

TEST_CASE("session validates tools/call") {
   SessionFixture fixture;
   fixture.catalog.add(makeTool(
       "alpha", "first",
       [](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate&) {}));
   fixture.initialize();

   {
      fixture.feed(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{}})");
      REQUIRE(fixture.stream->sent.size() == 2);
      const nlohmann::json response = fixture.answer(1);
      CHECK(response["error"]["code"] == -32602);
      CHECK(response["error"]["message"] ==
            "tools/call requires a string tool name");
   }
   {
      fixture.feed(callLine(3, "nope"));
      REQUIRE(fixture.stream->sent.size() == 3);
      const nlohmann::json response = fixture.answer(2);
      CHECK(response["error"]["code"] == -32602);
      CHECK(response["error"]["message"] == "Unknown tool: nope");
   }
   {
      fixture.feed(callLine(4, "alpha", nlohmann::json::array({1, 2})));
      REQUIRE(fixture.stream->sent.size() == 4);
      const nlohmann::json response = fixture.answer(3);
      CHECK(response["error"]["code"] == -32602);
      CHECK(response["error"]["message"] ==
            "tools/call arguments must be a JSON object");
   }
}

TEST_CASE("session passes the order and answers an inline simple result") {
   SessionFixture fixture;
   nlohmann::json seenArguments;
   fixture.catalog.add(makeTool(
       "echo", "echoes",
       [&seenArguments](sdl3mcp::ToolOrder order,
                        sdl3mcp::ToolCompletionDelegate& completion) {
          CHECK(order.id == 2);
          CHECK(order.name == "echo");
          seenArguments = order.arguments;
          completion.complete(sdl3mcp::ToolResult::text(order.arguments.dump()));
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "echo", nlohmann::json{{"x", 1}}));

   REQUIRE(fixture.stream->sent.size() == 2);
   CHECK(seenArguments == nlohmann::json{{"x", 1}});
   const nlohmann::json response = fixture.answer(1);
   CHECK(response["id"] == 2);
   CHECK(response["result"]["content"][0]["text"] == R"({"x":1})");
   CHECK(response["result"]["isError"] == false);
   CHECK(fixture.stream->binary.empty());
}

TEST_CASE("session treats absent arguments as an empty object") {
   SessionFixture fixture;
   nlohmann::json seenArguments = nlohmann::json::array();
   fixture.catalog.add(makeTool(
       "noargs", "takes nothing",
       [&seenArguments](sdl3mcp::ToolOrder order,
                        sdl3mcp::ToolCompletionDelegate& completion) {
          seenArguments = order.arguments;
          completion.complete(sdl3mcp::ToolResult::text("ok"));
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "noargs"));

   REQUIRE(fixture.stream->sent.size() == 2);
   CHECK(seenArguments.is_object());
   CHECK(seenArguments.empty());
}

TEST_CASE("session answers an inline raw frame") {
   SessionFixture fixture;
   fixture.catalog.add(makeTool(
       "frame", "one frame",
       [](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate& completion) {
          completion.complete(
              sdl3mcp::ToolResult::rawFrame(1, 1, 5, {1, 2, 3, 4}, "frame 5"));
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "frame"));

   REQUIRE(fixture.stream->sent.size() == 2);
   const nlohmann::json header = fixture.answer(1);
   CHECK(header["id"] == 2);
   CHECK(header["result"]["format"] == "rgba8888");
   CHECK(header["result"]["width"] == 1);
   CHECK(header["result"]["height"] == 1);
   CHECK(header["result"]["byteLength"] == 4);
   CHECK(header["result"]["frame"] == 5);
   CHECK(header["result"]["text"] == "frame 5");
   CHECK(fixture.stream->binary == std::vector<uint8_t>{1, 2, 3, 4});
}

TEST_CASE("session completes a deferred order later") {
   SessionFixture fixture;
   sdl3mcp::ToolCompletionDelegate* saved = nullptr;
   fixture.catalog.add(makeTool(
       "defer", "answers later",
       [&saved](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate& completion) {
          saved = &completion;
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "defer"));
   CHECK(fixture.stream->sent.size() == 1);
   CHECK(fixture.session->hasPendingOrder());
   REQUIRE(saved != nullptr);

   saved->complete(sdl3mcp::ToolResult::text("later"));
   REQUIRE(fixture.stream->sent.size() == 2);
   const nlohmann::json response = fixture.answer(1);
   CHECK(response["id"] == 2);
   CHECK(response["result"]["content"][0]["text"] == "later");
   CHECK(!fixture.session->hasPendingOrder());

   /* A second completion has no order left to answer. */
   saved->complete(sdl3mcp::ToolResult::text("again"));
   CHECK(fixture.stream->sent.size() == 2);
}

TEST_CASE("session ignores a completion after cancellation") {
   SessionFixture fixture;
   sdl3mcp::ToolCompletionDelegate* saved = nullptr;
   fixture.catalog.add(makeTool(
       "defer", "answers later",
       [&saved](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate& completion) {
          saved = &completion;
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "defer"));
   REQUIRE(saved != nullptr);

   saved->cancelled();
   saved->complete(sdl3mcp::ToolResult::text("late"));
   CHECK(fixture.stream->sent.size() == 1);
   CHECK(!fixture.session->hasPendingOrder());

   /* The session is still usable after a cancellation. */
   fixture.feed(R"({"jsonrpc":"2.0","id":3,"method":"ping"})");
   REQUIRE(fixture.stream->sent.size() == 2);
   CHECK(fixture.answer(1)["id"] == 3);
}

TEST_CASE("session dispatches a later request while a deferred order is pending") {
   SessionFixture fixture;
   std::vector<sdl3mcp::ToolCompletionDelegate*> saved;
   fixture.catalog.add(makeTool(
       "defer", "answers later",
       [&saved](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate& completion) {
          saved.push_back(&completion);
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "defer"));
   fixture.feed(R"({"jsonrpc":"2.0","id":3,"method":"ping"})");
   /* A deferred order does not block a later request: the ping answers now. */
   REQUIRE(fixture.stream->sent.size() == 2);
   CHECK(fixture.answer(1)["id"] == 3);
   REQUIRE(saved.size() == 1);

   saved[0]->complete(sdl3mcp::ToolResult::text("done"));
   REQUIRE(fixture.stream->sent.size() == 3);
   CHECK(fixture.answer(2)["id"] == 2);
}

TEST_CASE("session keeps independent deferred orders") {
   SessionFixture fixture;
   std::vector<sdl3mcp::ToolCompletionDelegate*> saved;
   fixture.catalog.add(makeTool(
       "defer", "answers later",
       [&saved](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate& completion) {
          saved.push_back(&completion);
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "defer"));
   fixture.feed(callLine(3, "defer"));
   CHECK(fixture.stream->sent.size() == 1);
   REQUIRE(saved.size() == 2);
   CHECK(fixture.session->hasPendingOrder());

   /* Each order answers with its own id, even out of order. */
   saved[1]->complete(sdl3mcp::ToolResult::text("second"));
   REQUIRE(fixture.stream->sent.size() == 2);
   CHECK(fixture.answer(1)["id"] == 3);
   CHECK(fixture.answer(1)["result"]["content"][0]["text"] == "second");

   saved[0]->complete(sdl3mcp::ToolResult::text("first"));
   REQUIRE(fixture.stream->sent.size() == 3);
   CHECK(fixture.answer(2)["id"] == 2);
   CHECK(fixture.answer(2)["result"]["content"][0]["text"] == "first");
   CHECK(!fixture.session->hasPendingOrder());
}

TEST_CASE("session drops a completion after close") {
   SessionFixture fixture;
   sdl3mcp::ToolCompletionDelegate* saved = nullptr;
   fixture.catalog.add(makeTool(
       "defer", "answers later",
       [&saved](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate& completion) {
          saved = &completion;
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "defer"));
   REQUIRE(saved != nullptr);

   fixture.session->close();
   CHECK(!fixture.session->isOpen());
   saved->complete(sdl3mcp::ToolResult::text("late"));
   CHECK(fixture.stream->sent.size() == 1);
   CHECK(!fixture.session->hasPendingOrder());
}

TEST_CASE("session closes when a write fails") {
   SessionFixture fixture;
   fixture.stream->writeFails = true;
   fixture.feed(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
   CHECK(fixture.stream->sent.empty());
   CHECK(fixture.stream->closeCount == 1);
   CHECK(!fixture.session->isOpen());

   fixture.stream->writeFails = false;
   fixture.feed(R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
   CHECK(fixture.stream->sent.empty());
}

TEST_CASE("session pumps its stream") {
   SessionFixture fixture;
   fixture.session->pump();
   CHECK(fixture.stream->pumpCount == 1);
}

TEST_CASE("stream reports a partial line when the peer closes") {
   RecordingHandler handler;
   TestStream stream;
   stream.setHandler(&handler);

   stream.feed("half");
   stream.finish();

   CHECK(handler.lines.empty());
   REQUIRE(handler.errors.size() == 1);
   CHECK(handler.errors[0].find("middle of a line") != std::string::npos);
   CHECK(handler.closes == 1);
}

TEST_CASE("session closes when its stream fails") {
   SessionFixture fixture;
   sdl3mcp::ToolCompletionDelegate* saved = nullptr;
   fixture.catalog.add(makeTool(
       "defer", "answers later",
       [&saved](sdl3mcp::ToolOrder, sdl3mcp::ToolCompletionDelegate& completion) {
          saved = &completion;
       }));

   fixture.initialize();
   fixture.feed(callLine(2, "defer"));
   REQUIRE(saved != nullptr);
   CHECK(fixture.session->hasPendingOrder());

   fixture.stream->fail("broken");
   CHECK(!fixture.session->isOpen());
   CHECK(fixture.session->lastError() == "broken");
   CHECK(!fixture.session->hasPendingOrder());

   /* The transport is gone, so a late completion has nowhere to answer. */
   saved->complete(sdl3mcp::ToolResult::text("late"));
   CHECK(fixture.stream->sent.size() == 1);
}
