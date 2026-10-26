#include "json_rpc.hpp"

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{

using sdl3mcp::parseMessage;
using sdl3mcp::RpcErrorCode;
using sdl3mcp::RpcMessage;

nlohmann::json errorCodeOf(const nlohmann::json &response)
{
    return response["error"]["code"];
}

/* A successful parse never leaves a stale error behind. */
void expectParsed(std::string_view line, RpcMessage &message)
{
    nlohmann::json errorResponse =
        sdl3mcp::makeError(nullptr, RpcErrorCode::InternalError, "stale");
    CHECK(parseMessage(line, message, errorResponse));
    CHECK(errorResponse.is_null());
}

void expectError(std::string_view line, RpcErrorCode code, const nlohmann::json &id)
{
    RpcMessage message;
    nlohmann::json errorResponse;
    CHECK(!parseMessage(line, message, errorResponse));
    CHECK(errorResponse.is_object());
    CHECK(errorResponse["jsonrpc"] == "2.0");
    CHECK(errorResponse.contains("error"));
    CHECK(!errorResponse.contains("result"));
    CHECK(errorCodeOf(errorResponse).get<int>() == static_cast<int>(code));
    CHECK(errorResponse["error"]["message"].is_string());
    CHECK(errorResponse["id"] == id);
}

void testVersionMacros()
{
    CHECK(NLOHMANN_JSON_VERSION_MAJOR == 3);
    CHECK(NLOHMANN_JSON_VERSION_MINOR == 12);
    CHECK(static_cast<int>(RpcErrorCode::ParseError) == -32700);
    CHECK(static_cast<int>(RpcErrorCode::InvalidRequest) == -32600);
    CHECK(static_cast<int>(RpcErrorCode::MethodNotFound) == -32601);
    CHECK(static_cast<int>(RpcErrorCode::InvalidParams) == -32602);
    CHECK(static_cast<int>(RpcErrorCode::InternalError) == -32603);
}

void testRequests()
{
    {
        RpcMessage message;
        expectParsed(
            R"({"jsonrpc":"2.0","id":"abc","method":"ping","params":{"verbose":true}})",
            message);
        CHECK(message.kind == RpcMessage::Kind::Request);
        CHECK(message.request.id.is_string());
        CHECK(message.request.id.get<std::string>() == "abc");
        CHECK(message.request.method == "ping");
        CHECK(message.request.params.is_object());
        CHECK(message.request.params["verbose"] == true);
    }
    {
        RpcMessage message;
        expectParsed(R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})", message);
        CHECK(message.kind == RpcMessage::Kind::Request);
        CHECK(message.request.id.is_number_integer());
        CHECK(message.request.id.get<int>() == 7);
        CHECK(message.request.method == "tools/list");
    }
    {
        RpcMessage message;
        expectParsed(
            R"({"jsonrpc":"2.0","id":9007199254740993,"method":"ping"})", message);
        CHECK(message.request.id.is_number_integer());
        CHECK(message.request.id.get<std::int64_t>() == 9007199254740993LL);
    }
    {
        RpcMessage message;
        expectParsed(R"({"jsonrpc":"2.0","id":1.5,"method":"ping"})", message);
        CHECK(message.request.id.is_number_float());
        CHECK(message.request.id.get<double>() == 1.5);
    }
    {
        /* Absent params becomes an empty object, never null. */
        RpcMessage message;
        expectParsed(R"({"jsonrpc":"2.0","id":1,"method":"ping"})", message);
        CHECK(message.request.params.is_object());
        CHECK(message.request.params.empty());
    }
    {
        RpcMessage message;
        expectParsed(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{}})", message);
        CHECK(message.request.params.is_object());
        CHECK(message.request.params.empty());
    }
    {
        /* Unknown fields are allowed; the layer is not a schema validator. */
        RpcMessage message;
        expectParsed(R"({"jsonrpc":"2.0","id":9,"method":"ping","meta":{"x":1},"foo":[1,2]})",
                     message);
        CHECK(message.kind == RpcMessage::Kind::Request);
        CHECK(message.request.id == 9);
        CHECK(message.request.method == "ping");
    }
    {
        RpcMessage message;
        expectParsed(" \r\n\t{\"jsonrpc\": \"2.0\", \"id\": 3, \"method\": \"ping\"}\r",
                     message);
        CHECK(message.kind == RpcMessage::Kind::Request);
        CHECK(message.request.id == 3);
    }
}

void testNotifications()
{
    {
        RpcMessage message;
        expectParsed(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", message);
        CHECK(message.kind == RpcMessage::Kind::Notification);
        CHECK(message.notification.method == "notifications/initialized");
        CHECK(message.notification.params.is_object());
        CHECK(message.notification.params.empty());
        CHECK(message.request.id.is_null());
        CHECK(message.request.method.empty());
    }
    {
        RpcMessage message;
        expectParsed(R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})",
                     message);
        CHECK(message.kind == RpcMessage::Kind::Notification);
        CHECK(message.notification.method == "notifications/initialized");
    }
}

void testParseErrors()
{
    expectError("{", RpcErrorCode::ParseError, nullptr);
    expectError("not json", RpcErrorCode::ParseError, nullptr);
    expectError(R"({"jsonrpc":"2.0",})", RpcErrorCode::ParseError, nullptr);
    expectError("", RpcErrorCode::ParseError, nullptr);
    expectError("   ", RpcErrorCode::ParseError, nullptr);
    expectError(" \t\r\n", RpcErrorCode::ParseError, nullptr);
}

void testInvalidRequests()
{
    /* MCP does not allow batches. */
    expectError("[]", RpcErrorCode::InvalidRequest, nullptr);
    expectError(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])",
                RpcErrorCode::InvalidRequest, nullptr);
    expectError("42", RpcErrorCode::InvalidRequest, nullptr);
    expectError("null", RpcErrorCode::InvalidRequest, nullptr);
    expectError("true", RpcErrorCode::InvalidRequest, nullptr);

    /* A valid id is echoed even when another field is wrong. */
    expectError(R"({"id":1,"method":"ping"})", RpcErrorCode::InvalidRequest, 1);
    expectError(R"({"jsonrpc":"1.0","id":"a","method":"ping"})",
                RpcErrorCode::InvalidRequest, "a");
    expectError(R"({"jsonrpc":2,"id":4,"method":"ping"})", RpcErrorCode::InvalidRequest, 4);
    expectError(R"({"jsonrpc":null,"id":5,"method":"ping"})",
                RpcErrorCode::InvalidRequest, 5);
    expectError(R"({"jsonrpc":"2.0","id":2})", RpcErrorCode::InvalidRequest, 2);
    expectError(R"({"jsonrpc":"2.0","id":2,"method":5})", RpcErrorCode::InvalidRequest, 2);
    expectError(R"({"jsonrpc":"2.0","id":2,"method":null})",
                RpcErrorCode::InvalidRequest, 2);
    expectError(R"({"jsonrpc":"2.0","id":2,"method":"ping","params":[]})",
                RpcErrorCode::InvalidRequest, 2);
    expectError(R"({"jsonrpc":"2.0","id":2,"method":"ping","params":null})",
                RpcErrorCode::InvalidRequest, 2);
    expectError(R"({"jsonrpc":"2.0","id":2,"method":"ping","params":"x"})",
                RpcErrorCode::InvalidRequest, 2);

    /* An id that cannot be echoed makes the error id null. */
    expectError(R"({"jsonrpc":"2.0","id":null,"method":"ping"})",
                RpcErrorCode::InvalidRequest, nullptr);
    expectError(R"({"jsonrpc":"2.0","id":false,"method":"ping"})",
                RpcErrorCode::InvalidRequest, nullptr);
    expectError(R"({"jsonrpc":"2.0","id":[1],"method":"ping"})",
                RpcErrorCode::InvalidRequest, nullptr);
    expectError(R"({"jsonrpc":"2.0","id":{"n":1},"method":"ping"})",
                RpcErrorCode::InvalidRequest, nullptr);
    expectError(R"({"jsonrpc":"2.0","id":{},"method":5})",
                RpcErrorCode::InvalidRequest, nullptr);

    /* Malformed notifications have no id to echo. */
    expectError(R"({"jsonrpc":"2.0","method":null})", RpcErrorCode::InvalidRequest, nullptr);
    expectError(R"({"jsonrpc":"2.0","method":"ping","params":3})",
                RpcErrorCode::InvalidRequest, nullptr);
    expectError(R"({"method":"ping"})", RpcErrorCode::InvalidRequest, nullptr);
}

void testMakeResult()
{
    {
        const nlohmann::json response =
            sdl3mcp::makeResult(42, nlohmann::json{{"ok", true}});
        CHECK(response.is_object());
        CHECK(response["jsonrpc"] == "2.0");
        CHECK(response["id"] == 42);
        CHECK(response["result"].is_object());
        CHECK(response["result"]["ok"] == true);
        CHECK(!response.contains("error"));
    }
    {
        /* The id keeps its JSON type. */
        const nlohmann::json response = sdl3mcp::makeResult("abc", nlohmann::json::array());
        CHECK(response["id"].is_string());
        CHECK(response["id"].get<std::string>() == "abc");
        CHECK(response["result"].is_array());
        CHECK(response["result"].empty());
    }
    {
        const nlohmann::json response = sdl3mcp::makeResult(1.5, nullptr);
        CHECK(response["id"].is_number_float());
        CHECK(response["result"].is_null());
    }
}

void testMakeError()
{
    {
        const nlohmann::json response =
            sdl3mcp::makeError("abc", RpcErrorCode::MethodNotFound, "no such method");
        CHECK(response.is_object());
        CHECK(response["jsonrpc"] == "2.0");
        CHECK(response["id"].is_string());
        CHECK(response["id"].get<std::string>() == "abc");
        CHECK(!response.contains("result"));
        CHECK(errorCodeOf(response).get<int>() == -32601);
        CHECK(response["error"]["message"].get<std::string>() == "no such method");
    }
    {
        const nlohmann::json response =
            sdl3mcp::makeError(7, RpcErrorCode::InternalError, "boom");
        CHECK(response["id"].is_number_integer());
        CHECK(response["id"].get<int>() == 7);
        CHECK(errorCodeOf(response).get<int>() == -32603);
        CHECK(!response.contains("result"));
    }
    {
        /* A null id is legal for errors that cannot be correlated. */
        const nlohmann::json response =
            sdl3mcp::makeError(nullptr, RpcErrorCode::ParseError, "bad");
        CHECK(response["id"].is_null());
        CHECK(errorCodeOf(response).get<int>() == -32700);
    }
}

} // namespace

int main()
{
    testVersionMacros();
    testRequests();
    testNotifications();
    testParseErrors();
    testInvalidRequests();
    testMakeResult();
    testMakeError();
    return 0;
}
