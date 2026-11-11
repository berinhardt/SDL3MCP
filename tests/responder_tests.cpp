/*
  The responder is the protocol code both platform arms share: one line in, one
  answer out, over a ByteSink. Most answers are ordinary requests, but the
  raw-frame contract is the one rule whose failure mode is silent: a header whose
  byteLength does not match the payload behind it makes sdl_capture time out
  instead of reporting an error. These cases pin the answers a client receives,
  one situation per case, over a sink that records each write as it arrives.

  No SDL window, GPU, transport, or browser is involved. A Session is driven
  through the same handleLine path the server uses, so tools/list comes from the
  real catalog, and a stub handler supplies the ToolResults the tools produce.
  The deferred path is covered too, because a browser answers frames later and
  both wirings must put the same bytes on the wire.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "responder.hpp"

#include "session.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using sdl3mcp::ByteSink;
using sdl3mcp::Frame;
using sdl3mcp::Responder;
using sdl3mcp::RpcMessage;
using sdl3mcp::Session;
using sdl3mcp::SessionResult;
using sdl3mcp::ToolCall;
using sdl3mcp::ToolHandler;
using sdl3mcp::ToolResult;

constexpr std::string_view kInitializeLine =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})";
constexpr std::string_view kToolsListLine =
    R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})";
constexpr std::string_view kCaptureLine =
    R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"sdl_capture","arguments":{}}})";
constexpr std::string_view kNotificationLine =
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
constexpr std::string_view kUnknownMethodLine =
    R"({"jsonrpc":"2.0","id":9,"method":"resources/list"})";
constexpr std::string_view kMalformedLine = R"({"jsonrpc":"2.0","id":3,)";

/*
  What a client receives: each writeLine as one string without its terminator,
  and every writeBytes appended to one byte vector. Keeping the two shapes apart
  is what lets a case assert that the newline ends the header line only and that
  the pixels follow it with nothing else.
*/
class RecordingSink : public ByteSink
{
public:
    bool writeLine(std::string_view line) override
    {
        lines.emplace_back(line);
        return true;
    }

    bool writeBytes(const uint8_t *data, std::size_t size) override
    {
        bytes.insert(bytes.end(), data, data + size);
        return true;
    }

    std::vector<std::string> lines;
    std::vector<uint8_t> bytes;
};

/* Accepts the answer line and refuses the pixels, as a client that died mid-frame. */
class FailingSink : public ByteSink
{
public:
    bool writeLine(std::string_view line) override
    {
        lines.emplace_back(line);
        return true;
    }

    bool writeBytes(const uint8_t *, std::size_t) override
    {
        errno = EPIPE;
        return false;
    }

    std::vector<std::string> lines;
};

/* A session past initialize, which is when tools/list and tools/call are answered. */
Session initializedSession()
{
    Session session;
    RpcMessage message;
    nlohmann::json parseError;
    REQUIRE(sdl3mcp::parseMessage(kInitializeLine, message, parseError));
    REQUIRE(parseError.is_null());
    const SessionResult result = session.handle(message);
    REQUIRE(result.response.has_value());
    REQUIRE(session.initialized());
    return session;
}

/* The call the session hands a handler for `line`. */
ToolCall parseCall(std::string_view line)
{
    Session session = initializedSession();
    RpcMessage message;
    nlohmann::json parseError;
    REQUIRE(sdl3mcp::parseMessage(line, message, parseError));
    REQUIRE(parseError.is_null());
    const SessionResult result = session.handle(message);
    REQUIRE(result.toolCall.has_value());
    return *result.toolCall;
}

/* The stub executor: one canned answer, whatever the call was. */
ToolHandler returning(ToolResult result)
{
    return [result = std::move(result)](const ToolCall &) { return result; };
}

/* For messages the session answers itself, where running the tools would be a bug. */
ToolHandler unreachableHandler()
{
    return [](const ToolCall &) {
        CHECK_MESSAGE(false, "the tool handler must not run here");
        return ToolResult{};
    };
}

/* The single response line, parsed. */
nlohmann::json answerLine(const RecordingSink &sink)
{
    REQUIRE(sink.lines.size() == 1);
    return nlohmann::json::parse(sink.lines.front());
}

TEST_CASE("tools/list answers with the tool list")
{
    Session session = initializedSession();
    RecordingSink sink;
    Responder responder(sink, session, unreachableHandler());

    std::string error;
    CHECK(responder.handleLine(kToolsListLine, error));
    CHECK(error.empty());

    const nlohmann::json response = answerLine(sink);
    CHECK(sink.bytes.empty());
    CHECK(response["id"] == 2);
    REQUIRE(response.contains("result"));
    const nlohmann::json &tools = response["result"]["tools"];
    REQUIRE(tools.is_array());
    CHECK(!tools.empty());
    CHECK(tools.front().contains("name"));
    CHECK(tools.front().contains("inputSchema"));
}

TEST_CASE("an unknown method is answered as an error")
{
    Session session = initializedSession();
    RecordingSink sink;
    Responder responder(sink, session, unreachableHandler());

    std::string error;
    CHECK(responder.handleLine(kUnknownMethodLine, error));
    CHECK(error.empty());

    const nlohmann::json response = answerLine(sink);
    CHECK(sink.bytes.empty());
    CHECK(response["id"] == 9);
    REQUIRE(response.contains("error"));
    CHECK(!response.contains("result"));
    CHECK(response["error"]["code"].get<int>() == -32601);
}

TEST_CASE("a malformed line is answered, not fatal")
{
    Session session = initializedSession();
    RecordingSink sink;
    Responder responder(sink, session, unreachableHandler());

    std::string error;
    CHECK(responder.handleLine(kMalformedLine, error));
    CHECK(error.empty());

    const nlohmann::json response = answerLine(sink);
    CHECK(sink.bytes.empty());
    CHECK(response["id"].is_null());
    REQUIRE(response.contains("error"));
    CHECK(response["error"]["code"].get<int>() == -32700);
}

TEST_CASE("a notification produces no output")
{
    Session session = initializedSession();
    RecordingSink sink;
    Responder responder(sink, session, unreachableHandler());

    std::string error;
    CHECK(responder.handleLine(kNotificationLine, error));
    CHECK(error.empty());
    CHECK(sink.lines.empty());
    CHECK(sink.bytes.empty());
}

TEST_CASE("a content tool result is relayed")
{
    ToolResult result;
    result.content = nlohmann::json::array(
        {{{"type", "text"}, {"text", "no window"}}});
    result.isError = true;

    Session session = initializedSession();
    RecordingSink sink;
    Responder responder(sink, session, returning(result));

    std::string error;
    CHECK(responder.handleLine(kCaptureLine, error));
    CHECK(error.empty());

    const nlohmann::json response = answerLine(sink);
    CHECK(sink.bytes.empty());
    CHECK(response["id"] == 7);
    REQUIRE(response["result"].contains("content"));
    CHECK(response["result"]["content"] == result.content);
    CHECK(response["result"]["isError"].get<bool>() == result.isError);
}

TEST_CASE("a frame result writes the header line and then the pixels")
{
    const std::vector<uint8_t> pixels{1, 2, 3, 4, 5, 6, 7, 8};
    ToolResult result;
    result.frame = Frame{2, 1, pixels, 42};

    Session session = initializedSession();
    RecordingSink sink;
    Responder responder(sink, session, returning(result));

    std::string error;
    CHECK(responder.handleLine(kCaptureLine, error));
    CHECK(error.empty());

    const nlohmann::json header = answerLine(sink);
    CHECK(header["id"] == 7);
    CHECK(header["result"]["format"] == "rgba8888");
    CHECK(header["result"]["width"] == 2);
    CHECK(header["result"]["height"] == 1);
    CHECK(header["result"]["frame"] == 42);
    REQUIRE(header["result"]["byteLength"].get<std::size_t>() == pixels.size());

    /* The newline ends the header line only: the sink holds no other line, and
       the bytes run from the header to the end of the sink with nothing else. */
    CHECK(sink.lines.front().find('\n') == std::string::npos);
    CHECK(sink.bytes == pixels);
}

TEST_CASE("a frame result carries its frame text")
{
    const std::vector<uint8_t> pixels{9, 8, 7, 6};
    ToolResult result;
    result.content = nlohmann::json::array();
    result.frameText = "frame 42";
    result.frame = Frame{1, 1, pixels, 42};

    Session session = initializedSession();
    RecordingSink sink;
    Responder responder(sink, session, returning(result));

    std::string error;
    CHECK(responder.handleLine(kCaptureLine, error));
    CHECK(error.empty());

    const nlohmann::json header = answerLine(sink);
    CHECK(header["result"]["text"] == "frame 42");
    REQUIRE(header["result"]["byteLength"].get<std::size_t>() == pixels.size());
    CHECK(sink.bytes == pixels);
}

TEST_CASE("a failed write stops the responder")
{
    ToolResult result;
    result.frame = Frame{1, 1, {1, 2, 3, 4}, 1};

    Session session = initializedSession();
    FailingSink sink;
    Responder responder(sink, session, returning(result));

    std::string error;
    CHECK(!responder.handleLine(kCaptureLine, error));
    CHECK(!error.empty());
    CHECK(sink.lines.size() == 1);
}

/*
  A handler that registered the call has no answer yet: handleLine must put
  nothing on the wire, and the call is answered exactly once later through
  answer(). Anything else would send two responses for one id.
*/
TEST_CASE("a deferred result writes nothing until it is answered")
{
    Session session = initializedSession();
    RecordingSink sink;
    Responder responder(sink, session, [](const ToolCall &) {
        ToolResult deferred;
        deferred.deferred = true;
        return deferred;
    });

    std::string error;
    CHECK(responder.handleLine(kCaptureLine, error));
    CHECK(error.empty());
    CHECK(sink.lines.empty());
    CHECK(sink.bytes.empty());

    ToolResult registered;
    registered.frame = Frame{1, 1, {1, 2, 3, 4}, 3};
    CHECK(responder.answer(parseCall(kCaptureLine), registered, error));
    CHECK(error.empty());

    const nlohmann::json header = answerLine(sink);
    CHECK(header["id"] == 7);
    CHECK(header["result"]["byteLength"].get<std::size_t>() == 4);
    CHECK(sink.bytes.size() == 4);
}

/* answer() must not run the handler again, and must frame exactly as handleLine does. */
void expectSameAnswers(const ToolResult &result)
{
    Session synchronousSession = initializedSession();
    RecordingSink synchronousSink;
    Responder synchronous(synchronousSink, synchronousSession, returning(result));
    std::string error;
    REQUIRE(synchronous.handleLine(kCaptureLine, error));
    REQUIRE(error.empty());
    REQUIRE(!synchronousSink.lines.empty());

    Session deferredSession = initializedSession();
    RecordingSink deferredSink;
    Responder deferred(deferredSink, deferredSession, unreachableHandler());
    REQUIRE(deferred.answer(parseCall(kCaptureLine), result, error));
    REQUIRE(error.empty());

    CHECK(deferredSink.lines == synchronousSink.lines);
    CHECK(deferredSink.bytes == synchronousSink.bytes);
}

TEST_CASE("a deferred answer writes the same bytes as handleLine")
{
    SUBCASE("content")
    {
        ToolResult result;
        result.content = nlohmann::json::array(
            {{{"type", "text"}, {"text", "hello"}}});
        expectSameAnswers(result);
    }

    SUBCASE("error content")
    {
        ToolResult result;
        result.content = nlohmann::json::array(
            {{{"type", "text"}, {"text", "no window"}}});
        result.isError = true;
        expectSameAnswers(result);
    }

    SUBCASE("frame")
    {
        ToolResult result;
        result.frame = Frame{2, 1, {1, 2, 3, 4, 5, 6, 7, 8}, 42};
        expectSameAnswers(result);
    }

    SUBCASE("frame with text")
    {
        ToolResult result;
        result.content = nlohmann::json::array();
        result.frameText = "frame 42";
        result.frame = Frame{1, 1, {9, 8, 7, 6}, 42};
        expectSameAnswers(result);
    }
}

} // namespace
