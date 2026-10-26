#include "json_rpc.hpp"

#include <utility>

namespace sdl3mcp
{
namespace
{

constexpr std::string_view kJsonRpcVersion = "2.0";

bool isBlank(std::string_view line) noexcept
{
    return line.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

bool isVersion20(const nlohmann::json &value)
{
    if (!value.is_string()) {
        return false;
    }
    const std::string_view version = value.get_ref<const std::string &>();
    return version == kJsonRpcVersion;
}

/* Only strings and numbers may travel as ids: the others cannot be echoed. */
bool isEchoableId(const nlohmann::json &id) noexcept
{
    return id.is_string() || id.is_number();
}

nlohmann::json reject(const nlohmann::json &id, std::string_view message)
{
    return makeError(id, RpcErrorCode::InvalidRequest, message);
}

} // namespace

bool parseMessage(std::string_view line,
                  RpcMessage &message,
                  nlohmann::json &errorResponse)
{
    if (isBlank(line)) {
        errorResponse = makeError(nlohmann::json(), RpcErrorCode::ParseError,
                                  "Parse error: empty input");
        return false;
    }

    const nlohmann::json document =
        nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
    if (document.is_discarded()) {
        errorResponse = makeError(nlohmann::json(), RpcErrorCode::ParseError,
                                  "Parse error: invalid JSON");
        return false;
    }
    if (document.is_array()) {
        errorResponse = reject(nlohmann::json(), "Batch requests are not supported");
        return false;
    }
    if (!document.is_object()) {
        errorResponse = reject(nlohmann::json(), "Request must be a JSON object");
        return false;
    }

    /*
      The id decides the error id, so it is validated first: a bad id must be
      reported as null even when the rest of the message is bad too.
    */
    nlohmann::json id;
    const auto idEntry = document.find("id");
    const bool isNotification = idEntry == document.end();
    if (!isNotification) {
        if (!isEchoableId(*idEntry)) {
            errorResponse = reject(nlohmann::json(), "Invalid request id");
            return false;
        }
        id = *idEntry;
    }

    const auto versionEntry = document.find("jsonrpc");
    if (versionEntry == document.end() || !isVersion20(*versionEntry)) {
        errorResponse = reject(id, "Missing or unsupported jsonrpc version");
        return false;
    }

    const auto methodEntry = document.find("method");
    if (methodEntry == document.end() || !methodEntry->is_string()) {
        errorResponse = reject(id, "Method must be a string");
        return false;
    }

    nlohmann::json params = nlohmann::json::object();
    const auto paramsEntry = document.find("params");
    if (paramsEntry != document.end()) {
        if (!paramsEntry->is_object()) {
            errorResponse = reject(id, "Params must be a JSON object");
            return false;
        }
        params = *paramsEntry;
    }

    const std::string method = methodEntry->get<std::string>();
    if (isNotification) {
        message.kind = RpcMessage::Kind::Notification;
        message.notification.method = method;
        message.notification.params = std::move(params);
        message.request = RpcRequest{};
    } else {
        message.kind = RpcMessage::Kind::Request;
        message.request.id = std::move(id);
        message.request.method = method;
        message.request.params = std::move(params);
        message.notification = RpcNotification{};
    }

    errorResponse = nlohmann::json();
    return true;
}

nlohmann::json makeResult(const nlohmann::json &id, nlohmann::json result)
{
    nlohmann::json response = nlohmann::json::object();
    response["jsonrpc"] = std::string(kJsonRpcVersion);
    response["id"] = id;
    response["result"] = std::move(result);
    return response;
}

nlohmann::json makeError(const nlohmann::json &id,
                         RpcErrorCode code,
                         std::string_view message)
{
    nlohmann::json response = nlohmann::json::object();
    response["jsonrpc"] = std::string(kJsonRpcVersion);
    response["id"] = id;
    response["error"] = nlohmann::json{
        {"code", static_cast<int>(code)},
        {"message", std::string(message)},
    };
    return response;
}

} // namespace sdl3mcp
