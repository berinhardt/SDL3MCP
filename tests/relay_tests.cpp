/*
  The relay end to end: the real executable is started with --listen on an
  ephemeral port and driven over real pipes, while a fake application dials in
  over TCP. That covers the CLI, the listener, the accept/single-session rule,
  the private initialize handshake, wait_session, the forwarded call path and
  the raw capture framing, in the same code the shipped binary runs.

  No SDL and no video driver are involved: neither side of the fake peer touches
  a window, so a failure is a protocol or socket bug rather than a driver one.
*/

#include "cli.hpp"
#include "socket_connect.hpp"
#include "supervisor.hpp"
#include "transport.hpp"

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using sdl3mcp::ReadStatus;
using sdl3mcp::Transport;

constexpr int kLineTimeoutMs = 5000;

/* A forked process with stdin/stdout/stderr pipes, closed and reaped on destruction. */
struct TestProcess
{
    pid_t pid = -1;
    int toChild = -1;
    int fromChild = -1;
    int stderrChild = -1;

    TestProcess() = default;
    TestProcess(const TestProcess &) = delete;
    TestProcess &operator=(const TestProcess &) = delete;

    ~TestProcess()
    {
        if (toChild >= 0) {
            ::close(toChild);
        }
        if (fromChild >= 0) {
            ::close(fromChild);
        }
        if (stderrChild >= 0) {
            ::close(stderrChild);
        }
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    void start(const std::string &executable, const std::vector<std::string> &arguments)
    {
        int inputPipe[2] = {-1, -1};
        int outputPipe[2] = {-1, -1};
        int errorPipe[2] = {-1, -1};
        CHECK(::pipe(inputPipe) == 0);
        CHECK(::pipe(outputPipe) == 0);
        CHECK(::pipe(errorPipe) == 0);

        const pid_t child = ::fork();
        CHECK(child >= 0);

        if (child == 0) {
            ::dup2(inputPipe[0], STDIN_FILENO);
            ::dup2(outputPipe[1], STDOUT_FILENO);
            ::dup2(errorPipe[1], STDERR_FILENO);
            ::close(inputPipe[0]);
            ::close(inputPipe[1]);
            ::close(outputPipe[0]);
            ::close(outputPipe[1]);
            ::close(errorPipe[0]);
            ::close(errorPipe[1]);

            std::vector<std::string> storage;
            storage.push_back(executable);
            for (const std::string &argument : arguments) {
                storage.push_back(argument);
            }
            std::vector<char *> argv;
            for (std::string &argument : storage) {
                argv.push_back(const_cast<char *>(argument.c_str()));
            }
            argv.push_back(nullptr);

            ::execv(executable.c_str(), argv.data());
            ::_exit(127);
        }

        pid = child;
        ::close(inputPipe[0]);
        ::close(outputPipe[1]);
        ::close(errorPipe[1]);
        toChild = inputPipe[1];
        fromChild = outputPipe[0];
        stderrChild = errorPipe[0];
    }

    /* Reads one newline-terminated line from the child's stderr. */
    std::string readStderrLine(int timeoutMs)
    {
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                return std::string();
            }

            pollfd descriptor{};
            descriptor.fd = stderrChild;
            descriptor.events = POLLIN;
            if (::poll(&descriptor, 1, static_cast<int>(remaining)) <= 0) {
                continue;
            }

            char chunk[256];
            const ssize_t count = ::read(stderrChild, chunk, sizeof chunk);
            if (count <= 0) {
                return std::string();
            }
            line.append(chunk, static_cast<size_t>(count));
            const std::size_t newline = line.find('\n');
            if (newline != std::string::npos) {
                return line.substr(0, newline);
            }
        }
    }

    void closeStdin()
    {
        if (toChild >= 0) {
            ::close(toChild);
            toChild = -1;
        }
    }

    bool waitForExit(int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        for (;;) {
            int status = 0;
            const pid_t result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD)) {
                pid = -1;
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(5ms);
        }
    }
};

/* The MCP client side of the relay, over the supervisor's stdin/stdout. */
struct RelayClient
{
    TestProcess process;
    std::unique_ptr<Transport> transport;
    std::vector<nlohmann::json> notifications;
    int nextId = 1;

    void start(const std::string &executable, std::uint16_t &port)
    {
        process.start(executable, {"--listen=127.0.0.1:0"});
        transport = std::make_unique<Transport>(process.fromChild, process.toChild);

        /* The bound endpoint is printed before serving; stderr is the diagnostics side. */
        const std::string line = process.readStderrLine(kLineTimeoutMs);
        const std::string prefix = "sdl3mcp: listening on ";
        CHECK(line.rfind(prefix, 0) == 0);
        const std::size_t separator = line.rfind(':');
        CHECK(separator != std::string::npos);
        const long parsed = std::strtol(line.c_str() + separator + 1, nullptr, 10);
        CHECK(parsed > 0 && parsed <= 65535);
        port = static_cast<std::uint16_t>(parsed);
    }

    nlohmann::json readMessage(int timeoutMs = kLineTimeoutMs)
    {
        std::string line;
        std::string error;
        const ReadStatus status = transport->readLine(line, error, timeoutMs);
        CHECK(status == ReadStatus::Line);

        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        CHECK(!message.is_discarded());
        return message;
    }

    /* Sends a request without waiting; the id is needed to read its response later. */
    int send(const std::string &method, nlohmann::json params = nlohmann::json::object())
    {
        const int id = nextId++;
        const nlohmann::json message{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", method},
            {"params", std::move(params)},
        };
        CHECK(transport->writeLine(message.dump()));
        return id;
    }

    nlohmann::json readResponse(int id, int timeoutMs = kLineTimeoutMs)
    {
        for (;;) {
            nlohmann::json received = readMessage(timeoutMs);
            if (received.contains("method")) {
                notifications.push_back(std::move(received));
                continue;
            }
            CHECK(received.contains("id"));
            CHECK(received["id"] == id);
            return received;
        }
    }

    nlohmann::json request(const std::string &method,
                           nlohmann::json params = nlohmann::json::object(),
                           int timeoutMs = kLineTimeoutMs)
    {
        return readResponse(send(method, std::move(params)), timeoutMs);
    }

    /* Reads one expected notification and remembers it for listChangedCount(). */
    nlohmann::json readNotification(const std::string &method,
                                    int timeoutMs = kLineTimeoutMs)
    {
        nlohmann::json message = readMessage(timeoutMs);
        CHECK(message.contains("method"));
        CHECK(message["method"] == method);
        notifications.push_back(message);
        return message;
    }

    nlohmann::json initialize()
    {
        return request("initialize", nlohmann::json{
                                         {"protocolVersion", "2025-11-25"},
                                         {"capabilities", nlohmann::json::object()},
                                         {"clientInfo", nlohmann::json{
                                              {"name", "relay-tests"},
                                              {"version", "0.0.0"},
                                          }},
                                     });
    }

    nlohmann::json toolsList() { return request("tools/list"); }

    nlohmann::json callTool(const std::string &name,
                            nlohmann::json arguments = nlohmann::json::object(),
                            int timeoutMs = kLineTimeoutMs)
    {
        return request("tools/call", nlohmann::json{
                                         {"name", name},
                                         {"arguments", std::move(arguments)},
                                     },
                       timeoutMs);
    }

    size_t listChangedCount() const
    {
        size_t count = 0;
        for (const nlohmann::json &message : notifications) {
            if (message.contains("method") &&
                message["method"] == "notifications/tools/list_changed") {
                ++count;
            }
        }
        return count;
    }
};

/* The application side: dials the relay and answers the supervisor's handshake. */
struct FakePeer
{
    int fd = -1;
    std::unique_ptr<Transport> transport;

    ~FakePeer()
    {
        if (transport) {
            transport->stop();
            transport.reset();
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }

    bool connect(std::uint16_t port)
    {
        std::string error;
        if (!sdl3mcp::connectSocket("127.0.0.1", port, fd, error)) {
            std::fprintf(stderr, "fake peer connect failed: %s\n", error.c_str());
            return false;
        }
        transport = std::make_unique<Transport>(fd, fd);
        return true;
    }

    nlohmann::json readMessage(int timeoutMs = kLineTimeoutMs)
    {
        std::string line;
        std::string error;
        const ReadStatus status = transport->readLine(line, error, timeoutMs);
        CHECK(status == ReadStatus::Line);
        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        CHECK(!message.is_discarded());
        return message;
    }

    void write(nlohmann::json message)
    {
        CHECK(transport->writeLine(message.dump()));
    }

    /* Answers the supervisor's private initialize, ack, and tools/list. */
    void handshake(const nlohmann::json &tools)
    {
        const nlohmann::json initialize = readMessage();
        CHECK(initialize["method"] == "initialize");
        CHECK(initialize.contains("id"));
        CHECK(initialize["params"]["protocolVersion"] == "2025-11-25");
        write(nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", initialize["id"]},
            {"result", nlohmann::json{
                 {"protocolVersion", "2025-11-25"},
                 {"capabilities", nlohmann::json{{"tools", nlohmann::json::object()}}},
                 {"serverInfo", nlohmann::json{
                      {"name", "fake-relay-peer"},
                      {"version", "0.0.0"},
                  }},
             }},
        });

        const nlohmann::json ack = readMessage();
        CHECK(ack["method"] == "notifications/initialized");

        const nlohmann::json list = readMessage();
        CHECK(list["method"] == "tools/list");
        write(nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", list["id"]},
            {"result", nlohmann::json{{"tools", tools}}},
        });
    }

    /* Answers one forwarded tools/call; the client reads the response afterwards. */
    void serveCall(const std::string &expectedName)
    {
        const nlohmann::json call = readMessage();
        CHECK(call["method"] == "tools/call");
        CHECK(call["params"]["name"] == expectedName);

        if (expectedName == "sdl_capture") {
            /* The library's raw framing: one header line, then exactly that many bytes. */
            constexpr int width = 2;
            constexpr int height = 2;
            constexpr size_t byteLength = width * height * 4;
            write(nlohmann::json{
                {"jsonrpc", "2.0"},
                {"id", call["id"]},
                {"result", nlohmann::json{
                     {"format", "rgba8888"},
                     {"width", width},
                     {"height", height},
                     {"byteLength", byteLength},
                 }},
            });
            std::vector<uint8_t> pixels(byteLength, 0x80);
            CHECK(transport->writeBytes(pixels.data(), pixels.size()));
            return;
        }

        write(nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", call["id"]},
            {"result", nlohmann::json{
                 {"content", nlohmann::json::array({
                                 nlohmann::json{{"type", "text"},
                                                {"text", "fake relay call ok"}},
                             })},
                 {"isError", false},
             }},
        });
    }
};

nlohmann::json peerToolList()
{
    return nlohmann::json::array({
        nlohmann::json{
            {"name", "app_info"},
            {"description", "fake relay tool"},
            {"inputSchema", nlohmann::json{{"type", "object"}}},
        },
        nlohmann::json{
            {"name", "sdl_capture"},
            {"description", "fake peer capture"},
            {"inputSchema", nlohmann::json{{"type", "object"}}},
        },
    });
}

std::vector<std::string> toolNames(const nlohmann::json &listResponse)
{
    CHECK(listResponse.contains("result"));
    const nlohmann::json &tools = listResponse["result"]["tools"];
    CHECK(tools.is_array());

    std::vector<std::string> names;
    for (const nlohmann::json &tool : tools) {
        names.push_back(tool.value("name", std::string()));
    }
    return names;
}

/* True when a fresh connection to an attached relay is closed instead of served. */
bool secondConnectionIsClosed(std::uint16_t port)
{
    int fd = -1;
    std::string error;
    if (!sdl3mcp::connectSocket("127.0.0.1", port, fd, error)) {
        std::fprintf(stderr, "second connect failed: %s\n", error.c_str());
        return false;
    }

    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, 3000);
    if (ready <= 0) {
        ::close(fd);
        return false;
    }

    char byte = 0;
    const ssize_t count = ::recv(fd, &byte, 1, 0);
    const bool closed = count == 0 || (count < 0 && errno == ECONNRESET);
    ::close(fd);
    return closed;
}

void testCommandLineRelay()
{
    const sdl3mcp::CliParseResult ephemeral =
        sdl3mcp::parseCommandLine({"--listen=127.0.0.1:0"});
    CHECK(ephemeral.ok);
    CHECK(ephemeral.options.relay());
    CHECK(ephemeral.options.listenHost == "127.0.0.1");
    CHECK(ephemeral.options.listenPort == 0);
    CHECK(ephemeral.options.target.empty());

    const sdl3mcp::CliParseResult ipv6 =
        sdl3mcp::parseCommandLine({"--listen=[::1]:4242"});
    CHECK(ipv6.ok);
    CHECK(ipv6.options.relay());
    CHECK(ipv6.options.listenHost == "::1");
    CHECK(ipv6.options.listenPort == 4242);

    CHECK(!sdl3mcp::parseCommandLine({"--listen"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--listen=127.0.0.1"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--listen=:7000"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--listen=127.0.0.1:70000"}).ok);
    /* --listen and --target are mutually exclusive until the spawner step defines it. */
    CHECK(!sdl3mcp::parseCommandLine({"--listen=127.0.0.1:0", "--target", "app"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({}).ok);
}

void testWaitSessionTimeout(const std::string &supervisor)
{
    std::uint16_t port = 0;
    RelayClient client;
    client.start(supervisor, port);
    CHECK(port != 0);
    CHECK(!client.initialize().contains("error"));

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"wait_session"}));

    const auto started = std::chrono::steady_clock::now();
    const nlohmann::json timeout = client.callTool(
        "wait_session", nlohmann::json{{"timeout_ms", 200}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    CHECK(!timeout.contains("error"));
    CHECK(timeout["result"]["isError"] == true);
    const std::string text = timeout["result"]["content"][0]["text"].get<std::string>();
    CHECK(text.find("200 ms") != std::string::npos);
    CHECK(elapsed >= 150);
    CHECK(elapsed < 5000);

    /* A bad timeout_ms is an argument error, not a hang. */
    const nlohmann::json invalid = client.callTool(
        "wait_session", nlohmann::json{{"timeout_ms", 0}});
    CHECK(invalid["result"]["isError"] == true);

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

void testRelayRoundTrip(const std::string &supervisor)
{
    std::uint16_t port = 0;
    RelayClient client;
    client.start(supervisor, port);
    CHECK(!client.initialize().contains("error"));
    CHECK(toolNames(client.toolsList()) == std::vector<std::string>{"wait_session"});

    /* wait_session with no timeout parks the supervisor until the app dials in. */
    const int waitId = client.send("tools/call", nlohmann::json{
                                                      {"name", "wait_session"},
                                                      {"arguments", nlohmann::json::object()},
                                                  });

    FakePeer peer;
    CHECK(peer.connect(port));
    peer.handshake(peerToolList());

    /* The attach is announced once and wakes the waiting call. */
    const nlohmann::json waited = client.readResponse(waitId);
    CHECK(waited["result"]["isError"] == false);
    CHECK(client.listChangedCount() == 1);

    const std::vector<std::string> attachedNames = toolNames(client.toolsList());
    CHECK((attachedNames == std::vector<std::string>{"app_info", "sdl_capture"}));

    /* sdl_capture is the supervisor's presentation, not the peer's. */
    const nlohmann::json list = client.toolsList();
    const nlohmann::json &capture = list["result"]["tools"][1];
    CHECK(capture["description"].get<std::string>().find("PNG") != std::string::npos);
    CHECK(capture["inputSchema"]["properties"].contains("region"));

    /* A forwarded call round-trips the app's answer unchanged. */
    const int callId = client.send("tools/call", nlohmann::json{
                                                      {"name", "app_info"},
                                                      {"arguments", nlohmann::json::object()},
                                                  });
    peer.serveCall("app_info");
    const nlohmann::json call = client.readResponse(callId);
    CHECK(call["result"]["isError"] == false);
    CHECK(call["result"]["content"][0]["text"] == "fake relay call ok");

    /* The raw capture framing becomes the usual PNG image block. */
    const int captureId = client.send("tools/call", nlohmann::json{
                                                          {"name", "sdl_capture"},
                                                          {"arguments", nlohmann::json::object()},
                                                      });
    peer.serveCall("sdl_capture");
    const nlohmann::json image = client.readResponse(captureId);
    CHECK(image["result"]["isError"] == false);
    const nlohmann::json &content = image["result"]["content"];
    CHECK(content.size() == 2);
    CHECK(content[0]["text"].get<std::string>().rfind("2x2 PNG", 0) == 0);
    CHECK(content[1]["type"] == "image");
    CHECK(content[1]["mimeType"] == "image/png");
    CHECK(content[1]["data"].get<std::string>().size() > 20);

    /* Exactly one peer: a second connection is closed, not queued. */
    CHECK(secondConnectionIsClosed(port));

    /* wait_session with a session attached returns rather than waiting. */
    const nlohmann::json already =
        client.callTool("wait_session", nlohmann::json::object());
    CHECK(already["result"]["isError"] == false);
    const std::string alreadyText =
        already["result"]["content"][0]["text"].get<std::string>();
    CHECK(alreadyText.find("already attached") != std::string::npos);

    /* The app goes away: the next message is the withdrawal, and wait_session is back. */
    peer.transport->stop();
    peer.transport.reset();
    ::close(peer.fd);
    peer.fd = -1;

    client.readNotification("notifications/tools/list_changed");
    CHECK(client.listChangedCount() == 2);
    CHECK(toolNames(client.toolsList()) == std::vector<std::string>{"wait_session"});

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

} // namespace

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    const std::string supervisor = argv[1];

    testCommandLineRelay();
    testWaitSessionTimeout(supervisor);
    testRelayRoundTrip(supervisor);

    std::printf("relay_tests: all checks passed\n");
    return 0;
}
