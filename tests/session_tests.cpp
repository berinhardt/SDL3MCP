#include "session.hpp"

#include "test_util.hpp"
#include "tools.hpp"

#include <SDL3MCP/SDL3MCP_version.h>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace
{

using sdl3mcp::RpcErrorCode;
using sdl3mcp::RpcMessage;
using sdl3mcp::Session;
using sdl3mcp::SessionResult;
using sdl3mcp::ToolCall;
using sdl3mcp::ToolDefinition;

SessionResult dispatch(Session &session, std::string_view line)
{
    RpcMessage message;
    nlohmann::json parseError;
    CHECK(sdl3mcp::parseMessage(line, message, parseError));
    CHECK(parseError.is_null());
    return session.handle(message);
}

nlohmann::json responseOf(Session &session, std::string_view line)
{
    const SessionResult result = dispatch(session, line);
    CHECK(result.response.has_value());
    CHECK(!result.toolCall.has_value());
    const nlohmann::json &response = *result.response;
    CHECK(response["jsonrpc"] == "2.0");
    return response;
}

nlohmann::json resultOf(Session &session,
                        std::string_view line,
                        const nlohmann::json &id)
{
    const nlohmann::json response = responseOf(session, line);
    CHECK(!response.contains("error"));
    CHECK(response.contains("result"));
    CHECK(response["id"] == id);
    return response["result"];
}

void expectError(Session &session,
                 std::string_view line,
                 RpcErrorCode code,
                 const nlohmann::json &id)
{
    const nlohmann::json response = responseOf(session, line);
    CHECK(!response.contains("result"));
    CHECK(response.contains("error"));
    CHECK(response["error"]["code"].get<int>() == static_cast<int>(code));
    CHECK(response["error"]["message"].is_string());
    CHECK(response["id"] == id);
}

void expectSilent(Session &session, std::string_view line)
{
    const SessionResult result = dispatch(session, line);
    CHECK(!result.response.has_value());
    CHECK(!result.toolCall.has_value());
}

std::string initializeLine(const nlohmann::json &id,
                           const nlohmann::json &protocolVersion)
{
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "initialize"},
        {"params", nlohmann::json{{"protocolVersion", protocolVersion}}},
    }
        .dump();
}

void initializeSession(Session &session)
{
    const nlohmann::json result =
        resultOf(session, initializeLine(1, "2025-11-25"), 1);
    CHECK(session.initialized());
    CHECK(result["protocolVersion"] == "2025-11-25");
}

const nlohmann::json &schemaOf(std::string_view name)
{
    const ToolDefinition *tool = sdl3mcp::findTool(name);
    CHECK(tool != nullptr);
    return tool->inputSchema;
}

nlohmann::json actionOf(const nlohmann::json &schema, const char *type)
{
    for (const nlohmann::json &action :
         schema["properties"]["actions"]["items"]["oneOf"]) {
        if (action["properties"]["type"]["const"] == type) {
            return action;
        }
    }
    CHECK(false);
    return nlohmann::json();
}

void testInitialize()
{
    for (const char *requested : {"2025-11-25", "2026-07-28"}) {
        Session session;
        const nlohmann::json result =
            resultOf(session, initializeLine(7, requested), 7);
        CHECK(session.initialized());
        /* The server answers with its own revision, not the requested one. */
        CHECK(result["protocolVersion"] == "2025-11-25");
        CHECK(result["capabilities"].is_object());
        CHECK(result["capabilities"].size() == 1);
        CHECK(result["capabilities"]["tools"].is_object());
        CHECK(result["serverInfo"].is_object());
        CHECK(result["serverInfo"]["name"] == "sdl3mcp");
        CHECK(result["serverInfo"]["version"] == SDL3MCP_VERSION_STRING);
    }
}

void testInitializeErrors()
{
    {
        Session session;
        expectError(session,
                    R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{}})",
                    RpcErrorCode::InvalidParams, 2);
        CHECK(!session.initialized());
    }
    {
        /* Absent params and absent protocolVersion are the same failure. */
        Session session;
        expectError(session, R"({"jsonrpc":"2.0","id":3,"method":"initialize"})",
                    RpcErrorCode::InvalidParams, 3);
        CHECK(!session.initialized());
    }
    {
        Session session;
        expectError(session, initializeLine(4, 20260728), RpcErrorCode::InvalidParams,
                    4);
        CHECK(!session.initialized());
    }
    {
        Session session;
        expectError(session, initializeLine(5, nullptr), RpcErrorCode::InvalidParams,
                    5);
        CHECK(!session.initialized());
    }
    {
        /* A rejected initialize does not consume the one allowed call. */
        Session session;
        expectError(session, initializeLine(6, 1.5), RpcErrorCode::InvalidParams, 6);
        const nlohmann::json result =
            resultOf(session, initializeLine(7, "2025-11-25"), 7);
        CHECK(session.initialized());
        CHECK(result["protocolVersion"] == "2025-11-25");
    }
    {
        /* A second initialize is an invalid request even with valid params. */
        Session session;
        initializeSession(session);
        expectError(session, initializeLine(8, "2025-11-25"),
                    RpcErrorCode::InvalidRequest, 8);
        expectError(session, initializeLine(9, "2026-07-28"),
                    RpcErrorCode::InvalidRequest, 9);
        const nlohmann::json result =
            resultOf(session, R"({"jsonrpc":"2.0","id":10,"method":"ping"})", 10);
        CHECK(result.is_object());
        CHECK(result.empty());
    }
}

void testPing()
{
    {
        Session session;
        const nlohmann::json result =
            resultOf(session, R"({"jsonrpc":"2.0","id":1,"method":"ping"})", 1);
        CHECK(result.is_object());
        CHECK(result.empty());
        CHECK(!session.initialized());
    }
    {
        Session session;
        initializeSession(session);
        const nlohmann::json result =
            resultOf(session, R"({"jsonrpc":"2.0","id":"pong","method":"ping"})",
                     "pong");
        CHECK(result.is_object());
        CHECK(result.empty());
    }
}

void testToolsRequireInitialize()
{
    Session session;
    expectError(session, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                RpcErrorCode::InvalidRequest, 1);
    expectError(
        session,
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"sdl_info"}})",
        RpcErrorCode::InvalidRequest, 2);
    CHECK(!session.initialized());
}

void testNotifications()
{
    {
        /* The notification alone never makes tools available. */
        Session session;
        expectSilent(session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        CHECK(!session.initialized());
        expectError(session, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                    RpcErrorCode::InvalidRequest, 1);
    }
    {
        Session session;
        initializeSession(session);
        expectSilent(session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        expectSilent(
            session,
            R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})");
        const nlohmann::json result =
            resultOf(session, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", 2);
        CHECK(result["tools"].is_array());
    }
}

void testUnknownMethod()
{
    {
        Session session;
        initializeSession(session);
        expectError(session,
                    R"({"jsonrpc":"2.0","id":3,"method":"resources/list"})",
                    RpcErrorCode::MethodNotFound, 3);
        expectError(session,
                    R"({"jsonrpc":"2.0","id":"x","method":"tools/frobnicate"})",
                    RpcErrorCode::MethodNotFound, "x");
    }
    {
        /* Unknown methods are method-not-found even before initialize. */
        Session session;
        expectError(session,
                    R"({"jsonrpc":"2.0","id":4,"method":"resources/list"})",
                    RpcErrorCode::MethodNotFound, 4);
    }
}

void testToolList()
{
    Session session;
    initializeSession(session);
    const nlohmann::json result =
        resultOf(session, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", 2);
    const nlohmann::json &tools = result["tools"];
    CHECK(tools.is_array());
    CHECK(tools.size() == 3);

    std::string names;
    for (const nlohmann::json &tool : tools) {
        CHECK(tool["name"].is_string());
        CHECK(tool["description"].is_string());
        CHECK(!tool["description"].get<std::string>().empty());
        CHECK(tool["inputSchema"].is_object());
        CHECK(tool["annotations"].is_object());
        /* Every schema is serializable and reparses. */
        CHECK(nlohmann::json::accept(tool["inputSchema"].dump()));
        names += tool["name"].get<std::string>() + " ";
    }
    CHECK(names == "sdl_info sdl_input sdl_capture ");

    CHECK(sdl3mcp::toolCatalog().size() == 3);
    CHECK(sdl3mcp::findTool("sdl_info") != nullptr);
    CHECK(sdl3mcp::findTool("sdl_input") != nullptr);
    CHECK(sdl3mcp::findTool("sdl_capture") != nullptr);
    CHECK(sdl3mcp::findTool("sdl_nope") == nullptr);
    CHECK(sdl3mcp::findTool("") == nullptr);
}

void testInfoSchema()
{
    const nlohmann::json &schema = schemaOf("sdl_info");
    CHECK(schema["type"] == "object");
    CHECK(schema["properties"].is_object());
    CHECK(schema["properties"].empty());
    CHECK(schema["additionalProperties"] == false);
}

void testInputSchema()
{
    const nlohmann::json &schema = schemaOf("sdl_input");
    CHECK(schema["type"] == "object");
    CHECK(schema["additionalProperties"] == false);
    CHECK(schema["required"] == nlohmann::json::array({"actions"}));

    const nlohmann::json &actions = schema["properties"]["actions"];
    CHECK(actions["type"] == "array");
    CHECK(actions["minItems"] == 1);
    CHECK(actions["maxItems"] == 256);
    CHECK(actions["items"]["oneOf"].size() == 6);

    const nlohmann::json key = actionOf(schema, "key");
    const nlohmann::json &keyProperties = key["properties"];
    CHECK(!keyProperties.contains("repeat"));
    CHECK(keyProperties["key"]["type"] == "string");
    CHECK(keyProperties["action"]["enum"] ==
          nlohmann::json::array({"down", "up", "press"}));
    CHECK(keyProperties["action"]["default"] == "press");
    CHECK(keyProperties["modifiers"]["maxItems"] == 4);
    CHECK(keyProperties["modifiers"]["items"]["enum"] ==
          nlohmann::json::array({"ctrl", "shift", "alt", "gui"}));
    CHECK(key["required"] == nlohmann::json::array({"type", "key"}));
    CHECK(key["additionalProperties"] == false);

    const nlohmann::json text = actionOf(schema, "text");
    CHECK(text["properties"]["text"]["type"] == "string");
    CHECK(text["properties"]["text"]["minLength"] == 1);
    CHECK(text["properties"]["text"]["maxLength"] == 1024);
    CHECK(text["required"] == nlohmann::json::array({"type", "text"}));
    CHECK(text["additionalProperties"] == false);

    const nlohmann::json move = actionOf(schema, "mouse_move");
    CHECK(move["properties"]["x"]["type"] == "number");
    CHECK(move["properties"]["y"]["type"] == "number");
    CHECK(move["properties"]["dx"]["type"] == "number");
    CHECK(move["properties"]["dy"]["type"] == "number");
    CHECK(move["required"] == nlohmann::json::array({"type"}));
    CHECK(move["additionalProperties"] == false);

    const nlohmann::json button = actionOf(schema, "mouse_button");
    CHECK(button["properties"]["button"]["enum"] ==
          nlohmann::json::array({"left", "middle", "right", "x1", "x2"}));
    CHECK(button["properties"]["action"]["enum"] ==
          nlohmann::json::array({"down", "up", "click"}));
    CHECK(button["properties"]["x"]["type"] == "number");
    CHECK(button["properties"]["y"]["type"] == "number");
    CHECK(button["properties"]["clicks"]["type"] == "integer");
    CHECK(button["properties"]["clicks"]["minimum"] == 1);
    CHECK(button["properties"]["clicks"]["maximum"] == 3);
    CHECK(button["properties"]["clicks"]["default"] == 1);
    CHECK(button["required"] == nlohmann::json::array({"type", "button"}));
    CHECK(button["additionalProperties"] == false);

    const nlohmann::json scroll = actionOf(schema, "scroll");
    CHECK(scroll["properties"]["x"]["default"] == 0);
    CHECK(scroll["properties"]["y"]["type"] == "number");
    CHECK(scroll["required"] == nlohmann::json::array({"type", "y"}));
    CHECK(scroll["additionalProperties"] == false);
    /* Scroll units are notches: 1.0 is one notch. */
    CHECK(scroll["description"].get<std::string>().find("1.0") !=
          std::string::npos);

    const nlohmann::json release = actionOf(schema, "release_all");
    CHECK(release["properties"].size() == 1);
    CHECK(release["required"] == nlohmann::json::array({"type"}));
    CHECK(release["additionalProperties"] == false);

    for (const nlohmann::json &action : actions["items"]["oneOf"]) {
        CHECK(action["additionalProperties"] == false);
        CHECK(action["required"].is_array());
        CHECK(!action["required"].empty());
    }
}

void testCaptureSchema()
{
    const nlohmann::json &schema = schemaOf("sdl_capture");
    CHECK(schema["type"] == "object");
    CHECK(schema["additionalProperties"] == false);
    /* No top-level required member: every parameter is optional. */
    CHECK(!schema.contains("required"));

    const nlohmann::json &maxWidth = schema["properties"]["max_width"];
    CHECK(maxWidth["type"] == "integer");
    CHECK(maxWidth["minimum"] == 16);
    CHECK(maxWidth["maximum"] == 16384);

    const nlohmann::json &timeout = schema["properties"]["timeout_ms"];
    CHECK(timeout["type"] == "integer");
    CHECK(timeout["minimum"] == 1);
    CHECK(timeout["maximum"] == 60000);

    const nlohmann::json &region = schema["properties"]["region"];
    CHECK(region["type"] == "object");
    CHECK(region["additionalProperties"] == false);
    CHECK(region["required"] ==
          nlohmann::json::array({"x", "y", "width", "height"}));
    CHECK(region["properties"].size() == 4);
    CHECK(region["properties"]["x"]["type"] == "integer");
    CHECK(region["properties"]["y"]["type"] == "integer");
    CHECK(region["properties"]["x"]["minimum"] == 0);
    CHECK(region["properties"]["y"]["minimum"] == 0);
    CHECK(region["properties"]["width"]["type"] == "integer");
    CHECK(region["properties"]["height"]["type"] == "integer");
    CHECK(region["properties"]["width"]["minimum"] == 1);
    CHECK(region["properties"]["height"]["minimum"] == 1);
}

void testAnnotations()
{
    const ToolDefinition *info = sdl3mcp::findTool("sdl_info");
    const ToolDefinition *input = sdl3mcp::findTool("sdl_input");
    const ToolDefinition *capture = sdl3mcp::findTool("sdl_capture");
    CHECK(info != nullptr);
    CHECK(input != nullptr);
    CHECK(capture != nullptr);

    CHECK(info->annotations["readOnlyHint"] == true);
    CHECK(info->annotations["idempotentHint"] == true);
    CHECK(info->annotations["openWorldHint"] == false);

    CHECK(input->annotations["readOnlyHint"] == false);
    CHECK(input->annotations["idempotentHint"] == false);
    CHECK(input->annotations["openWorldHint"] == false);

    CHECK(capture->annotations["readOnlyHint"] == true);
    CHECK(capture->annotations["idempotentHint"] == false);
    CHECK(capture->annotations["openWorldHint"] == false);
}

void testToolCalls()
{
    Session session;
    initializeSession(session);

    {
        const SessionResult result = dispatch(
            session,
            R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"sdl_input","arguments":{"actions":[{"type":"key","key":"right","action":"down"}]}}})");
        CHECK(!result.response.has_value());
        CHECK(result.toolCall.has_value());
        const ToolCall &call = *result.toolCall;
        CHECK(call.id == 5);
        CHECK(call.name == "sdl_input");
        CHECK(call.arguments.is_object());
        CHECK(call.arguments["actions"].is_array());
        CHECK(call.arguments["actions"][0]["key"] == "right");
        CHECK(call.arguments["actions"][0]["action"] == "down");
    }
    {
        /* Absent arguments becomes an empty object, never null. */
        const SessionResult result = dispatch(
            session,
            R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"sdl_info"}})");
        CHECK(!result.response.has_value());
        CHECK(result.toolCall.has_value());
        CHECK(result.toolCall->id == 6);
        CHECK(result.toolCall->name == "sdl_info");
        CHECK(result.toolCall->arguments.is_object());
        CHECK(result.toolCall->arguments.empty());
    }
    {
        /* The request id keeps its JSON type. */
        const SessionResult result = dispatch(
            session,
            R"({"jsonrpc":"2.0","id":"cap","method":"tools/call","params":{"name":"sdl_capture","arguments":{"max_width":1280}}})");
        CHECK(result.toolCall.has_value());
        CHECK(result.toolCall->id.is_string());
        CHECK(result.toolCall->id == "cap");
        CHECK(result.toolCall->arguments["max_width"] == 1280);
    }
    {
        /* The session only dispatches; it never executes a tool. */
        const SessionResult result = dispatch(
            session,
            R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"sdl_capture","arguments":{}}})");
        CHECK(result.toolCall.has_value());
        CHECK(result.toolCall->arguments.is_object());
        CHECK(result.toolCall->arguments.empty());
    }
}

void testToolCallErrors()
{
    Session session;
    initializeSession(session);

    expectError(
        session,
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"sdl_nope"}})",
        RpcErrorCode::InvalidParams, 1);
    expectError(session,
                R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{}})",
                RpcErrorCode::InvalidParams, 2);
    expectError(
        session,
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":42}})",
        RpcErrorCode::InvalidParams, 3);
    expectError(
        session,
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":null}})",
        RpcErrorCode::InvalidParams, 4);
    expectError(
        session,
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"sdl_info","arguments":[1,2]}})",
        RpcErrorCode::InvalidParams, 5);
    expectError(
        session,
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"sdl_info","arguments":"x"}})",
        RpcErrorCode::InvalidParams, 6);
    expectError(
        session,
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"sdl_info","arguments":null}})",
        RpcErrorCode::InvalidParams, 7);

    /* A rejected call leaves the session usable. */
    const nlohmann::json result =
        resultOf(session, R"({"jsonrpc":"2.0","id":8,"method":"ping"})", 8);
    CHECK(result.is_object());
}

} // namespace

int main()
{
    testInitialize();
    testInitializeErrors();
    testPing();
    testToolsRequireInitialize();
    testNotifications();
    testUnknownMethod();
    testToolList();
    testInfoSchema();
    testInputSchema();
    testCaptureSchema();
    testAnnotations();
    testToolCalls();
    testToolCallErrors();
    return 0;
}
