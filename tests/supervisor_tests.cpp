/*
  The supervisor end to end: the real executable is started as a child of the
  test, driven over real pipes with per-line timeouts, and pointed at the fake
  MCP child. That covers the CLI, the line loop, the state machine, fork/exec,
  the proxy, and shutdown in the same code the shipped binary runs.

  The test says nothing about SDL: neither side initialises video, so a failure
  is a protocol or process bug, not a driver problem.
*/

#include "cli.hpp"
#include "supervisor.hpp"
#include "transport.hpp"

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
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
using sdl3mcp::SupervisorState;
using sdl3mcp::Transport;

constexpr int kLineTimeoutMs = 5000;

/* A forked process with a line channel, closed and reaped on destruction. */
struct TestProcess
{
    pid_t pid = -1;
    int toChild = -1;
    int fromChild = -1;

    ~TestProcess()
    {
        if (toChild >= 0) {
            ::close(toChild);
        }
        if (fromChild >= 0) {
            ::close(fromChild);
        }
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    TestProcess() = default;
    TestProcess(const TestProcess &) = delete;
    TestProcess &operator=(const TestProcess &) = delete;

    void start(const std::string &executable, const std::vector<std::string> &arguments)
    {
        int inputPipe[2] = {-1, -1};
        int outputPipe[2] = {-1, -1};
        CHECK(::pipe(inputPipe) == 0);
        CHECK(::pipe(outputPipe) == 0);

        const pid_t child = ::fork();
        CHECK(child >= 0);

        if (child == 0) {
            ::dup2(inputPipe[0], STDIN_FILENO);
            ::dup2(outputPipe[1], STDOUT_FILENO);
            ::close(inputPipe[0]);
            ::close(inputPipe[1]);
            ::close(outputPipe[0]);
            ::close(outputPipe[1]);

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
        toChild = inputPipe[1];
        fromChild = outputPipe[0];
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

struct SupervisorClient
{
    TestProcess process;
    std::unique_ptr<Transport> transport;
    std::vector<nlohmann::json> notifications;
    int nextId = 1;

    void start(const std::string &executable, const std::vector<std::string> &arguments)
    {
        process.start(executable, arguments);
        transport = std::make_unique<Transport>(process.fromChild, process.toChild);
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

    /* Sends a request and returns its response, collecting notifications. */
    nlohmann::json request(const std::string &method,
                           nlohmann::json params = nlohmann::json::object())
    {
        const int id = nextId++;
        const nlohmann::json message{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", method},
            {"params", std::move(params)},
        };
        CHECK(transport->writeLine(message.dump()));

        for (;;) {
            nlohmann::json received = readMessage();
            if (received.contains("method")) {
                notifications.push_back(std::move(received));
                continue;
            }
            CHECK(received.contains("id"));
            CHECK(received["id"] == id);
            return received;
        }
    }

    nlohmann::json initialize()
    {
        return request("initialize", nlohmann::json{
                                         {"protocolVersion", "2025-11-25"},
                                         {"capabilities", nlohmann::json::object()},
                                         {"clientInfo", nlohmann::json{
                                              {"name", "supervisor-tests"},
                                              {"version", "0.0.0"},
                                          }},
                                     });
    }

    nlohmann::json toolsList() { return request("tools/list"); }

    nlohmann::json callTool(const std::string &name,
                            nlohmann::json arguments = nlohmann::json::object())
    {
        return request("tools/call", nlohmann::json{
                                         {"name", name},
                                         {"arguments", std::move(arguments)},
                                     });
    }

    nlohmann::json spawn() { return callTool("spawn"); }

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

std::string spawnText(const nlohmann::json &spawnResponse)
{
    CHECK(spawnResponse.contains("result"));
    CHECK(spawnResponse["result"].contains("content"));
    const nlohmann::json &content = spawnResponse["result"]["content"];
    CHECK(content.is_array());
    CHECK(!content.empty());
    CHECK(content[0].contains("text"));
    return content[0]["text"].get<std::string>();
}

std::string makeTempPath()
{
    char pattern[] = "/tmp/sdl3mcp-supervisor-test-XXXXXX";
    const int descriptor = ::mkstemp(pattern);
    CHECK(descriptor >= 0);
    ::close(descriptor);
    ::unlink(pattern);
    return std::string(pattern);
}

pid_t readPidFile(const std::string &path)
{
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    for (;;) {
        std::FILE *file = std::fopen(path.c_str(), "r");
        if (file != nullptr) {
            int pid = -1;
            const int scanned = std::fscanf(file, "%d", &pid);
            std::fclose(file);
            if (scanned == 1) {
                return static_cast<pid_t>(pid);
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return -1;
        }
        std::this_thread::sleep_for(10ms);
    }
}

bool processIsAlive(pid_t pid)
{
    if (pid <= 0) {
        return false;
    }
    if (::kill(pid, 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

void testCommandLine()
{
    const sdl3mcp::CliParseResult parsed = sdl3mcp::parseCommandLine(
        {"--startup-timeout-ms", "250", "--target", "/bin/app", "--fullscreen", "-v"});
    CHECK(parsed.ok);
    CHECK(parsed.options.startupTimeoutMs == 250);
    CHECK(parsed.options.target == "/bin/app");
    CHECK((parsed.options.targetArguments ==
           std::vector<std::string>{"--fullscreen", "-v"}));

    const sdl3mcp::CliParseResult defaults =
        sdl3mcp::parseCommandLine({"--target", "app"});
    CHECK(defaults.ok);
    CHECK(defaults.options.startupTimeoutMs == sdl3mcp::kDefaultStartupTimeoutMs);
    CHECK(defaults.options.targetArguments.empty());

    CHECK(!sdl3mcp::parseCommandLine({}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--target"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--bogus"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"app"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--startup-timeout-ms", "0", "--target", "app"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--startup-timeout-ms", "10"}).ok);
}

void testToolListViews()
{
    const nlohmann::json idle =
        sdl3mcp::supervisorToolList(SupervisorState::Idle, nlohmann::json::array());
    CHECK(idle["tools"].is_array());
    CHECK(idle["tools"].size() == 1);
    CHECK(idle["tools"][0]["name"] == "spawn");
    CHECK(idle["tools"][0]["inputSchema"]["type"] == "object");

    const nlohmann::json starting =
        sdl3mcp::supervisorToolList(SupervisorState::Starting, nlohmann::json::array());
    CHECK(starting["tools"].is_array());
    CHECK(starting["tools"].empty());

    const nlohmann::json childTools = nlohmann::json::array({
        nlohmann::json{{"name", "sdl_info"}},
        nlohmann::json{{"name", "sdl_input"}},
    });
    const nlohmann::json running =
        sdl3mcp::supervisorToolList(SupervisorState::Running, childTools);
    CHECK(running["tools"] == childTools);
}

void testIdleListsOnlySpawn(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild});

    const nlohmann::json initialize = client.initialize();
    CHECK(!initialize.contains("error"));
    CHECK(initialize["result"]["protocolVersion"] == "2025-11-25");
    CHECK(initialize["result"]["capabilities"]["tools"]["listChanged"] == true);
    CHECK(initialize["result"]["serverInfo"]["name"] == "sdl3mcp-supervisor");
    CHECK(initialize["result"]["serverInfo"]["version"].is_string());

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"spawn"}));
    CHECK(client.listChangedCount() == 0);

    const nlohmann::json ping = client.request("ping");
    CHECK(ping["result"] == nlohmann::json::object());

    const nlohmann::json unknown = client.request("nope");
    CHECK(unknown.contains("error"));
    CHECK(unknown["error"]["code"] == -32601);

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

void testSpawnAndProxy(const std::string &supervisor,
                       const std::string &fakeChild,
                       const std::string &pidFile)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--window-delay-ms", "300",
                              "--pid-file", pidFile});
    CHECK(!client.initialize().contains("error"));

    const nlohmann::json spawn = client.spawn();
    CHECK(!spawn.contains("error"));
    CHECK(spawn["result"]["isError"] == false);

    const std::string text = spawnText(spawn);
    CHECK(text.find("Fake Child Window") != std::string::npos);
    CHECK(text.find("640") != std::string::npos);
    CHECK(text.find("480") != std::string::npos);

    /* Starting and Running each sent one list-changed notification. */
    CHECK(client.listChangedCount() >= 2);

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"sdl_info", "sdl_input", "sdl_capture"}));

    /* Proxied sdl_info keeps the child's payload. */
    const nlohmann::json info = client.callTool("sdl_info");
    CHECK(info["result"]["isError"] == false);
    const std::string infoText = info["result"]["content"][0]["text"].get<std::string>();
    const nlohmann::json payload = nlohmann::json::parse(infoText);
    CHECK(payload["window"]["title"] == "Fake Child Window");
    CHECK(payload["window"]["width"] == 640);
    CHECK(payload["window"]["height"] == 480);

    /* Proxied sdl_input keeps the child's content unchanged. */
    const nlohmann::json input =
        client.callTool("sdl_input", nlohmann::json{{"actions", nlohmann::json::array()}});
    CHECK(input["result"]["isError"] == false);
    CHECK(input["result"]["content"] ==
          nlohmann::json::array({
              nlohmann::json{{"type", "text"}, {"text", "fake sdl_input ok"}},
          }));

    /* A child JSON-RPC error travels back unchanged. */
    const nlohmann::json unknown = client.callTool("nope");
    CHECK(unknown.contains("error"));
    CHECK(unknown["error"]["code"] == -32602);
    CHECK(unknown["error"]["message"] == "Unknown tool: nope");

    /* A second spawn is refused while the child owns the tools. */
    const nlohmann::json second = client.spawn();
    CHECK(second["result"]["isError"] == true);
    CHECK(spawnText(second).find("spawn is only valid while idle") != std::string::npos);

    /* stdin EOF must stop the child, not leave it running. */
    const pid_t childPid = readPidFile(pidFile);
    CHECK(childPid > 0);
    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
    CHECK(!processIsAlive(childPid));
}

void testChildExitWhileRunning(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--exit-after-ms", "400"});
    CHECK(!client.initialize().contains("error"));

    const nlohmann::json spawn = client.spawn();
    CHECK(!spawn.contains("error"));
    CHECK(spawn["result"]["isError"] == false);

    /* The only message after the spawn response is the withdrawal notice. */
    const nlohmann::json notification = client.readMessage();
    CHECK(notification["method"] == "notifications/tools/list_changed");

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"spawn"}));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

void testChildExitDuringStartup(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--startup-timeout-ms", "5000", "--target", fakeChild,
                              "--exit-after-ms", "100", "--no-window"});
    CHECK(!client.initialize().contains("error"));

    const nlohmann::json spawn = client.spawn();
    CHECK(spawn["result"]["isError"] == true);
    CHECK(spawnText(spawn).find("fake-child:") != std::string::npos);

    /* Starting and the failed return to Idle are both announced. */
    CHECK(client.listChangedCount() >= 2);

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"spawn"}));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

void testInitializeFailure(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--startup-timeout-ms", "5000", "--target", fakeChild,
                              "--fail-initialize"});
    CHECK(!client.initialize().contains("error"));

    const nlohmann::json spawn = client.spawn();
    CHECK(spawn["result"]["isError"] == true);
    CHECK(spawnText(spawn).find("cannot initialize") != std::string::npos);

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"spawn"}));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

void testNonexistentTarget(const std::string &supervisor)
{
    SupervisorClient client;
    client.start(supervisor, {"--startup-timeout-ms", "2000", "--target",
                              "/nonexistent/sdl3mcp-test-target"});
    CHECK(!client.initialize().contains("error"));

    const nlohmann::json spawn = client.spawn();
    CHECK(spawn["result"]["isError"] == true);
    const std::string text = spawnText(spawn);
    CHECK(text.find("spawn failed") != std::string::npos);
    CHECK(text.find("cannot execute") != std::string::npos);

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"spawn"}));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

void testWindowTimeout(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--startup-timeout-ms", "400", "--target", fakeChild,
                              "--no-window"});
    CHECK(!client.initialize().contains("error"));

    const auto started = std::chrono::steady_clock::now();
    const nlohmann::json spawn = client.spawn();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    CHECK(spawn["result"]["isError"] == true);
    CHECK(spawnText(spawn).find("did not open a window") != std::string::npos);
    /* The 400 ms budget, not an unbounded wait. */
    CHECK(elapsed < 5000);

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"spawn"}));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

} // namespace

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    const std::string supervisor = argv[1];
    const std::string fakeChild = argv[2];

    testCommandLine();
    testToolListViews();
    testIdleListsOnlySpawn(supervisor, fakeChild);

    const std::string pidFile = makeTempPath();
    testSpawnAndProxy(supervisor, fakeChild, pidFile);
    ::unlink(pidFile.c_str());

    testChildExitWhileRunning(supervisor, fakeChild);
    testChildExitDuringStartup(supervisor, fakeChild);
    testInitializeFailure(supervisor, fakeChild);
    testNonexistentTarget(supervisor);
    testWindowTimeout(supervisor, fakeChild);

    std::printf("supervisor_tests: all checks passed\n");
    return 0;
}
