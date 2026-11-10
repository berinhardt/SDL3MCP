#include "responder.hpp"

#include <cerrno>
#include <cstring>
#include <string_view>
#include <utility>

namespace sdl3mcp
{

Responder::Responder(ByteSink &sink, Session &session, ToolHandler handler)
    : sink_(sink),
      session_(session),
      handler_(std::move(handler))
{
}

bool Responder::handleLine(std::string_view line, std::string &error)
{
    RpcMessage message;
    nlohmann::json parseError;
    if (!parseMessage(line, message, parseError)) {
        /* Malformed input is answered, not fatal: the session continues. */
        return writeResponse(parseError, error);
    }

    const SessionResult result = session_.handle(message);
    if (result.response) {
        return writeResponse(*result.response, error);
    }
    if (result.toolCall) {
        return writeToolCall(*result.toolCall, error);
    }
    /* A notification: nothing to write, nothing to do. */
    return true;
}

bool Responder::writeResponse(const nlohmann::json &response, std::string &error)
{
    if (sink_.writeLine(response.dump())) {
        return true;
    }

    const int errorCode = errno;
    error = std::string("cannot write to the client: ") + std::strerror(errorCode);
    return false;
}

bool Responder::writeToolCall(const ToolCall &call, std::string &error)
{
    const ToolResult result = handler_(call);

    /*
      A raw frame is one JSON-RPC result line naming the payload, immediately
      followed by the pixels with no newline: the header's byteLength is the only
      framing the supervisor needs. `text` names the frame for tools that report
      one (sdl_input); sdl_capture leaves it out.
    */
    if (result.frame.has_value()) {
        nlohmann::json header{
            {"format", "rgba8888"},
            {"width", result.frame->width},
            {"height", result.frame->height},
            {"byteLength", result.frame->pixels.size()},
            {"frame", result.frame->frame},
        };
        if (!result.frameText.empty()) {
            header["text"] = result.frameText;
        }
        if (!writeResponse(makeResult(call.id, header), error)) {
            return false;
        }
        if (!sink_.writeBytes(result.frame->pixels.data(),
                              result.frame->pixels.size())) {
            const int errorCode = errno;
            error = std::string("cannot write the frame payload to the client: ") +
                    std::strerror(errorCode);
            return false;
        }
        return true;
    }

    return writeResponse(makeResult(call.id, nlohmann::json{
                                             {"content", result.content},
                                             {"isError", result.isError},
                                         }),
                         error);
}

} // namespace sdl3mcp
