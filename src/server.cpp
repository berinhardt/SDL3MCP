#include "server.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <string_view>
#include <system_error>
#include <utility>

#include <pthread.h>

namespace sdl3mcp
{

Server::Server(int readFd,
               int writeFd,
               Session &session,
               ToolHandler handler,
               size_t maxLineBytes)
    : transport_(readFd, writeFd, maxLineBytes),
      session_(session),
      handler_(std::move(handler))
{
}

Server::~Server()
{
    stop();
    join();
}

bool Server::start(std::string &error)
{
    error.clear();
    if (started_) {
        error = "the server is already started";
        return false;
    }

    try {
        thread_ = std::thread(&Server::run, this);
    } catch (const std::system_error &exception) {
        error = std::string("cannot start the server thread: ") + exception.what();
        return false;
    }

    started_ = true;
    return true;
}

void Server::stop()
{
    transport_.stop();
}

void Server::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Server::failed() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

std::string Server::lastError() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void Server::run()
{
    blockSigpipe();

    for (;;) {
        std::string line;
        std::string error;
        const ReadStatus status = transport_.readLine(line, error);
        if (status == ReadStatus::Eof || status == ReadStatus::Stopped) {
            return;
        }
        if (status == ReadStatus::Error) {
            setFailure(error.empty() ? std::string("the transport could not be read") : error);
            return;
        }
        /* Only a bounded read can time out; the server reads indefinitely. */
        if (status == ReadStatus::Timeout) {
            continue;
        }

        try {
            if (!handleLine(line)) {
                return;
            }
        } catch (const std::exception &exception) {
            setFailure(std::string("the server failed while handling a message: ") + exception.what());
            return;
        } catch (...) {
            setFailure("the server failed while handling a message: unknown exception");
            return;
        }
    }
}

void Server::blockSigpipe()
{
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    /*
      The mask is per thread, so doing it here covers every write this thread
      makes, including those the handler might not expect. A closed pipe then
      surfaces as EPIPE from write() instead of killing the app.
    */
    (void)pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
}

bool Server::handleLine(std::string_view line)
{
    RpcMessage message;
    nlohmann::json parseError;
    if (!parseMessage(line, message, parseError)) {
        /* Malformed input is answered, not fatal: the session continues. */
        return writeResponse(parseError);
    }

    const SessionResult result = session_.handle(message);
    if (result.response) {
        return writeResponse(*result.response);
    }
    if (result.toolCall) {
        return writeToolCall(*result.toolCall);
    }
    /* A notification: nothing to write, nothing to do. */
    return true;
}

bool Server::writeResponse(const nlohmann::json &response)
{
    if (transport_.writeLine(response.dump())) {
        return true;
    }

    const int errorCode = errno;
    setFailure(std::string("cannot write to the client: ") + std::strerror(errorCode));
    return false;
}

bool Server::writeToolCall(const ToolCall &call)
{
    const ToolResult result = handler_(call);

    /*
      A raw frame is one JSON-RPC result line naming the payload, immediately
      followed by the pixels with no newline: the header's byteLength is the only
      framing the supervisor needs.
    */
    if (result.frame.has_value()) {
        const nlohmann::json header{
            {"format", "rgba8888"},
            {"width", result.frame->width},
            {"height", result.frame->height},
            {"byteLength", result.frame->pixels.size()},
        };
        if (!writeResponse(makeResult(call.id, header))) {
            return false;
        }
        if (!transport_.writeBytes(result.frame->pixels.data(),
                                   result.frame->pixels.size())) {
            const int errorCode = errno;
            setFailure(std::string("cannot write the frame payload to the client: ") +
                       std::strerror(errorCode));
            return false;
        }
        return true;
    }

    return writeResponse(makeResult(call.id, nlohmann::json{
                                             {"content", result.content},
                                             {"isError", result.isError},
                                         }));
}

void Server::setFailure(std::string message)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    lastError_ = std::move(message);
}

} // namespace sdl3mcp
