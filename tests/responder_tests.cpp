/*
  The responder answers a tool call at two moments: immediately, inside
  handleLine, and — on Emscripten — frames later, when the app thread produces
  the result. Both must put the same bytes on the wire, so this test drives the
  synchronous path and the deferred answer() over the same fake sink and
  compares the two wirings byte for byte.

  The sink flattens the wire: a line write appends its newline, a byte write
  appends as-is, which is exactly what a client sees for a content result and
  for a raw-frame header followed by its pixels. A failing write can be forced
  to cover the error path both entry points share.
*/

#include "responder.hpp"

#include "session.hpp"
#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{

using sdl3mcp::ByteSink;
using sdl3mcp::Frame;
using sdl3mcp::Responder;
using sdl3mcp::RpcMessage;
using sdl3mcp::Session;
using sdl3mcp::ToolCall;
using sdl3mcp::ToolResult;

constexpr std::string_view kCaptureLine =
    R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"sdl_capture","arguments":{}}})";

class FakeSink : public ByteSink
{
public:
    bool writeLine(std::string_view line) override
    {
        if (failLines) {
            errno = EPIPE;
            return false;
        }
        wire.append(line);
        wire.push_back('\n');
        return true;
    }

    bool writeBytes(const uint8_t *data, std::size_t size) override
    {
        if (failBytes) {
            errno = EPIPE;
            return false;
        }
        wire.append(reinterpret_cast<const char *>(data), size);
        return true;
    }

    std::string wire;
    bool failLines = false;
    bool failBytes = false;
};

/* A session that has completed initialize, so tools/call is dispatched. */
void initializeSession(Session &session)
{
    RpcMessage message;
    nlohmann::json parseError;
    CHECK(sdl3mcp::parseMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})",
        message, parseError));
    CHECK(parseError.is_null());
    session.handle(message);
}

ToolCall parsedCall(Session &session, std::string_view line)
{
    RpcMessage message;
    nlohmann::json parseError;
    CHECK(sdl3mcp::parseMessage(line, message, parseError));
    CHECK(parseError.is_null());
    const sdl3mcp::SessionResult result = session.handle(message);
    CHECK(!result.response.has_value());
    CHECK(result.toolCall.has_value());
    return *result.toolCall;
}

/* handleLine's bytes for `result`, over a handler that always returns it. */
std::string synchronousWire(const ToolResult &result)
{
    Session session;
    initializeSession(session);
    FakeSink sink;
    Responder responder(sink, session,
                        [&](const ToolCall &) { return result; });
    std::string error;
    CHECK(responder.handleLine(kCaptureLine, error));
    CHECK(error.empty());
    return sink.wire;
}

/* answer()'s bytes for the stored call and the same result. */
std::string deferredWire(const ToolResult &result)
{
    Session parsingSession;
    initializeSession(parsingSession);
    const ToolCall call = parsedCall(parsingSession, kCaptureLine);

    Session session;
    FakeSink sink;
    Responder responder(sink, session, [](const ToolCall &) {
        /* The deferred path must not run the handler again. */
        CHECK(false);
        return ToolResult{};
    });
    std::string error;
    CHECK(responder.answer(call, result, error));
    CHECK(error.empty());
    return sink.wire;
}

void expectSameWire(const ToolResult &result)
{
    const std::string synchronous = synchronousWire(result);
    CHECK(!synchronous.empty());
    CHECK(deferredWire(result) == synchronous);
}

void testContentResult()
{
    ToolResult result;
    result.content = nlohmann::json::array(
        {{{"type", "text"}, {"text", "hello"}}});
    expectSameWire(result);
}

void testErrorResult()
{
    ToolResult result;
    result.content = nlohmann::json::array(
        {{{"type", "text"}, {"text", "no window"}}});
    result.isError = true;
    expectSameWire(result);
}

void testFrameResult()
{
    ToolResult result;
    result.frame = Frame{2, 1, {1, 2, 3, 4, 5, 6, 7, 8}, 42};
    expectSameWire(result);

    /* The header line is followed by exactly the pixels, with nothing between. */
    const std::string wire = deferredWire(result);
    const std::size_t newline = wire.find('\n');
    CHECK(newline != std::string::npos);
    const nlohmann::json header = nlohmann::json::parse(wire.substr(0, newline));
    CHECK(header["result"]["format"] == "rgba8888");
    CHECK(header["result"]["width"] == 2);
    CHECK(header["result"]["height"] == 1);
    CHECK(header["result"]["byteLength"] == 8);
    CHECK(header["result"]["frame"] == 42);
    CHECK(!header["result"].contains("text"));
    CHECK(wire.size() == newline + 1 + 8);
    CHECK(wire.substr(newline + 1) == std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
}

void testFrameResultWithText()
{
    ToolResult result;
    result.content = nlohmann::json::array();
    result.frameText = "frame 42";
    result.frame = Frame{1, 1, {9, 8, 7, 6}, 42};
    expectSameWire(result);

    const std::string wire = deferredWire(result);
    const nlohmann::json header =
        nlohmann::json::parse(wire.substr(0, wire.find('\n')));
    CHECK(header["result"]["text"] == "frame 42");
}

/*
  A handler that registered the call has no answer yet: handleLine must put
  nothing on the wire, and the call is answered exactly once later through
  answer(). Anything else would send two responses for one id.
*/
void testDeferredResultWritesNothing()
{
    Session session;
    initializeSession(session);
    FakeSink sink;
    Responder responder(sink, session, [](const ToolCall &) {
        ToolResult result;
        result.deferred = true;
        return result;
    });

    std::string error;
    CHECK(responder.handleLine(kCaptureLine, error));
    CHECK(error.empty());
    CHECK(sink.wire.empty());

    Session parsingSession;
    initializeSession(parsingSession);
    const ToolCall call = parsedCall(parsingSession, kCaptureLine);

    ToolResult registered;
    registered.frame = Frame{1, 1, {1, 2, 3, 4}, 3};
    CHECK(responder.answer(call, registered, error));
    CHECK(error.empty());

    /* One response for id 7, then exactly the four pixel bytes. */
    const std::size_t newline = sink.wire.find('\n');
    CHECK(newline != std::string::npos);
    const nlohmann::json header = nlohmann::json::parse(sink.wire.substr(0, newline));
    CHECK(header["id"] == 7);
    CHECK(header["result"]["byteLength"] == 4);
    CHECK(sink.wire.size() == newline + 1 + 4);
}

void testWriteFailure()
{
    Session parsingSession;
    initializeSession(parsingSession);
    const ToolCall call = parsedCall(parsingSession, kCaptureLine);

    Session session;
    FakeSink sink;
    sink.failBytes = true;
    Responder responder(sink, session, [](const ToolCall &) { return ToolResult{}; });

    ToolResult result;
    result.frame = Frame{1, 1, {1, 2, 3, 4}, 1};
    std::string error;
    CHECK(!responder.answer(call, result, error));
    CHECK(!error.empty());
    CHECK(error.find("frame payload") != std::string::npos);

    sink.failBytes = false;
    sink.failLines = true;
    CHECK(!responder.answer(call, result, error));
    CHECK(!error.empty());
}

} // namespace

int main()
{
    testContentResult();
    testErrorResult();
    testFrameResult();
    testFrameResultWithText();
    testDeferredResultWritesNothing();
    testWriteFailure();
    return 0;
}
