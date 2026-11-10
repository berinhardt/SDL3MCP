#include "websocket_peer.hpp"

#include "responder.hpp"

#include <cerrno>
#include <cstring>
#include <string_view>
#include <utility>

namespace sdl3mcp
{
namespace
{

/* Transport's default cap: one line may not grow our memory without end. */
constexpr size_t kMaxLineBytes = 1024 * 1024;

constexpr unsigned short kNormalCloseCode = 1000;

std::string withResult(std::string_view action, int result)
{
    return std::string(action) + " (result " + std::to_string(result) + ")";
}

} // namespace

WebSocketPeer::~WebSocketPeer()
{
    releaseSocket();
}

bool WebSocketPeer::open(std::string_view host, std::uint16_t port, std::string &error)
{
    error.clear();
    if (socket_ != 0) {
        error = "the WebSocket peer is already open";
        return false;
    }
    if (!emscripten_websocket_is_supported()) {
        error = "this browser does not support WebSockets";
        return false;
    }

    std::string url("ws://");
    url.append(host);
    url.push_back(':');
    url.append(std::to_string(port));

    EmscriptenWebSocketCreateAttributes attributes;
    emscripten_websocket_init_create_attributes(&attributes);
    /* The url is read during the call itself; the std::string outlives it. */
    attributes.url = url.c_str();
    /*
      The creating thread is the browser's main thread, so the socket stays on
      it: the callbacks and pump() share that thread and the queue stays
      lock-free.
    */
    attributes.createOnMainThread = false;

    const EMSCRIPTEN_WEBSOCKET_T socket = emscripten_websocket_new(&attributes);
    if (socket <= 0) {
        error = withResult("the WebSocket for '" + url + "' could not be created", socket);
        return false;
    }
    socket_ = socket;

    EMSCRIPTEN_RESULT registration =
        emscripten_websocket_set_onmessage_callback(socket_, this, &WebSocketPeer::onMessage);
    if (registration == EMSCRIPTEN_RESULT_SUCCESS) {
        registration =
            emscripten_websocket_set_onclose_callback(socket_, this, &WebSocketPeer::onClose);
    }
    if (registration == EMSCRIPTEN_RESULT_SUCCESS) {
        registration =
            emscripten_websocket_set_onerror_callback(socket_, this, &WebSocketPeer::onError);
    }
    if (registration != EMSCRIPTEN_RESULT_SUCCESS) {
        /* No callback has run yet, so releasing the socket here is safe. */
        releaseSocket();
        error = withResult("the WebSocket callbacks could not be registered",
                           static_cast<int>(registration));
        return false;
    }
    return true;
}

bool WebSocketPeer::pump(Responder &responder, std::string &error)
{
    error.clear();
    if (failed_) {
        error = lastError_;
        return false;
    }

    for (;;) {
        const size_t newline = incoming_.find('\n');
        if (newline == std::string::npos) {
            /* A request split across frames is still one line: keep the fragment. */
            if (incoming_.size() > kMaxLineBytes) {
                return failLineLimit(error);
            }
            return true;
        }
        if (newline > kMaxLineBytes) {
            return failLineLimit(error);
        }

        std::string line = incoming_.substr(0, newline);
        incoming_.erase(0, newline + 1);
        /* One trailing '\r' is framing, not content: accept CRLF clients. */
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!responder.handleLine(line, error)) {
            return false;
        }
    }
}

bool WebSocketPeer::writeLine(std::string_view line)
{
    std::string framed;
    framed.reserve(line.size() + 1);
    framed.append(line);
    framed.push_back('\n');
    return writeBytes(reinterpret_cast<const uint8_t *>(framed.data()), framed.size());
}

bool WebSocketPeer::writeBytes(const uint8_t *data, size_t size)
{
    if (size == 0) {
        return true;
    }
    if (data == nullptr) {
        errno = EINVAL;
        return false;
    }
    if (socket_ == 0) {
        return failWrite("the WebSocket peer is closed");
    }
    /* The browser copies the bytes into its own buffer before the call returns. */
    const EMSCRIPTEN_RESULT result = emscripten_websocket_send_binary(
        socket_, const_cast<uint8_t *>(data), static_cast<uint32_t>(size));
    if (result != EMSCRIPTEN_RESULT_SUCCESS) {
        return failWrite(withResult("the WebSocket send failed", static_cast<int>(result)));
    }
    return true;
}

void WebSocketPeer::stop()
{
    releaseSocket();
}

bool WebSocketPeer::failed() const
{
    return failed_;
}

std::string WebSocketPeer::lastError() const
{
    return lastError_;
}

bool WebSocketPeer::onMessage(int, const EmscriptenWebSocketMessageEvent *event, void *userData)
{
    auto *peer = static_cast<WebSocketPeer *>(userData);
    if (peer != nullptr && event != nullptr) {
        peer->appendBytes(event->data, event->numBytes, event->isText);
    }
    /* The browser ignores the return value; the peer's state carries the verdict. */
    return true;
}

bool WebSocketPeer::onClose(int, const EmscriptenWebSocketCloseEvent *event, void *userData)
{
    auto *peer = static_cast<WebSocketPeer *>(userData);
    if (peer == nullptr) {
        return true;
    }

    std::string message = "the WebSocket closed";
    if (event != nullptr) {
        message += " with code " + std::to_string(event->code);
        /*
          Emscripten truncates the reason into a fixed char[512] without promising
          a terminator, so the read is bounded by the field, not by a search.
        */
        const size_t reasonLength = strnlen(event->reason, sizeof event->reason);
        if (reasonLength > 0) {
            message += ": ";
            message.append(event->reason, reasonLength);
        }
    }
    peer->fail(std::move(message));
    return true;
}

bool WebSocketPeer::onError(int, const EmscriptenWebSocketErrorEvent *, void *userData)
{
    auto *peer = static_cast<WebSocketPeer *>(userData);
    if (peer != nullptr) {
        peer->fail("the WebSocket reported an error");
    }
    return true;
}

void WebSocketPeer::appendBytes(const uint8_t *data, size_t size, bool isText)
{
    if (failed_ || size == 0 || data == nullptr) {
        return;
    }
    /*
      Emscripten's text path counts the terminating NUL in numBytes, and a text
      frame still carries the same UTF-8 line stream: drop the NUL, not the data.
    */
    if (isText && data[size - 1] == '\0') {
        --size;
    }
    incoming_.append(reinterpret_cast<const char *>(data), size);

    /*
      pump() runs once per frame, so a peer that never sends a newline could pile
      frames up between two calls: fail as soon as the pending fragment is over
      the cap instead of buffering without a bound.
    */
    const size_t lastNewline = incoming_.rfind('\n');
    const size_t pending =
        lastNewline == std::string::npos ? incoming_.size() : incoming_.size() - lastNewline - 1;
    if (pending > kMaxLineBytes) {
        incoming_.clear();
        fail(lineLimitError());
    }
}

bool WebSocketPeer::fail(std::string message)
{
    /* The first failure is the cause; everything after it is a consequence. */
    if (!failed_) {
        failed_ = true;
        lastError_ = std::move(message);
    }
    return false;
}

bool WebSocketPeer::failWrite(std::string message)
{
    /*
      Responders word write errors with strerror(errno), as they do for the fd
      transport, so a browser send failure reports the same "Broken pipe" a
      desktop peer does rather than whatever errno the last call left behind.
    */
    errno = EPIPE;
    return fail(std::move(message));
}

bool WebSocketPeer::failLineLimit(std::string &error)
{
    /* Transport drops the buffer on the same violation: the framing is lost. */
    incoming_.clear();
    fail(lineLimitError());
    error = lastError_;
    return false;
}

void WebSocketPeer::releaseSocket()
{
    if (socket_ == 0) {
        return;
    }
    /*
      close() lets the browser flush what it already queued; delete() detaches
      the callbacks first, so nothing reaches this peer after stop().
    */
    emscripten_websocket_close(socket_, kNormalCloseCode, "");
    emscripten_websocket_delete(socket_);
    socket_ = 0;
}

std::string WebSocketPeer::lineLimitError() const
{
    return "input line exceeds the " + std::to_string(kMaxLineBytes) + " byte limit";
}

} // namespace sdl3mcp
