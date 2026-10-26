/*
  The server loop is exercised through real pipes rather than mocks: the tests
  write JSON-RPC lines into one pipe and read responses from another, so framing,
  polling, SIGPIPE handling, and the session dispatch are all covered by the same
  code that will run against stdin/stdout.

  A few checks drive Transport directly, because the server cannot observe them:
  CRLF stripping, the Stopped status, and the 50 ms poll wake-up after stop().
*/

#include "server.hpp"

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>

namespace
{

using sdl3mcp::ReadStatus;
using sdl3mcp::Server;
using sdl3mcp::Session;
using sdl3mcp::ToolCall;
using sdl3mcp::ToolResult;
using sdl3mcp::Transport;

constexpr int kPollTimeoutMs = 5000;
constexpr int kFailureTimeoutMs = 2000;

/* A pipe pair that closes both ends on destruction. */
struct Pipe
{
    int readFd = -1;
    int writeFd = -1;

    Pipe()
    {
        int descriptors[2] = {-1, -1};
        CHECK(::pipe(descriptors) == 0);
        readFd = descriptors[0];
        writeFd = descriptors[1];
    }

    ~Pipe()
    {
        closeRead();
        closeWrite();
    }

    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;

    void closeRead()
    {
        if (readFd >= 0) {
            ::close(readFd);
            readFd = -1;
        }
    }

    void closeWrite()
    {
        if (writeFd >= 0) {
            ::close(writeFd);
            writeFd = -1;
        }
    }
};

void writeAll(int fd, std::string_view data)
{
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(fd, data.data() + written, data.size() - written);
        CHECK(count > 0);
        written += static_cast<size_t>(count);
    }
}

/*
  The handler stands in for tool execution. It echoes the tool name so the test
  can prove the call reached it and that the id travelled unchanged.
*/
ToolResult echoHandler(const ToolCall &call)
{
    return ToolResult{
        nlohmann::json::array({
            nlohmann::json{{"type", "text"}, {"text", call.name}},
        }),
        false,
    };
}

struct Harness
{
    Pipe toServer;
    Pipe fromServer;
    Session session;
    Server server;

    explicit Harness(size_t maxLineBytes = 1024 * 1024)
        : server(toServer.readFd, fromServer.writeFd, session, echoHandler, maxLineBytes)
    {
        std::string error;
        CHECK(server.start(error));
        CHECK(error.empty());
    }

    ~Harness()
    {
        server.stop();
        server.join();
    }

    Harness(const Harness &) = delete;
    Harness &operator=(const Harness &) = delete;

    void send(std::string_view line)
    {
        writeAll(toServer.writeFd, std::string(line) + "\n");
    }

    /* Reads one newline-terminated line, blocking for at most kPollTimeoutMs. */
    nlohmann::json receive()
    {
        for (;;) {
            const size_t newline = pending_.find('\n');
            if (newline != std::string::npos) {
                const std::string line = pending_.substr(0, newline);
                pending_.erase(0, newline + 1);
                return nlohmann::json::parse(line);
            }

            pollfd descriptor{};
            descriptor.fd = fromServer.readFd;
            descriptor.events = POLLIN;
            const int ready = ::poll(&descriptor, 1, kPollTimeoutMs);
            CHECK(ready > 0);

            char chunk[4096];
            const ssize_t count = ::read(fromServer.readFd, chunk, sizeof chunk);
            CHECK(count > 0);
            pending_.append(chunk, static_cast<size_t>(count));
        }
    }

    void initialize()
    {
        send(R"({"jsonrpc":"2.0","id":0,"method":"initialize",)"
             R"("params":{"protocolVersion":"2025-11-25"}})");
        CHECK(receive()["id"] == 0);
    }

private:
    std::string pending_;
};

/* Polls failed() so a stuck server shows up as a failed check, not a hang. */
void waitForFailure(const Server &server, int timeoutMs = kFailureTimeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.failed()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(server.failed());
}

void testInitialize()
{
    Harness harness;
    harness.send(R"({"jsonrpc":"2.0","id":1,"method":"initialize",)"
                 R"("params":{"protocolVersion":"2025-11-25"}})");

    const nlohmann::json response = harness.receive();
    CHECK(response["jsonrpc"] == "2.0");
    CHECK(response["id"] == 1);
    CHECK(!response.contains("error"));

    const nlohmann::json &result = response["result"];
    CHECK(result["protocolVersion"] == "2025-11-25");
    CHECK(result["serverInfo"]["name"] == "sdl3mcp");
    CHECK(result["serverInfo"]["version"].is_string());
    CHECK(result["capabilities"].contains("tools"));
    CHECK(!harness.server.failed());
}

void testNotificationsAndToolList()
{
    Harness harness;
    harness.initialize();

    /* A notification produces no response: the next line read must be the ping. */
    harness.send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    harness.send(R"({"jsonrpc":"2.0","id":3,"method":"ping"})");

    const nlohmann::json ping = harness.receive();
    CHECK(ping["id"] == 3);
    CHECK(ping["result"] == nlohmann::json::object());

    harness.send(R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})");
    const nlohmann::json list = harness.receive();
    CHECK(list["id"] == 4);

    const nlohmann::json &tools = list["result"]["tools"];
    CHECK(tools.is_array());
    CHECK(tools.size() == 3);
    CHECK(tools[0]["name"] == "sdl_info");
    CHECK(tools[1]["name"] == "sdl_input");
    CHECK(tools[2]["name"] == "sdl_capture");
    CHECK(!harness.server.failed());
}

void testInvalidInputThenContinues()
{
    Harness harness;
    harness.initialize();

    harness.send(R"({"jsonrpc":"2.0","id":5,)");
    const nlohmann::json parseError = harness.receive();
    CHECK(parseError["id"].is_null());
    CHECK(parseError["error"]["code"].get<int>() == -32700);

    harness.send(R"({"id":6,"method":"ping"})");
    const nlohmann::json requestError = harness.receive();
    CHECK(requestError["id"] == 6);
    CHECK(requestError["error"]["code"].get<int>() == -32600);

    /* Both errors were answered; the session is still alive. */
    harness.send(R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
    const nlohmann::json ping = harness.receive();
    CHECK(ping["id"] == 7);
    CHECK(ping["result"] == nlohmann::json::object());
    CHECK(!harness.server.failed());
}

void testUnknownMethod()
{
    Harness harness;
    harness.initialize();

    harness.send(R"({"jsonrpc":"2.0","id":8,"method":"tools/unknown"})");
    const nlohmann::json response = harness.receive();
    CHECK(response["id"] == 8);
    CHECK(response["error"]["code"].get<int>() == -32601);
    CHECK(!harness.server.failed());
}

void testToolCall()
{
    Harness harness;
    harness.initialize();

    harness.send(R"({"jsonrpc":"2.0","id":9,"method":"tools/call",)"
                 R"("params":{"name":"sdl_input","arguments":{}}})");

    const nlohmann::json response = harness.receive();
    CHECK(response["id"] == 9);
    CHECK(!response.contains("error"));

    const nlohmann::json &result = response["result"];
    CHECK(result["isError"] == false);
    CHECK(result["content"].is_array());
    CHECK(result["content"].size() == 1);
    CHECK(result["content"][0]["type"] == "text");
    CHECK(result["content"][0]["text"] == "sdl_input");
    CHECK(!harness.server.failed());
}

void testEofExitsCleanly()
{
    Harness harness;
    harness.initialize();

    harness.toServer.closeWrite();
    harness.server.join();

    CHECK(!harness.server.failed());
    CHECK(harness.server.lastError().empty());
}

void testLineLimit()
{
    constexpr size_t kMaxLineBytes = 64;
    Harness harness(kMaxLineBytes);

    /* No newline ever arrives, so only the cap can end the read. */
    writeAll(harness.toServer.writeFd, std::string(kMaxLineBytes * 2, 'a'));
    waitForFailure(harness.server);
    harness.server.join();

    const std::string error = harness.server.lastError();
    CHECK(error.find("limit") != std::string::npos);
    CHECK(error.find("64") != std::string::npos);
}

void testWriteFailure()
{
    Harness harness;
    harness.initialize();

    /* The client is gone: the next write is EPIPE, which must not kill us. */
    harness.fromServer.closeRead();
    harness.send(R"({"jsonrpc":"2.0","id":10,"method":"ping"})");

    waitForFailure(harness.server);
    harness.server.join();
    CHECK(!harness.server.lastError().empty());
}

void testTransportFraming()
{
    Pipe input;
    Pipe output;
    Transport transport(input.readFd, output.writeFd);

    writeAll(input.writeFd, "first\r\nsecond\n");
    std::string line;
    std::string error;

    CHECK(transport.readLine(line, error) == ReadStatus::Line);
    CHECK(line == "first");
    CHECK(error.empty());

    CHECK(transport.readLine(line, error) == ReadStatus::Line);
    CHECK(line == "second");

    CHECK(transport.writeLine("hello"));
    char written[16] = {};
    CHECK(::read(output.readFd, written, sizeof written) == 6);
    CHECK(std::string_view(written, 6) == "hello\n");
}

void testTransportEofAndStop()
{
    Pipe input;
    Pipe output;

    {
        Transport transport(input.readFd, output.writeFd);
        input.closeWrite();

        std::string line;
        std::string error;
        CHECK(transport.readLine(line, error) == ReadStatus::Eof);
        CHECK(error.empty());
    }

    /* An idle reader must notice stop() at the next poll tick, not at EOF. */
    Pipe idleInput;
    Pipe idleOutput;
    Transport transport(idleInput.readFd, idleOutput.writeFd);

    ReadStatus status = ReadStatus::Line;
    std::thread reader([&transport, &status] {
        std::string line;
        std::string error;
        status = transport.readLine(line, error);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto stoppedAt = std::chrono::steady_clock::now();
    transport.stop();
    reader.join();
    const auto elapsed = std::chrono::steady_clock::now() - stoppedAt;

    CHECK(transport.stopped());
    CHECK(status == ReadStatus::Stopped);
    CHECK(elapsed < std::chrono::milliseconds(500));
}

} // namespace

int main()
{
    testTransportFraming();
    testTransportEofAndStop();

    testInitialize();
    testNotificationsAndToolList();
    testInvalidInputThenContinues();
    testUnknownMethod();
    testToolCall();
    testEofExitsCleanly();
    testLineLimit();
    testWriteFailure();

    std::puts("server_tests passed");
    return 0;
}
