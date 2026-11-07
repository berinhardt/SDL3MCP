/*
  The spawner end to end: the real executable dials a relay the test listens on,
  then spawns the configured fake child, which dials back with --mcp=<endpoint>
  and speaks MCP exactly like an application.

  Covered here: every --targets validation failure, the app_spawn_<alias> tool
  view, a successful spawn (pid returned only after the handshake), the tool list
  becoming the application's, its return after termination, the child-exits-first
  error, the timeout path terminating a child that never connects, the explicit
  --listen bind failure, and the spawner taking its live child with it when the
  relay connection closes.
*/

#include "cli.hpp"
#include "socket_peer.hpp"
#include "supervisor.hpp"
#include "targets.hpp"
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
#include <cstring>
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

std::string tempPath()
{
    char buffer[] = "/tmp/sdl3mcp-spawner-XXXXXX";
    const int fd = ::mkstemp(buffer);
    CHECK(fd >= 0);
    ::close(fd);
    return std::string(buffer);
}

void writeFile(const std::string &path, const std::string &contents)
{
    std::FILE *file = std::fopen(path.c_str(), "wb");
    CHECK(file != nullptr);
    CHECK(std::fwrite(contents.data(), 1, contents.size(), file) == contents.size());
    CHECK(std::fclose(file) == 0);
}

std::string readFile(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr);
    std::string contents;
    char chunk[256];
    std::size_t count = 0;
    while ((count = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
        contents.append(chunk, count);
    }
    std::fclose(file);
    return contents;
}

struct TempPath
{
    std::string path;
    TempPath() : path(tempPath()) {}
    explicit TempPath(std::string value) : path(std::move(value)) {}
    ~TempPath()
    {
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }
    TempPath(const TempPath &) = delete;
    TempPath &operator=(const TempPath &) = delete;
};

/* A forked process with stdin/stdout/stderr pipes, killed and reaped on destruction. */
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
        for (int *fd : {&toChild, &fromChild, &stderrChild}) {
            if (*fd >= 0) {
                ::close(*fd);
                *fd = -1;
            }
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

/* The relay side the spawner dials: a listener plus the accepted MCP connection. */
struct RelaySide
{
    int listener = -1;
    int connection = -1;
    std::unique_ptr<Transport> transport;
    std::vector<nlohmann::json> notifications;
    int nextId = 1;

    ~RelaySide()
    {
        if (transport) {
            transport->stop();
            transport.reset();
        }
        if (connection >= 0) {
            ::close(connection);
        }
        if (listener >= 0) {
            ::close(listener);
        }
    }

    std::uint16_t listen()
    {
        std::uint16_t port = 0;
        std::string error;
        CHECK(sdl3mcp::listenSocket("127.0.0.1", 0, listener, port, error));
        CHECK(port != 0);
        return port;
    }

    /* Polls with a bound so a spawner that failed to start does not hang the test. */
    void accept(int timeoutMs)
    {
        pollfd descriptor{};
        descriptor.fd = listener;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, timeoutMs);
        CHECK(ready == 1);
        connection = ::accept(listener, nullptr, nullptr);
        CHECK(connection >= 0);
        ::close(listener);
        listener = -1;
        transport = std::make_unique<Transport>(connection, connection);
    }

    nlohmann::json readMessage(int timeoutMs = kLineTimeoutMs)
    {
        std::string line;
        std::string error;
        const ReadStatus status = transport->readLine(line, error, timeoutMs);
        if (status != ReadStatus::Line) {
            std::fprintf(stderr, "relay read status %d: %s\n", static_cast<int>(status),
                         error.c_str());
        }
        CHECK(status == ReadStatus::Line);
        const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        CHECK(!message.is_discarded());
        return message;
    }

    int send(const std::string &method, nlohmann::json params = nlohmann::json::object())
    {
        const int id = nextId++;
        CHECK(transport->writeLine(nlohmann::json{
                                       {"jsonrpc", "2.0"},
                                       {"id", id},
                                       {"method", method},
                                       {"params", std::move(params)},
                                   }
                                       .dump()));
        return id;
    }

    void write(const nlohmann::json &message)
    {
        CHECK(transport->writeLine(message.dump()));
    }

    nlohmann::json readResponse(int id, int timeoutMs = kLineTimeoutMs)
    {
        for (;;) {
            nlohmann::json message = readMessage(timeoutMs);
            if (message.contains("method")) {
                notifications.push_back(std::move(message));
                continue;
            }
            CHECK(message.contains("id"));
            CHECK(message["id"] == id);
            return message;
        }
    }

    nlohmann::json request(const std::string &method,
                           nlohmann::json params = nlohmann::json::object(),
                           int timeoutMs = kLineTimeoutMs)
    {
        return readResponse(send(method, std::move(params)), timeoutMs);
    }

    nlohmann::json initialize()
    {
        return request("initialize", nlohmann::json{
                                         {"protocolVersion", "2025-11-25"},
                                         {"capabilities", nlohmann::json::object()},
                                         {"clientInfo", nlohmann::json{
                                              {"name", "spawner-tests"},
                                              {"version", "0.0.0"},
                                          }},
                                     });
    }

    void notifyInitialized()
    {
        write(nlohmann::json{{"jsonrpc", "2.0"},
                             {"method", "notifications/initialized"}});
    }

    nlohmann::json toolsList() { return request("tools/list"); }

    nlohmann::json callTool(const std::string &name,
                            nlohmann::json arguments = nlohmann::json::object(),
                            int timeoutMs = kLineTimeoutMs)
    {
        return readResponse(send("tools/call", nlohmann::json{
                                                   {"name", name},
                                                   {"arguments", std::move(arguments)},
                                               }),
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

    /* Reads messages until at least `expected` list_changed notifications arrived. */
    void waitForListChanged(size_t expected)
    {
        while (listChangedCount() < expected) {
            nlohmann::json message = readMessage();
            CHECK(message.contains("method"));
            notifications.push_back(std::move(message));
        }
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

std::string resultText(const nlohmann::json &response)
{
    CHECK(response.contains("result"));
    CHECK(response["result"]["content"].is_array());
    CHECK(!response["result"]["content"].empty());
    return response["result"]["content"][0].value("text", std::string());
}

bool hasTool(const std::vector<std::string> &names, const std::string &name)
{
    for (const std::string &candidate : names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

bool processIsGone(pid_t pid)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        if (::kill(pid, 0) != 0 && errno == ESRCH) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
}

pid_t parsePidFile(const std::string &path)
{
    const std::string text = readFile(path);
    const long parsed = std::strtol(text.c_str(), nullptr, 10);
    CHECK(parsed > 0);
    return static_cast<pid_t>(parsed);
}

/* ---- targets-file validation ------------------------------------------- */

void testTargetsValidation()
{
    const auto failsWith = [](const std::string &contents, const std::string &needle) {
        TempPath file;
        writeFile(file.path, contents);
        sdl3mcp::SpawnTargets targets;
        std::string error;
        CHECK(!sdl3mcp::loadSpawnTargets(file.path, targets, error));
        if (error.find(needle) == std::string::npos) {
            std::fprintf(stderr, "error for %s did not name %s: %s\n", contents.c_str(),
                         needle.c_str(), error.c_str());
        }
        CHECK(error.find(needle) != std::string::npos);
        CHECK(targets.empty());
    };

    failsWith("{", "not valid JSON");
    failsWith("[]", "JSON object");
    failsWith("{}", "defines no aliases");
    failsWith(R"({"Bad":["app"]})", "Bad");
    failsWith(R"({"bad-key":["app"]})", "bad-key");
    failsWith(R"({"app_spawn_x":["app"]})", "app_spawn_x");
    failsWith(R"({"x":null})", "x");
    failsWith(R"({"x":[]})", "x");
    failsWith(R"({"x":["app",5]})", "x");
    failsWith(R"({"x":["app","--mcp=stdio"]})", "x");

    sdl3mcp::SpawnTargets targets;
    std::string error;
    CHECK(!sdl3mcp::loadSpawnTargets("/nonexistent/sdl3mcp-targets.json", targets, error));
    CHECK(error.find("cannot open") != std::string::npos);

    TempPath valid;
    writeFile(valid.path, R"({"alpha":["./alpha","--flag"],"beta":["/bin/beta"]})");
    CHECK(sdl3mcp::loadSpawnTargets(valid.path, targets, error));
    CHECK(targets.size() == 2);
    CHECK(targets[0].alias == "alpha");
    CHECK((targets[0].argv == std::vector<std::string>{"./alpha", "--flag"}));
    CHECK(targets[1].alias == "beta");
    CHECK((targets[1].argv == std::vector<std::string>{"/bin/beta"}));
}

/* ---- command line and the pre-spawn tool view -------------------------- */

void testCommandLineAndToolList()
{
    CHECK(!sdl3mcp::parseCommandLine({"--connect=127.0.0.1:1234"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--connect=not-an-endpoint"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--connect=127.0.0.1:0"}).ok);

    TempPath targets;
    writeFile(targets.path, R"({"alpha":["app"],"beta":["other"]})");
    CHECK(!sdl3mcp::parseCommandLine({"--targets=" + targets.path}).ok);

    const sdl3mcp::CliParseResult parsed =
        sdl3mcp::parseCommandLine({"--connect=127.0.0.1:1234",
                                   "--targets=" + targets.path});
    CHECK(parsed.ok);
    CHECK(parsed.options.spawner());
    CHECK(!parsed.options.relay());
    CHECK(parsed.options.connectHost == "127.0.0.1");
    CHECK(parsed.options.connectPort == 1234);
    CHECK(parsed.options.targets.size() == 2);

    const sdl3mcp::CliParseResult withListen =
        sdl3mcp::parseCommandLine({"--connect=127.0.0.1:1234",
                                   "--targets=" + targets.path,
                                   "--listen=127.0.0.1:9999"});
    CHECK(withListen.ok);
    CHECK(withListen.options.spawner());
    CHECK(!withListen.options.relay());
    CHECK(withListen.options.listenHost == "127.0.0.1");
    CHECK(withListen.options.listenPort == 9999);

    CHECK(!sdl3mcp::parseCommandLine({"--connect=127.0.0.1:1234",
                                      "--targets=" + targets.path,
                                      "--target", "app"})
               .ok);

    const std::vector<std::string> names =
        toolNames(nlohmann::json{{"result", sdl3mcp::spawnerToolList(parsed.options.targets)}});
    CHECK((names == std::vector<std::string>{"app_spawn_alpha", "app_spawn_beta"}));

    /* Each tool accepts an optional timeout_ms defaulting to 5000. */
    const nlohmann::json spawnerTools = sdl3mcp::spawnerToolList(parsed.options.targets);
    const nlohmann::json &tool = spawnerTools["tools"][0];
    const nlohmann::json &timeout = tool["inputSchema"]["properties"]["timeout_ms"];
    CHECK(timeout["default"] == 5000);
    CHECK(timeout["minimum"] == 1);
}

/* ---- the spawner against a relay the test owns ------------------------- */

struct SpawnerFixture
{
    TempPath targetsFile;
    TempPath appPidFile;
    TempPath slowPidFile;
    TempPath crashPidFile;
    RelaySide relay;
    TestProcess process;

    void writeTargets(const std::string &fakeChild)
    {
        nlohmann::json targets = nlohmann::json::object();
        targets["app"] = nlohmann::json::array(
            {fakeChild, "--pid-file", appPidFile.path});
        targets["slow"] = nlohmann::json::array(
            {fakeChild, "--pid-file", slowPidFile.path, "--dial-delay-ms", "5000"});
        targets["crasher"] = nlohmann::json::array(
            {fakeChild, "--pid-file", crashPidFile.path, "--exit-before-connect",
             "--exit-code", "7"});
        writeFile(targetsFile.path, targets.dump());
    }

    void start(const std::string &spawner,
               const std::string &extraListen = std::string())
    {
        const std::uint16_t port = relay.listen();
        std::vector<std::string> arguments{"--connect=127.0.0.1:" +
                                           std::to_string(port),
                                           "--targets=" + targetsFile.path};
        if (!extraListen.empty()) {
            arguments.push_back(extraListen);
        }
        process.start(spawner, arguments);
        relay.accept(kLineTimeoutMs);
        CHECK(!relay.initialize().contains("error"));
        relay.notifyInitialized();
    }
};

void testSpawnerRoundTrip(const std::string &spawner, const std::string &fakeChild)
{
    SpawnerFixture fixture;
    fixture.writeTargets(fakeChild);
    fixture.start(spawner);

    const std::vector<std::string> idleNames = toolNames(fixture.relay.toolsList());
    CHECK(idleNames.size() == 3);
    CHECK(hasTool(idleNames, "app_spawn_app"));
    CHECK(hasTool(idleNames, "app_spawn_slow"));
    CHECK(hasTool(idleNames, "app_spawn_crasher"));

    /* A successful spawn: pid only after the accepted peer answers initialize. */
    const nlohmann::json spawned = fixture.relay.callTool("app_spawn_app");
    CHECK(spawned["result"]["isError"] == false);
    const std::string spawnedText = resultText(spawned);
    CHECK(spawnedText.find("pid") != std::string::npos);
    const pid_t appPid = parsePidFile(fixture.appPidFile.path);
    CHECK(::kill(appPid, 0) == 0);

    /* Starting and attached each announce once. */
    fixture.relay.waitForListChanged(2);

    const std::vector<std::string> attachedNames = toolNames(fixture.relay.toolsList());
    CHECK(!attachedNames.empty());
    for (const std::string &name : attachedNames) {
        CHECK(name.rfind("app_spawn_", 0) != 0);
    }
    CHECK(hasTool(attachedNames, "sdl_info"));
    CHECK(hasTool(attachedNames, "sdl_capture"));

    /* sdl_terminate is an ordinary forward; the app then exits on its own. */
    const nlohmann::json terminated = fixture.relay.callTool("sdl_terminate");
    CHECK(terminated["result"]["isError"] == false);
    CHECK(resultText(terminated) == "termination requested");

    fixture.relay.waitForListChanged(3);
    const std::vector<std::string> afterTerminate = toolNames(fixture.relay.toolsList());
    CHECK(afterTerminate.size() == 3);
    CHECK(hasTool(afterTerminate, "app_spawn_app"));

    /* A child that exits before connecting reports its exit status. */
    const nlohmann::json crashed = fixture.relay.callTool("app_spawn_crasher");
    CHECK(crashed["result"]["isError"] == true);
    const std::string crashText = resultText(crashed);
    CHECK(crashText.find("7") != std::string::npos);
    fixture.relay.waitForListChanged(5);

    /* A child that never connects is terminated when timeout_ms expires. */
    const auto started = std::chrono::steady_clock::now();
    const nlohmann::json timedOut =
        fixture.relay.callTool("app_spawn_slow", nlohmann::json{{"timeout_ms", 300}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    CHECK(timedOut["result"]["isError"] == true);
    const std::string timeoutText = resultText(timedOut);
    CHECK(timeoutText.find("300") != std::string::npos);
    CHECK(timeoutText.find("pid") != std::string::npos);
    CHECK(elapsed >= 250);
    CHECK(elapsed < 5000);

    const pid_t slowPid = parsePidFile(fixture.slowPidFile.path);
    CHECK(processIsGone(slowPid));
    fixture.relay.waitForListChanged(7);
    CHECK(hasTool(toolNames(fixture.relay.toolsList()), "app_spawn_app"));

    /* Spawn again, then close the relay: the spawner exits and takes the child. */
    const nlohmann::json again = fixture.relay.callTool("app_spawn_app");
    CHECK(again["result"]["isError"] == false);
    fixture.relay.waitForListChanged(9);
    const pid_t livePid = parsePidFile(fixture.appPidFile.path);

    fixture.relay.transport->stop();
    fixture.relay.transport.reset();
    ::close(fixture.relay.connection);
    fixture.relay.connection = -1;

    CHECK(fixture.process.waitForExit(5000));
    CHECK(processIsGone(livePid));
}

void testExplicitListenBindFailure(const std::string &spawner, const std::string &fakeChild)
{
    SpawnerFixture fixture;
    fixture.writeTargets(fakeChild);

    /* Occupy a port the spawner is then told to bind explicitly. */
    int blocker = -1;
    std::uint16_t blockedPort = 0;
    std::string error;
    CHECK(sdl3mcp::listenSocket("127.0.0.1", 0, blocker, blockedPort, error));
    CHECK(blockedPort != 0);

    const std::string listenFlag = "--listen=127.0.0.1:" + std::to_string(blockedPort);
    fixture.start(spawner, listenFlag);

    const nlohmann::json failed = fixture.relay.callTool("app_spawn_app");
    ::close(blocker);
    CHECK(failed["result"]["isError"] == true);
    const std::string text = resultText(failed);
    CHECK(text.find("--listen") != std::string::npos);
    CHECK(text.find(std::to_string(blockedPort)) != std::string::npos);

    fixture.relay.waitForListChanged(2);
    CHECK(hasTool(toolNames(fixture.relay.toolsList()), "app_spawn_app"));
}

} // namespace

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    const std::string spawner = argv[1];
    const std::string fakeChild = argv[2];

    testTargetsValidation();
    testCommandLineAndToolList();
    testSpawnerRoundTrip(spawner, fakeChild);
    testExplicitListenBindFailure(spawner, fakeChild);

    std::printf("spawner_tests: all checks passed\n");
    return 0;
}
