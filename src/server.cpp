#include "server.hpp"

#include <csignal>
#include <exception>
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
      responder_(transport_, session, std::move(handler))
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
            std::string handleError;
            if (!responder_.handleLine(line, handleError)) {
                setFailure(std::move(handleError));
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

void Server::setFailure(std::string message)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    lastError_ = std::move(message);
}

} // namespace sdl3mcp
