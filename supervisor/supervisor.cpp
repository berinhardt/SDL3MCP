#include "supervisor.hpp"

#include <SDL3MCP/SDL3MCP_version.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <exception>
#include <string>
#include <utility>

#include <pthread.h>

namespace sdl3mcp
{
namespace
{

constexpr std::string_view kProtocolVersion = "2025-11-25";
constexpr std::string_view kServerName = "sdl3mcp-supervisor";
constexpr int kWindowPollMs = 50;

nlohmann::json textContent(std::string text)
{
    return nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", std::move(text)}},
    });
}

SupervisorToolResult toolError(std::string text)
{
    return SupervisorToolResult{textContent(std::move(text)), true};
}

SupervisorToolResult toolOk(std::string text)
{
    return SupervisorToolResult{textContent(std::move(text)), false};
}

nlohmann::json toolResultJson(const nlohmann::json &id, const SupervisorToolResult &result)
{
    return makeResult(id, nlohmann::json{
                              {"content", result.content},
                              {"isError", result.isError},
                          });
}

std::string stateName(SupervisorState state)
{
    switch (state) {
    case SupervisorState::Idle:
        return "idle";
    case SupervisorState::Starting:
        return "starting";
    case SupervisorState::Running:
        return "running";
    }
    return "idle";
}

std::string describeRpcError(const nlohmann::json &error)
{
    if (error.is_object()) {
        const auto message = error.find("message");
        if (message != error.end() && message->is_string()) {
            return message->get<std::string>();
        }
    }
    return error.dump();
}

/*
  Reads the window out of an sdl_info result. The text block is JSON, matching
  the library's own sdl_info, so the supervisor needs no SDL to understand it.
*/
bool windowFromInfo(const nlohmann::json &result, nlohmann::json &window)
{
    const auto content = result.find("content");
    if (content == result.end() || !content->is_array() || content->empty()) {
        return false;
    }

    const nlohmann::json &block = (*content)[0];
    const auto type = block.find("type");
    const auto text = block.find("text");
    if (type == block.end() || *type != "text" || text == block.end() ||
        !text->is_string()) {
        return false;
    }

    const nlohmann::json info =
        nlohmann::json::parse(text->get<std::string>(), nullptr, false);
    if (info.is_discarded() || !info.is_object()) {
        return false;
    }

    const auto entry = info.find("window");
    if (entry == info.end() || entry->is_null() || !entry->is_object()) {
        return false;
    }

    window = *entry;
    return true;
}

std::string describeWindow(const nlohmann::json &window)
{
    std::string title = "(untitled)";
    const auto titleEntry = window.find("title");
    if (titleEntry != window.end() && titleEntry->is_string()) {
        title = titleEntry->get<std::string>();
    }

    const auto dimension = [&window](const char *name) {
        const auto entry = window.find(name);
        return entry != window.end() && entry->is_number_integer()
                   ? entry->get<int>()
                   : 0;
    };

    const int width = dimension("width");
    const int height = dimension("height");

    std::string text = "spawned \"" + title + "\" (" + std::to_string(width) + "x" +
                       std::to_string(height) + " points";

    const int pixelWidth = dimension("pixelWidth");
    const int pixelHeight = dimension("pixelHeight");
    if (pixelWidth > 0 && pixelHeight > 0) {
        text += ", " + std::to_string(pixelWidth) + "x" + std::to_string(pixelHeight) +
                " pixels";
    }

    text += ")";
    return text;
}

} // namespace

nlohmann::json spawnToolDefinition()
{
    return nlohmann::json{
        {"name", "spawn"},
        {"description",
         "Start the application the supervisor was launched with and wait until it "
         "has a window. While it runs its own tools replace this one; call spawn "
         "again only after the application has exited."},
        {"inputSchema",
         nlohmann::json{
             {"type", "object"},
             {"properties", nlohmann::json::object()},
             {"additionalProperties", false},
         }},
        {"annotations",
         nlohmann::json{
             {"readOnlyHint", false},
             {"destructiveHint", false},
             {"idempotentHint", false},
             {"openWorldHint", false},
         }},
    };
}

nlohmann::json supervisorToolList(SupervisorState state, const nlohmann::json &childTools)
{
    switch (state) {
    case SupervisorState::Idle:
        return nlohmann::json{
            {"tools", nlohmann::json::array({spawnToolDefinition()})},
        };
    case SupervisorState::Starting:
        return nlohmann::json{{"tools", nlohmann::json::array()}};
    case SupervisorState::Running:
        return nlohmann::json{
            {"tools", childTools.is_array() ? childTools : nlohmann::json::array()},
        };
    }
    return nlohmann::json{{"tools", nlohmann::json::array()}};
}

Supervisor::Supervisor(int readFd, int writeFd, SupervisorOptions options)
    : transport_(readFd, writeFd), options_(std::move(options))
{
}

Supervisor::~Supervisor()
{
    shutdown();
}

int Supervisor::run()
{
    blockSigpipe();

    try {
        monitor_ = std::thread(&Supervisor::monitorLoop, this);
    } catch (const std::exception &) {
        failed_.store(true);
        shutdown();
        return 1;
    }

    for (;;) {
        std::string line;
        std::string error;
        const ReadStatus status = transport_.readLine(line, error);
        if (status == ReadStatus::Eof || status == ReadStatus::Stopped) {
            break;
        }
        if (status == ReadStatus::Error) {
            failed_.store(true);
            break;
        }
        if (status == ReadStatus::Timeout) {
            continue;
        }
        try {
            if (!handleLine(line)) {
                break;
            }
        } catch (const std::exception &) {
            /* An exception from a hostile or broken peer stops the supervisor,
               but only after shutdown() has stopped the child. */
            failed_.store(true);
            break;
        }
    }

    shutdown();
    return failed_.load() ? 1 : 0;
}

void Supervisor::stop()
{
    transport_.stop();
}

SupervisorState Supervisor::state() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return state_;
}

bool Supervisor::failed() const
{
    return failed_.load();
}

void Supervisor::blockSigpipe()
{
    /* Writes happen on the request and monitor threads, so both block it. */
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
}

int Supervisor::remainingMs(Deadline deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
        return 0;
    }
    /* Clamp so a huge --startup-timeout-ms cannot overflow the int we pass on. */
    return static_cast<int>(std::min<long long>(remaining, 600000));
}

void Supervisor::monitorLoop()
{
    blockSigpipe();

    for (;;) {
        ChildProcess *child = nullptr;
        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            monitorCondition_.wait(lock, [this] {
                return stopping_ ||
                       (state_ == SupervisorState::Running && child_ != nullptr);
            });
            if (stopping_) {
                return;
            }
            child = child_.get();
        }

        /* Blocks until this child exits; stop() makes it exit by terminating it. */
        child->waitForExit();

        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            if (stopping_) {
                return;
            }
            if (child_.get() != child || state_ != SupervisorState::Running) {
                /* A startup failure on the request thread already handled it. */
                continue;
            }
            state_ = SupervisorState::Idle;
            childTools_ = nlohmann::json::array();
        }

        sendToolsListChanged();
    }
}

void Supervisor::shutdown()
{
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        stopping_ = true;
    }
    monitorCondition_.notify_all();
    transport_.stop();

    if (child_) {
        child_->terminate();
    }
    if (monitor_.joinable()) {
        monitor_.join();
    }

    childTransport_.reset();
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        child_.reset();
        state_ = SupervisorState::Idle;
        childTools_ = nlohmann::json::array();
    }
}

bool Supervisor::handleLine(const std::string &line)
{
    RpcMessage message;
    nlohmann::json parseError;
    if (!parseMessage(line, message, parseError)) {
        return writeJson(parseError);
    }

    if (message.kind == RpcMessage::Kind::Notification) {
        return true;
    }

    handleRequest(message.request);
    return !failed_.load();
}

void Supervisor::handleRequest(const RpcRequest &request)
{
    if (request.method == "initialize") {
        writeJson(initializeResult(request.id, request.params));
        return;
    }
    if (request.method == "ping") {
        writeJson(makeResult(request.id, nlohmann::json::object()));
        return;
    }
    if (request.method == "tools/list") {
        handleToolsList(request.id);
        return;
    }
    if (request.method == "tools/call") {
        handleToolsCall(request.id, request.params);
        return;
    }

    writeJson(makeError(request.id, RpcErrorCode::MethodNotFound,
                        "Method not found: " + request.method));
}

nlohmann::json Supervisor::initializeResult(const nlohmann::json &id,
                                            const nlohmann::json &params)
{
    if (initialized_) {
        return makeError(id, RpcErrorCode::InvalidRequest,
                         "initialize was already called");
    }

    const auto version = params.find("protocolVersion");
    if (version == params.end() || !version->is_string()) {
        return makeError(id, RpcErrorCode::InvalidParams,
                         "initialize requires a string protocolVersion");
    }

    initialized_ = true;

    return makeResult(id, nlohmann::json{
                              {"protocolVersion", std::string(kProtocolVersion)},
                              {"capabilities", nlohmann::json{
                                   {"tools", nlohmann::json{{"listChanged", true}}},
                               }},
                              {"serverInfo", nlohmann::json{
                                   {"name", std::string(kServerName)},
                                   {"version", std::string(SDL3MCP_VERSION_STRING)},
                               }},
                          });
}

void Supervisor::handleToolsList(const nlohmann::json &id)
{
    if (!initialized_) {
        writeJson(makeError(id, RpcErrorCode::InvalidRequest,
                            "tools are unavailable before initialize"));
        return;
    }

    SupervisorState current = SupervisorState::Idle;
    nlohmann::json tools;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        current = state_;
        tools = childTools_;
    }

    writeJson(makeResult(id, supervisorToolList(current, tools)));
}

void Supervisor::handleToolsCall(const nlohmann::json &id, const nlohmann::json &params)
{
    if (!initialized_) {
        writeJson(makeError(id, RpcErrorCode::InvalidRequest,
                            "tools are unavailable before initialize"));
        return;
    }

    const auto name = params.find("name");
    if (name == params.end() || !name->is_string()) {
        writeJson(makeError(id, RpcErrorCode::InvalidParams,
                            "tools/call requires a string tool name"));
        return;
    }
    const std::string toolName = name->get<std::string>();

    nlohmann::json arguments = nlohmann::json::object();
    const auto supplied = params.find("arguments");
    if (supplied != params.end()) {
        if (!supplied->is_object()) {
            writeJson(makeError(id, RpcErrorCode::InvalidParams,
                                "tools/call arguments must be a JSON object"));
            return;
        }
        arguments = *supplied;
    }

    /*
      spawn always belongs to the supervisor: while the child runs this is also
      the short-circuit that keeps a forwarded "spawn" from reaching the app.
    */
    if (toolName == "spawn") {
        writeToolResult(id, spawn());
        return;
    }

    if (state() != SupervisorState::Running) {
        writeToolResult(id, toolError("the application is not running; call spawn first"));
        return;
    }

    writeJson(forwardChildCall(id, toolName, arguments));
}

SupervisorToolResult Supervisor::spawn()
{
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (state_ != SupervisorState::Idle) {
            return toolError("spawn is only valid while idle; the application is " +
                             stateName(state_) + ".");
        }
        state_ = SupervisorState::Starting;
        childTools_ = nlohmann::json::array();
    }
    sendToolsListChanged();

    const Deadline deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options_.startupTimeoutMs);

    nlohmann::json window = nlohmann::json::object();
    std::string failure;
    if (!startChild(deadline, window, failure)) {
        return failSpawn(failure);
    }

    return toolOk(describeWindow(window));
}

bool Supervisor::startChild(Deadline deadline, nlohmann::json &window, std::string &failure)
{
    auto child = std::make_unique<ChildProcess>();
    if (!child->start(ChildCommand{options_.target, options_.targetArguments}, failure)) {
        return false;
    }

    childTransport_.reset();
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        child_ = std::move(child);
    }
    ChildProcess *process = child_.get();

    const int execTimeout = std::max(remainingMs(deadline), 1);
    if (!process->waitForExec(execTimeout, failure)) {
        return false;
    }

    childTransport_ = std::make_unique<Transport>(process->stdoutFd(), process->stdinFd());

    std::string error;
    nlohmann::json ignored;
    const nlohmann::json initializeParams{
        {"protocolVersion", std::string(kProtocolVersion)},
        {"capabilities", nlohmann::json::object()},
        {"clientInfo", nlohmann::json{
             {"name", std::string(kServerName)},
             {"version", std::string(SDL3MCP_VERSION_STRING)},
         }},
    };
    if (!requestChild("initialize", initializeParams, ignored, deadline, error)) {
        failure = error;
        return false;
    }

    /* The child is now a server; a real MCP client acknowledges initialize. */
    if (!childTransport_->writeLine(nlohmann::json{
                                          {"jsonrpc", "2.0"},
                                          {"method", "notifications/initialized"},
                                      }
                                          .dump())) {
        failure = "cannot acknowledge initialize to the application";
        return false;
    }

    nlohmann::json listResult;
    if (!requestChild("tools/list", nlohmann::json::object(), listResult, deadline, error)) {
        failure = error;
        return false;
    }

    const auto tools = listResult.find("tools");
    if (tools == listResult.end() || !tools->is_array()) {
        failure = "the application answered tools/list without a tools array";
        return false;
    }

    for (;;) {
        if (process->hasExited()) {
            failure = "the application exited during startup with status " +
                      std::to_string(process->exitCode());
            return false;
        }

        const int remaining = remainingMs(deadline);
        if (remaining <= 0) {
            failure = "the application did not open a window within " +
                      std::to_string(options_.startupTimeoutMs) + " ms";
            return false;
        }

        nlohmann::json infoResult;
        const nlohmann::json infoRequest{
            {"name", "sdl_info"},
            {"arguments", nlohmann::json::object()},
        };
        if (!requestChild("tools/call", infoRequest, infoResult, deadline, error)) {
            failure = error;
            return false;
        }
        if (windowFromInfo(infoResult, window)) {
            break;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(kWindowPollMs, remaining)));
    }

    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        childTools_ = *tools;
        state_ = SupervisorState::Running;
    }
    monitorCondition_.notify_all();
    sendToolsListChanged();
    return true;
}

SupervisorToolResult Supervisor::failSpawn(const std::string &reason)
{
    std::string stderrTail;
    if (child_) {
        child_->terminate();
        stderrTail = child_->stderrTail();
    }

    childTransport_.reset();
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        child_.reset();
        state_ = SupervisorState::Idle;
        childTools_ = nlohmann::json::array();
    }
    monitorCondition_.notify_all();
    sendToolsListChanged();

    std::string text = "spawn failed: " + reason;
    if (stderrTail.empty()) {
        text += "\nthe application wrote nothing to stderr";
    } else {
        text += "\napplication stderr (last lines):\n" + stderrTail;
    }
    return toolError(std::move(text));
}

bool Supervisor::requestChild(std::string_view method,
                              const nlohmann::json &params,
                              nlohmann::json &result,
                              Deadline deadline,
                              std::string &error)
{
    error.clear();
    if (!childTransport_) {
        error = "the application is not running";
        return false;
    }

    const long long requestId = nextChildRequestId_++;
    const nlohmann::json request{
        {"jsonrpc", "2.0"},
        {"id", requestId},
        {"method", std::string(method)},
        {"params", params},
    };
    if (!childTransport_->writeLine(request.dump())) {
        error = "cannot write " + std::string(method) + " to the application";
        return false;
    }

    for (;;) {
        const int remaining = remainingMs(deadline);
        if (remaining <= 0) {
            error = "the application did not answer " + std::string(method) +
                    " before the startup timeout";
            return false;
        }

        std::string line;
        std::string readError;
        const ReadStatus status = childTransport_->readLine(line, readError, remaining);
        if (status == ReadStatus::Timeout) {
            error = "the application did not answer " + std::string(method) +
                    " before the startup timeout";
            return false;
        }
        if (status != ReadStatus::Line) {
            error = "the application closed its connection during " + std::string(method);
            return false;
        }

        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object() || message.contains("method")) {
            continue;
        }
        const auto id = message.find("id");
        if (id == message.end() || *id != requestId) {
            continue;
        }
        if (message.contains("error")) {
            error = "the application returned an error for " + std::string(method) + ": " +
                    describeRpcError(message["error"]);
            return false;
        }
        const auto payload = message.find("result");
        if (payload == message.end()) {
            error = "the application answered " + std::string(method) + " without a result";
            return false;
        }

        result = *payload;
        return true;
    }
}

nlohmann::json Supervisor::forwardChildCall(const nlohmann::json &id,
                                            const std::string &name,
                                            const nlohmann::json &arguments)
{
    if (!childTransport_) {
        return toolResultJson(id, toolError("the application is not running"));
    }

    const long long requestId = nextChildRequestId_++;
    const nlohmann::json request{
        {"jsonrpc", "2.0"},
        {"id", requestId},
        {"method", "tools/call"},
        {"params", nlohmann::json{{"name", name}, {"arguments", arguments}}},
    };
    if (!childTransport_->writeLine(request.dump())) {
        return toolResultJson(id, toolError("cannot reach the application"));
    }

    /*
      No deadline: the child's own tool timeouts bound the wait, and a child that
      dies closes the pipe, which is reported as a tool error.
    */
    for (;;) {
        std::string line;
        std::string error;
        const ReadStatus status = childTransport_->readLine(line, error);
        if (status != ReadStatus::Line) {
            return toolResultJson(
                id, toolError("the application closed its connection during " + name));
        }

        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object() || message.contains("method")) {
            continue;
        }
        const auto responseId = message.find("id");
        if (responseId == message.end() || *responseId != requestId) {
            continue;
        }

        /* A JSON-RPC error from the child travels back unchanged. */
        if (message.contains("error")) {
            return nlohmann::json{
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", message["error"]},
            };
        }

        const auto result = message.find("result");
        if (result == message.end() || !result->is_object()) {
            return toolResultJson(
                id, toolError("the application returned an invalid tools/call result"));
        }

        SupervisorToolResult forwarded;
        const auto content = result->find("content");
        forwarded.content =
            content != result->end() ? *content : nlohmann::json::array();
        const auto isError = result->find("isError");
        forwarded.isError =
            isError != result->end() && isError->is_boolean() && isError->get<bool>();

        return toolResultJson(id, forwarded);
    }
}

bool Supervisor::writeJson(const nlohmann::json &message)
{
    const std::lock_guard<std::mutex> lock(writeMutex_);
    /* Replace rather than throw: child-provided strings decide the encoding. */
    const std::string line =
        message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (transport_.writeLine(line)) {
        return true;
    }
    failed_.store(true);
    return false;
}

void Supervisor::sendToolsListChanged()
{
    writeJson(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"method", "notifications/tools/list_changed"},
    });
}

void Supervisor::writeToolResult(const nlohmann::json &id,
                                 const SupervisorToolResult &result)
{
    writeJson(toolResultJson(id, result));
}

} // namespace sdl3mcp
