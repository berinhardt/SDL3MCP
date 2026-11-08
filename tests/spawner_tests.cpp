/*
  The spawner end to end: the real executable dials a relay the test listens on,
  then spawns the configured fake child, which dials back with --mcp=<endpoint>
  and speaks MCP exactly like an application.

  Covered here: every --targets validation failure, the app_spawn_<alias> tool
  view, a successful spawn (the pid arrives only after the child answers
  initialize, measured against a child that answers late), the tool list staying
  byte-identical through spawn, attach, and exit, the child-exits-first error,
  the dial-timeout and initialize-timeout paths terminating the child, the
  explicit --listen bind failure, the spawner taking its live child with it when
  the relay connection closes, and the assembled relay + spawner +
  dial-back-child loopback that a manual run exercises.
*/

#include "cli.hpp"
#include "socket_peer.hpp"
#include "supervisor.hpp"
#include "targets.hpp"
#include "transport.hpp"

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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

/* A private directory emptied and removed on destruction. */
struct TempDir
{
    std::string path;

    TempDir()
    {
        char buffer[] = "/tmp/sdl3mcp-targets-XXXXXX";
        const char *created = ::mkdtemp(buffer);
        CHECK(created != nullptr);
        path = created;
    }

    ~TempDir()
    {
        if (path.empty()) {
            return;
        }
        if (DIR *directory = ::opendir(path.c_str())) {
            while (const dirent *entry = ::readdir(directory)) {
                const std::string name = entry->d_name;
                if (name != "." && name != "..") {
                    ::unlink((path + "/" + name).c_str());
                }
            }
            ::closedir(directory);
        }
        ::rmdir(path.c_str());
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    std::string file(const std::string &name) const { return path + "/" + name; }
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

    /*
      Marks this side's pipe ends close-on-exec, so a later child of the test
      cannot keep the far end alive after this side closes it.
    */
    void closeOnExec()
    {
        for (const int fd : {toChild, fromChild, stderrChild}) {
            if (fd < 0) {
                continue;
            }
            const int flags = ::fcntl(fd, F_GETFD);
            CHECK(flags >= 0);
            CHECK(::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0);
        }
    }
};

/* The relay side the spawner dials: a listener plus the accepted MCP connection. */
struct RelaySide
{
    TestProcess process;
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

    /* The client half: a real supervisor process spoken to over its own pipes. */
    void startProcess(const std::string &executable,
                      const std::vector<std::string> &arguments)
    {
        process.start(executable, arguments);
        transport = std::make_unique<Transport>(process.fromChild, process.toChild);
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

/*
  Polls tools/list until the predicate accepts the names. A view change is never
  taken from a notification alone: the list the caller would see is the thing
  under test, and it has to settle within the bounded wait.
*/
std::vector<std::string> awaitToolList(
    RelaySide &relay,
    const std::function<bool(const std::vector<std::string> &)> &accept,
    int timeoutMs = kLineTimeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const std::vector<std::string> names = toolNames(relay.toolsList());
        if (accept(names)) {
            return names;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::string seen;
            for (const std::string &name : names) {
                seen += (seen.empty() ? "" : ", ") + name;
            }
            std::fprintf(stderr, "tools/list never matched; last view: %s\n", seen.c_str());
            CHECK(false);
            return names;
        }
        std::this_thread::sleep_for(20ms);
    }
}

/*
  The tool list no longer reveals whether an application is attached, so the
  spawner's Idle is observed through behaviour: `sdl_info` is refused with the
  no-application message until the child has exited and the monitor has published
  Idle. Polling it is harmless while a child runs.
*/
void awaitSpawnerIdle(RelaySide &relay, int timeoutMs = kLineTimeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const nlohmann::json info = relay.callTool("sdl_info");
        CHECK(!info.contains("error"));
        if (info["result"]["isError"] == true &&
            resultText(info).find("no application is attached") != std::string::npos) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "the spawner never returned to idle; last sdl_info: %s\n",
                         info.dump().c_str());
            CHECK(false);
            return;
        }
        std::this_thread::sleep_for(20ms);
    }
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

/* ---- multiple --targets sources ---------------------------------------- */

void testTargetsSources()
{
    /* A directory contributes every top-level *.json in sorted filename order. */
    TempDir directory;
    writeFile(directory.file("10-vulkan.json"), R"({"alpha":["./alpha"]})");
    writeFile(directory.file("20-opengl.json"), R"({"beta":["/bin/beta"]})");
    writeFile(directory.file("notes.txt"), "ignored");

    sdl3mcp::SpawnTargets targets;
    std::string error;
    CHECK(sdl3mcp::loadSpawnTargets(directory.path, targets, error));
    CHECK(targets.size() == 2);
    CHECK(targets[0].alias == "alpha");
    CHECK(targets[1].alias == "beta");

    /* Repeating --targets unions a directory and a hand-written file. */
    TempDir extra;
    writeFile(extra.file("extra.json"), R"({"gamma":["./gamma"]})");
    const sdl3mcp::CliParseResult repeated =
        sdl3mcp::parseCommandLine({"--connect=127.0.0.1:1234",
                                   "--targets=" + directory.path,
                                   "--targets=" + extra.file("extra.json")});
    CHECK(repeated.ok);
    CHECK(repeated.options.targetsPaths.size() == 2);
    CHECK((toolNames(nlohmann::json{{"result", sdl3mcp::spawnerToolList(
                                                        repeated.options.targets)}}) ==
           std::vector<std::string>{"app_spawn_alpha", "app_spawn_beta",
                                    "app_spawn_gamma"}));

    /* A duplicate alias across two files names both and replaces neither. */
    TempDir duplicates;
    writeFile(duplicates.file("a.json"), R"({"alpha":["/a"]})");
    writeFile(duplicates.file("b.json"), R"({"alpha":["/b"]})");
    CHECK(!sdl3mcp::loadSpawnTargets(duplicates.path, targets, error));
    CHECK(targets.empty());
    CHECK(error.find(duplicates.file("a.json")) != std::string::npos);
    CHECK(error.find(duplicates.file("b.json")) != std::string::npos);

    /* Malformed JSON inside a directory names the offending file. */
    TempDir broken;
    writeFile(broken.file("good.json"), R"({"good":["/good"]})");
    writeFile(broken.file("zz-broken.json"), "{");
    CHECK(!sdl3mcp::loadSpawnTargets(broken.path, targets, error));
    CHECK(error.find(broken.file("zz-broken.json")) != std::string::npos);

    /* A non-object top level inside a directory names its file too. */
    TempDir arrayTop;
    writeFile(arrayTop.file("list.json"), "[]");
    CHECK(!sdl3mcp::loadSpawnTargets(arrayTop.path, targets, error));
    CHECK(error.find(arrayTop.file("list.json")) != std::string::npos);
    CHECK(error.find("JSON object") != std::string::npos);

    /* A directory with no *.json files is an error naming the directory. */
    TempDir empty;
    CHECK(!sdl3mcp::loadSpawnTargets(empty.path, targets, error));
    CHECK(error.find(empty.path) != std::string::npos);
    CHECK(error.find("*.json") != std::string::npos);
}

/* ---- the spawner against a relay the test owns ------------------------- */

struct SpawnerFixture
{
    TempPath targetsFile;
    TempPath appPidFile;
    TempPath slowPidFile;
    TempPath crashPidFile;
    TempPath slowInitPidFile;
    TempPath silentPidFile;
    RelaySide relay;
    TestProcess process;

    void writeTargets(const std::string &fakeChild)
    {
        nlohmann::json targets = nlohmann::json::object();
        targets["app"] = nlohmann::json::array(
            {fakeChild, "--pid-file", appPidFile.path});
        targets["slow"] = nlohmann::json::array(
            {fakeChild, "--pid-file", slowPidFile.path, "--dial-delay-ms", "5000"});
        targets["slowinit"] = nlohmann::json::array(
            {fakeChild, "--pid-file", slowInitPidFile.path, "--initialize-delay-ms",
             "700"});
        targets["silent"] = nlohmann::json::array(
            {fakeChild, "--pid-file", silentPidFile.path, "--initialize-delay-ms",
             "5000"});
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

    /* The mode-derived view: app_spawn_* plus the application block, always. */
    const nlohmann::json idleList = fixture.relay.toolsList();
    const std::vector<std::string> idleNames = toolNames(idleList);
    CHECK(idleNames.size() == 9);
    CHECK(hasTool(idleNames, "sdl_info"));
    CHECK(hasTool(idleNames, "sdl_input"));
    CHECK(hasTool(idleNames, "sdl_capture"));
    CHECK(hasTool(idleNames, "sdl_terminate"));
    CHECK(hasTool(idleNames, "app_spawn_app"));
    CHECK(hasTool(idleNames, "app_spawn_slow"));
    CHECK(hasTool(idleNames, "app_spawn_slowinit"));
    CHECK(hasTool(idleNames, "app_spawn_silent"));
    CHECK(hasTool(idleNames, "app_spawn_crasher"));

    /*
      A successful spawn blocks for the whole handshake: the child answers
      initialize only after 700 ms, and the pid text may not arrive before it.
    */
    const auto started = std::chrono::steady_clock::now();
    const nlohmann::json spawned = fixture.relay.callTool("app_spawn_slowinit");
    const auto spawnElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    CHECK(spawned["result"]["isError"] == false);
    const std::string spawnedText = resultText(spawned);
    CHECK(spawnedText.find("pid") != std::string::npos);
    CHECK(spawnElapsed >= 650);
    const pid_t appPid = parsePidFile(fixture.slowInitPidFile.path);
    CHECK(::kill(appPid, 0) == 0);

    /* Attaching the application does not move the spawner's list by one byte. */
    CHECK(fixture.relay.toolsList()["result"].dump() == idleList["result"].dump());

    /* sdl_terminate is an ordinary forward; the app then exits on its own. */
    const nlohmann::json terminated = fixture.relay.callTool("sdl_terminate");
    CHECK(terminated["result"]["isError"] == false);
    CHECK(resultText(terminated) == "termination requested");

    /* The exit restores Idle without changing the list. */
    awaitSpawnerIdle(fixture.relay);
    CHECK(fixture.relay.toolsList()["result"].dump() == idleList["result"].dump());

    /* A child that exits before connecting reports its exit status. */
    const nlohmann::json crashed = fixture.relay.callTool("app_spawn_crasher");
    CHECK(crashed["result"]["isError"] == true);
    const std::string crashText = resultText(crashed);
    CHECK(crashText.find("7") != std::string::npos);
    CHECK(fixture.relay.toolsList()["result"].dump() == idleList["result"].dump());

    /* A child that never dials is terminated when timeout_ms expires. */
    const auto dialStarted = std::chrono::steady_clock::now();
    const nlohmann::json timedOut =
        fixture.relay.callTool("app_spawn_slow", nlohmann::json{{"timeout_ms", 300}});
    const auto dialElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - dialStarted)
                                 .count();
    CHECK(timedOut["result"]["isError"] == true);
    const std::string timeoutText = resultText(timedOut);
    CHECK(timeoutText.find("300") != std::string::npos);
    CHECK(timeoutText.find("pid") != std::string::npos);
    CHECK(dialElapsed >= 250);
    CHECK(dialElapsed < 5000);
    CHECK(processIsGone(parsePidFile(fixture.slowPidFile.path)));
    CHECK(fixture.relay.toolsList()["result"].dump() == idleList["result"].dump());

    /* A child that connects but never answers initialize hits the same path. */
    const auto answerStarted = std::chrono::steady_clock::now();
    const nlohmann::json unanswered =
        fixture.relay.callTool("app_spawn_silent", nlohmann::json{{"timeout_ms", 400}});
    const auto answerElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - answerStarted)
                                   .count();
    CHECK(unanswered["result"]["isError"] == true);
    const std::string unansweredText = resultText(unanswered);
    CHECK(unansweredText.find("initialize") != std::string::npos);
    /* The spawner's deadline is its own timeout_ms, not --startup-timeout-ms. */
    CHECK(unansweredText.find("spawn timeout") != std::string::npos);
    CHECK(answerElapsed >= 350);
    CHECK(answerElapsed < 5000);
    CHECK(processIsGone(parsePidFile(fixture.silentPidFile.path)));
    CHECK(fixture.relay.toolsList()["result"].dump() == idleList["result"].dump());

    /* A spawn and an exit change nothing a client can list, so nothing is announced. */
    CHECK(fixture.relay.listChangedCount() == 0);

    /* Spawn again, then close the relay: the spawner exits and takes the child. */
    const nlohmann::json again = fixture.relay.callTool("app_spawn_app");
    CHECK(again["result"]["isError"] == false);
    CHECK(fixture.relay.toolsList()["result"].dump() == idleList["result"].dump());
    const pid_t livePid = parsePidFile(fixture.appPidFile.path);

    fixture.relay.transport->stop();
    fixture.relay.transport.reset();
    ::close(fixture.relay.connection);
    fixture.relay.connection = -1;

    CHECK(fixture.process.waitForExit(5000));
    CHECK(processIsGone(livePid));
}

/*
  The assembled path: a real relay process in front of a real spawner process,
  with the fake child dialling the spawner and speaking MCP like an application.
  This is the shape of the manual run that found the missing contract, so it
  asserts exactly what such a caller observes: the spawn answer means the
  session is up, the relay's own tools/list follows it in both directions, and a
  capture arrives as a PNG of the application's dimensions.
*/
void testRelayLoopback(const std::string &sdl3mcp, const std::string &fakeChild)
{
    TempPath targetsFile;
    TempPath pidFile;
    writeFile(targetsFile.path,
              nlohmann::json{{"app", nlohmann::json::array(
                                         {fakeChild, "--pid-file", pidFile.path,
                                          "--capture-width", "3",
                                          "--capture-height", "2"})}}
                  .dump());

    /* The client speaks to the relay's stdio; the relay listens on an ephemeral port. */
    RelaySide client;
    client.startProcess(sdl3mcp, {"--listen=127.0.0.1:0"});

    const std::string line = client.process.readStderrLine(kLineTimeoutMs);
    const std::string prefix = "sdl3mcp: listening on ";
    CHECK(line.rfind(prefix, 0) == 0);
    const std::size_t separator = line.rfind(':');
    CHECK(separator != std::string::npos);
    const long parsed = std::strtol(line.c_str() + separator + 1, nullptr, 10);
    CHECK(parsed > 0 && parsed <= 65535);
    const std::uint16_t port = static_cast<std::uint16_t>(parsed);

    /* The spawner must not inherit the client's ends of the relay's pipes. */
    client.process.closeOnExec();

    TestProcess spawner;
    spawner.start(sdl3mcp, {"--connect=127.0.0.1:" + std::to_string(port),
                            "--targets=" + targetsFile.path});

    CHECK(!client.initialize().contains("error"));
    const std::vector<std::string> beforeSpawn =
        awaitToolList(client, [](const std::vector<std::string> &names) {
            return hasTool(names, "app_spawn_app");
        });
    CHECK(beforeSpawn.size() == 6);
    CHECK(hasTool(beforeSpawn, "wait_session"));
    CHECK(hasTool(beforeSpawn, "sdl_info"));
    CHECK(hasTool(beforeSpawn, "sdl_capture"));

    /* Snapshot the exact bytes: a spawn inside the spawner must not move them. */
    const nlohmann::json stable = client.toolsList();

    /* The spawn answer carries the pid only once the child answered initialize. */
    const nlohmann::json spawned = client.callTool("app_spawn_app");
    CHECK(spawned["result"]["isError"] == false);
    CHECK(resultText(spawned).find("pid") != std::string::npos);
    const pid_t childPid = parsePidFile(pidFile.path);
    CHECK(::kill(childPid, 0) == 0);

    /*
      The relay's list is fixed before the spawn and stays byte-identical during
      it: the spawner never moved its own list, so nothing arrived late.
    */
    CHECK(client.toolsList()["result"].dump() == stable["result"].dump());

    /* The capture keeps its dimensions and its image form across both hops. */
    const nlohmann::json capture = client.callTool("sdl_capture");
    CHECK(capture["result"]["isError"] == false);
    const nlohmann::json &content = capture["result"]["content"];
    CHECK(content.size() == 2);
    CHECK(content[0]["text"].get<std::string>().rfind("3x2 PNG", 0) == 0);
    CHECK(content[1]["type"] == "image");
    CHECK(content[1]["mimeType"] == "image/png");
    const std::string data = content[1]["data"].get<std::string>();
    CHECK(data.size() > 20);
    /* The base64 of the PNG signature: the payload crossed both hops intact. */
    CHECK(data.rfind("iVBORw0KGgo", 0) == 0);

    /* Termination runs back through the same chain; the list still does not move. */
    const nlohmann::json terminated = client.callTool("sdl_terminate");
    CHECK(terminated["result"]["isError"] == false);
    CHECK(resultText(terminated) == "termination requested");
    CHECK(client.toolsList()["result"].dump() == stable["result"].dump());
    CHECK(processIsGone(childPid));

    /* The client leaving ends the relay, which ends the spawner behind it. */
    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
    CHECK(spawner.waitForExit(kLineTimeoutMs));
}

/*
  The application-side peer: the same recovery as the relay's, observed through
  the spawner. A dead application leaves the list untouched, and a forwarded
  call that was in flight when it died answers instead of holding the slot.
*/
void testAppPeerGone(const std::string &spawner, const std::string &fakeChild)
{
    SpawnerFixture fixture;
    fixture.writeTargets(fakeChild);
    fixture.start(spawner);

    const nlohmann::json spawned = fixture.relay.callTool("app_spawn_app");
    CHECK(spawned["result"]["isError"] == false);
    const pid_t appPid = parsePidFile(fixture.appPidFile.path);
    const nlohmann::json stable = fixture.relay.toolsList();

    /* The application dies while the spawner is idle. */
    CHECK(::kill(appPid, SIGKILL) == 0);
    awaitSpawnerIdle(fixture.relay);
    CHECK(processIsGone(appPid));
    CHECK(fixture.relay.toolsList()["result"].dump() == stable["result"].dump());

    /* The session recovered, so the same alias spawns again. */
    const nlohmann::json again = fixture.relay.callTool("app_spawn_app");
    CHECK(again["result"]["isError"] == false);
    CHECK(fixture.relay.toolsList()["result"].dump() == stable["result"].dump());

    /*
      Stop the application so the call is really in flight, then kill it: the
      answer must name the tool and the session must recover.
    */
    const pid_t livePid = parsePidFile(fixture.appPidFile.path);
    CHECK(::kill(livePid, SIGSTOP) == 0);
    const int callId = fixture.relay.send("tools/call", nlohmann::json{
                                                           {"name", "sdl_info"},
                                                           {"arguments", nlohmann::json::object()},
                                                       });
    std::this_thread::sleep_for(100ms);
    CHECK(::kill(livePid, SIGKILL) == 0);

    const nlohmann::json answer = fixture.relay.readResponse(callId);
    CHECK(answer["result"]["isError"] == true);
    CHECK(resultText(answer).find("sdl_info") != std::string::npos);

    awaitSpawnerIdle(fixture.relay);
    CHECK(fixture.relay.toolsList()["result"].dump() == stable["result"].dump());
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

    const nlohmann::json stable = fixture.relay.toolsList();
    const nlohmann::json failed = fixture.relay.callTool("app_spawn_app");
    ::close(blocker);
    CHECK(failed["result"]["isError"] == true);
    const std::string text = resultText(failed);
    CHECK(text.find("--listen") != std::string::npos);
    CHECK(text.find(std::to_string(blockedPort)) != std::string::npos);

    /* A failed spawn changes nothing a client lists. */
    CHECK(fixture.relay.toolsList()["result"].dump() == stable["result"].dump());
    CHECK(fixture.relay.listChangedCount() == 0);
}

} // namespace

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    const std::string spawner = argv[1];
    const std::string fakeChild = argv[2];

    testTargetsValidation();
    testCommandLineAndToolList();
    testTargetsSources();
    testSpawnerRoundTrip(spawner, fakeChild);
    testRelayLoopback(spawner, fakeChild);
    testAppPeerGone(spawner, fakeChild);
    testExplicitListenBindFailure(spawner, fakeChild);

    std::printf("spawner_tests: all checks passed\n");
    return 0;
}
