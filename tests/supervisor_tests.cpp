/*
  The supervisor end to end: the real executable is started as a child of the
  test, driven over real pipes with per-line timeouts, and pointed at the fake
  MCP child. That covers the CLI, the line loop, the state machine, fork/exec,
  the proxy, the terminate watchdog, and shutdown in the same code the shipped
  binary runs.

  When a third argument names examples/minimal, the same client also drives the
  real library through the proxy under SDL_VIDEODRIVER=dummy: spawn, sdl_info,
  sdl_input, sdl_capture and sdl_terminate. The fake child keeps every protocol
  scenario deterministic; the real app proves the two halves meet.

  The test says nothing about SDL for the fake scenarios: neither side
  initialises video, so a failure is a protocol or process bug, not a driver
  problem.
*/

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "cli.hpp"
#include "supervisor.hpp"
#include "tools.hpp"
#include "transport.hpp"

#include <nlohmann/json.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dirent.h>

namespace
{

/* The CTest registration supplies the binaries as argv[1], argv[2] and, for the
   real-example run, argv[3]. */
std::string g_supervisor;
std::string g_fakeChild;
std::string g_example;

using namespace std::chrono_literals;
using sdl3mcp::ReadStatus;
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
                           nlohmann::json params = nlohmann::json::object(),
                           int timeoutMs = kLineTimeoutMs)
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
                            nlohmann::json arguments = nlohmann::json::object(),
                            int timeoutMs = kLineTimeoutMs)
    {
        return request("tools/call", nlohmann::json{
                                         {"name", name},
                                         {"arguments", std::move(arguments)},
                                     },
                       timeoutMs);
    }

    nlohmann::json spawn(int timeoutMs = kLineTimeoutMs)
    {
        return callTool("spawn", nlohmann::json::object(), timeoutMs);
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

/* Child mode advertises the same set in Idle, Starting and Running. */
std::vector<std::string> childToolNames()
{
    return {"spawn", "sdl_info", "sdl_input", "sdl_capture", "sdl_terminate"};
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

/* The tool document with this name in a tools/list response, or nullptr. */
const nlohmann::json *findTool(const nlohmann::json &listResponse, const std::string &name)
{
    CHECK(listResponse.contains("result"));
    const nlohmann::json &tools = listResponse["result"]["tools"];
    CHECK(tools.is_array());
    for (const nlohmann::json &tool : tools) {
        if (tool.value("name", std::string()) == name) {
            return &tool;
        }
    }
    return nullptr;
}

int base64Value(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

std::vector<uint8_t> decodeBase64(std::string_view text)
{
    std::vector<uint8_t> decoded;
    uint32_t buffer = 0;
    int bits = 0;
    for (const char character : text) {
        if (character == '=') {
            break;
        }
        const int value = base64Value(character);
        CHECK(value >= 0);
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFFu));
            buffer &= (1u << bits) - 1u;
        }
    }
    return decoded;
}

/* Reads a PNG's IHDR dimensions, which is the only image check the proxy needs. */
bool pngHasSize(const std::vector<uint8_t> &png, uint32_t width, uint32_t height)
{
    constexpr std::array<uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 33 || !std::equal(signature.begin(), signature.end(), png.begin())) {
        return false;
    }
    if (png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R') {
        return false;
    }
    const auto dimension = [&png](std::size_t offset) {
        return (static_cast<uint32_t>(png[offset]) << 24) |
               (static_cast<uint32_t>(png[offset + 1]) << 16) |
               (static_cast<uint32_t>(png[offset + 2]) << 8) |
               static_cast<uint32_t>(png[offset + 3]);
    };
    return dimension(16) == width && dimension(20) == height;
}

std::string makeTempPath()
{
    char pattern[] = "/tmp/sdl3mcp-test-XXXXXX";
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

/* True when some process is still executing `executable` (by resolved path). */
bool anyProcessUsesExecutable(const std::string &executable)
{
    char resolved[PATH_MAX];
    if (::realpath(executable.c_str(), resolved) == nullptr) {
        return false;
    }

    DIR *directory = ::opendir("/proc");
    if (directory == nullptr) {
        return false;
    }

    bool found = false;
    while (const dirent *entry = ::readdir(directory)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        const std::string link = std::string("/proc/") + entry->d_name + "/exe";
        char target[PATH_MAX];
        const ssize_t length = ::readlink(link.c_str(), target, sizeof target - 1);
        if (length <= 0) {
            continue;
        }
        target[length] = '\0';
        if (std::string(target) == resolved) {
            found = true;
            break;
        }
    }

    ::closedir(directory);
    return found;
}

bool waitForNoProcessUsingExecutable(const std::string &executable, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (anyProcessUsesExecutable(executable)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void testCommandLine()
{
    const sdl3mcp::CliParseResult parsed = sdl3mcp::parseCommandLine(
        {"--startup-timeout-ms", "250", "--terminate-timeout-ms", "750", "--target",
         "/bin/app", "--fullscreen", "-v"});
    CHECK(parsed.ok);
    CHECK(parsed.options.startupTimeoutMs == 250);
    CHECK(parsed.options.terminateTimeoutMs == 750);
    CHECK(parsed.options.target == "/bin/app");
    CHECK((parsed.options.targetArguments ==
           std::vector<std::string>{"--fullscreen", "-v"}));

    const sdl3mcp::CliParseResult defaults =
        sdl3mcp::parseCommandLine({"--target", "app"});
    CHECK(defaults.ok);
    CHECK(defaults.options.startupTimeoutMs == sdl3mcp::kDefaultStartupTimeoutMs);
    CHECK(defaults.options.terminateTimeoutMs == sdl3mcp::kDefaultTerminateTimeoutMs);
    CHECK(defaults.options.targetArguments.empty());

    CHECK(!sdl3mcp::parseCommandLine({}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--target"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--bogus"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"app"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--startup-timeout-ms", "0", "--target", "app"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--startup-timeout-ms", "10"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--terminate-timeout-ms", "0", "--target", "app"}).ok);
    CHECK(!sdl3mcp::parseCommandLine({"--terminate-timeout-ms", "10"}).ok);
}

void testToolListViews()
{
    /* Child mode: `spawn` plus the application's four tools, in every state. */
    sdl3mcp::SupervisorOptions childOptions;
    const nlohmann::json childBase = sdl3mcp::supervisorToolList(childOptions);
    CHECK((toolNames(nlohmann::json{{"result", childBase}}) ==
           std::vector<std::string>{"spawn", "sdl_info", "sdl_input", "sdl_capture",
                                    "sdl_terminate"}));

    /* The view is a function of the mode only: nothing else can change it. */
    CHECK(sdl3mcp::supervisorToolList(childOptions) == childBase);

    /* Relay mode: `wait_session` plus those same four, before any peer exists. */
    sdl3mcp::SupervisorOptions relayOptions;
    relayOptions.listenHost = "127.0.0.1";
    const nlohmann::json relayBase = sdl3mcp::supervisorToolList(relayOptions);
    const nlohmann::json relayResponse{{"result", relayBase}};
    CHECK((toolNames(relayResponse) ==
           std::vector<std::string>{"wait_session", "sdl_info", "sdl_input", "sdl_capture",
                                    "sdl_terminate"}));

    /* Spawner mode: one app_spawn_<alias> per target, then the same four. */
    sdl3mcp::SupervisorOptions spawnerOptions;
    spawnerOptions.connectHost = "127.0.0.1";
    spawnerOptions.connectPort = 1234;
    sdl3mcp::SpawnTarget target;
    target.alias = "alpha";
    target.argv = {"./alpha"};
    spawnerOptions.targets.push_back(target);
    CHECK((toolNames(nlohmann::json{{"result",
                                     sdl3mcp::supervisorToolList(spawnerOptions)}}) ==
           std::vector<std::string>{"app_spawn_alpha", "sdl_info", "sdl_input",
                                    "sdl_capture", "sdl_terminate"}));

    /* sdl_capture always carries the supervisor's PNG presentation. */
    const nlohmann::json *capture = findTool(relayResponse, "sdl_capture");
    CHECK(capture != nullptr);
    CHECK(capture->at("description").get<std::string>().find("PNG") != std::string::npos);
    const nlohmann::json &properties = capture->at("inputSchema").at("properties");
    CHECK(properties.size() == 2);
    CHECK(properties.contains("region"));
    CHECK(properties.contains("timeout_ms"));
    CHECK(properties["timeout_ms"]["minimum"] == 1);
    CHECK(properties["timeout_ms"]["maximum"] == 60000);

    /* Merging a bare application over the base is an identity. */
    CHECK(sdl3mcp::mergePeerTools(relayBase, sdl3mcp::applicationToolList()) == relayBase);

    /* The peer's own entries win by name, except for the supervisor's capture. */
    nlohmann::json peerTools = sdl3mcp::applicationToolList();
    for (nlohmann::json &tool : peerTools) {
        const std::string name = tool.value("name", std::string());
        if (name == "sdl_info") {
            tool["description"] = "peer info";
        } else if (name == "sdl_capture") {
            tool["description"] = "peer capture";
            tool["inputSchema"] = nlohmann::json{{"type", "object"}};
        }
    }
    peerTools.push_back(nlohmann::json{{"name", "app_extra"},
                                       {"description", "peer extra"},
                                       {"inputSchema", nlohmann::json{{"type", "object"}}}});

    const nlohmann::json merged = sdl3mcp::mergePeerTools(relayBase, peerTools);
    const nlohmann::json mergedResponse{{"result", merged}};
    CHECK((toolNames(mergedResponse) ==
           std::vector<std::string>{"wait_session", "sdl_info", "sdl_input", "sdl_capture",
                                    "sdl_terminate", "app_extra"}));
    const nlohmann::json *mergedInfo = findTool(mergedResponse, "sdl_info");
    CHECK(mergedInfo != nullptr);
    CHECK((*mergedInfo)["description"] == "peer info");
    const nlohmann::json *mergedCapture = findTool(mergedResponse, "sdl_capture");
    CHECK(mergedCapture != nullptr);
    CHECK((*mergedCapture)["description"] != "peer capture");
    CHECK(mergedCapture->at("inputSchema").at("properties").contains("region"));
}

void testIdleAdvertisesSpawnAndAppTools(const std::string &supervisor,
                                        const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild});

    const nlohmann::json initialize = client.initialize();
    CHECK(!initialize.contains("error"));
    CHECK(initialize["result"]["protocolVersion"] == "2025-11-25");
    CHECK(initialize["result"]["capabilities"]["tools"]["listChanged"] == true);
    CHECK(initialize["result"]["serverInfo"]["name"] == "sdl3mcp");
    CHECK(initialize["result"]["serverInfo"]["version"].is_string());

    /*
      Idle already advertises the application tools: the list depends on the
      mode, not on whether a child is attached, so a spawn cannot move it.
    */
    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"spawn", "sdl_info", "sdl_input",
                                            "sdl_capture", "sdl_terminate"}));
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

    const nlohmann::json idleList = client.toolsList();

    const nlohmann::json spawn = client.spawn();
    CHECK(!spawn.contains("error"));
    CHECK(spawn["result"]["isError"] == false);

    const std::string text = spawnText(spawn);
    CHECK(text.find("Fake Child Window") != std::string::npos);
    /* Geometry is sampled at the handshake, before the window manager settles it, so none is quoted. */
    CHECK(text.find("640") == std::string::npos);
    CHECK(text.find(" points") == std::string::npos);

    /* A spawn changes no tool list, so it announces no change either. */
    CHECK(client.listChangedCount() == 0);
    CHECK(client.toolsList()["result"].dump() == idleList["result"].dump());

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == std::vector<std::string>{"spawn", "sdl_info", "sdl_input",
                                            "sdl_capture", "sdl_terminate"}));

    /* Proxied sdl_info keeps the child's payload. */
    const nlohmann::json info = client.callTool("sdl_info");
    CHECK(info["result"]["isError"] == false);
    const std::string infoText = info["result"]["content"][0]["text"].get<std::string>();
    const nlohmann::json payload = nlohmann::json::parse(infoText);
    CHECK(payload["window"]["title"] == "Fake Child Window");
    CHECK(payload["window"]["width"] == 640);
    CHECK(payload["window"]["height"] == 480);

    /* Running advertises the supervisor's sdl_capture: region + timeout_ms, PNG. */
    const nlohmann::json list = client.toolsList();
    const nlohmann::json *captureTool = findTool(list, "sdl_capture");
    CHECK(captureTool != nullptr);
    const nlohmann::json &captureProperties = (*captureTool)["inputSchema"]["properties"];
    CHECK(captureProperties.size() == 2);
    CHECK(captureProperties.contains("region"));
    CHECK(captureProperties.contains("timeout_ms"));
    const std::string captureDescription = (*captureTool)["description"].get<std::string>();
    CHECK(captureDescription.find("PNG") != std::string::npos);
    CHECK(captureDescription.find("native pixel size") != std::string::npos);

    /* Proxied sdl_input keeps the child's content unchanged. */
    const nlohmann::json input =
        client.callTool("sdl_input", nlohmann::json{{"actions", nlohmann::json::array()}});
    CHECK(input["result"]["isError"] == false);
    CHECK(input["result"]["content"] ==
          nlohmann::json::array({
              nlohmann::json{{"type", "text"}, {"text", "fake sdl_input ok"}},
          }));

    /*
      A name the child does not advertise is refused by the supervisor instead
      of being forwarded, so it answers at once with the name in the message.
    */
    const nlohmann::json unknown = client.callTool("nope");
    CHECK(!unknown.contains("error"));
    CHECK(unknown["result"]["isError"] == true);
    CHECK(spawnText(unknown) == "the application does not provide nope");

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

/*
  sdl_capture through the proxy: the child sends a raw 64x32 frame and the
  client receives those same 64x32 pixels as image/png, with a summary naming
  the native size. Native size is the regression: the proxy must never resize.
*/
void testCaptureStaysNativeSize(const std::string &supervisor,
                                const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--capture-width", "64",
                              "--capture-height", "32"});
    CHECK(!client.initialize().contains("error"));
    CHECK(!client.spawn().contains("error"));

    const nlohmann::json capture = client.callTool("sdl_capture");
    CHECK(capture["result"]["isError"] == false);
    const nlohmann::json &content = capture["result"]["content"];
    CHECK(content.is_array());
    CHECK(content.size() == 2);
    CHECK(content[0]["type"] == "text");
    CHECK(content[1]["type"] == "image");
    CHECK(content[1]["mimeType"] == "image/png");

    const std::string text = content[0]["text"].get<std::string>();
    CHECK(text.rfind("64x32 PNG", 0) == 0);
    CHECK(text.find(" bytes") != std::string::npos);
    CHECK(text.find("frame 11 attached") != std::string::npos);
    CHECK(pngHasSize(decodeBase64(content[1]["data"].get<std::string>()), 64, 32));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/* The region reaches the child unchanged; the summary reports it. */
void testCaptureRegion(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--capture-width", "64",
                              "--capture-height", "32"});
    CHECK(!client.initialize().contains("error"));
    CHECK(!client.spawn().contains("error"));

    const nlohmann::json region{
        {"x", 0}, {"y", 0}, {"width", 16}, {"height", 8}};
    const nlohmann::json capture =
        client.callTool("sdl_capture", nlohmann::json{{"region", region},
                                                      {"timeout_ms", 2000}});
    CHECK(capture["result"]["isError"] == false);
    const nlohmann::json &content = capture["result"]["content"];
    CHECK(content.size() == 2);
    const std::string text = content[0]["text"].get<std::string>();
    CHECK(text.rfind("16x8 PNG at 0,0", 0) == 0);
    CHECK(text.find("frame 11 attached") != std::string::npos);
    CHECK(pngHasSize(decodeBase64(content[1]["data"].get<std::string>()), 16, 8));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/* A text error result from the child passes through with isError and no image. */
void testCaptureTextError(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--capture-error"});
    CHECK(!client.initialize().contains("error"));
    CHECK(!client.spawn().contains("error"));

    const nlohmann::json capture = client.callTool("sdl_capture");
    CHECK(!capture.contains("error"));
    CHECK(capture["result"]["isError"] == true);
    CHECK(capture["result"]["content"] ==
          nlohmann::json::array({
              nlohmann::json{{"type", "text"}, {"text", "fake capture failed"}},
          }));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/* A JSON-RPC error object from the child travels back unchanged. */
void testCaptureRpcError(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--capture-rpc-error"});
    CHECK(!client.initialize().contains("error"));
    CHECK(!client.spawn().contains("error"));

    const nlohmann::json capture = client.callTool("sdl_capture");
    CHECK(capture.contains("error"));
    CHECK(capture["error"]["code"] == -32603);
    CHECK(capture["error"]["message"] == "fake capture rpc failure");

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/* A header whose byteLength does not match the dimensions is rejected unread. */
void testCaptureMismatch(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--capture-width", "16",
                              "--capture-height", "16", "--capture-mismatch"});
    CHECK(!client.initialize().contains("error"));
    CHECK(!client.spawn().contains("error"));

    const nlohmann::json capture = client.callTool("sdl_capture");
    CHECK(!capture.contains("error"));
    CHECK(capture["result"]["isError"] == true);
    const std::string text = spawnText(capture);
    CHECK(text.find("byteLength") != std::string::npos);

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/*
  A 160x96 frame is 61440 payload bytes, far beyond the transport's 4096-byte
  read chunk, so this proves the header and payload are framed correctly across
  several reads.
*/
void testCaptureLargeFrame(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--capture-width", "160",
                              "--capture-height", "96"});
    CHECK(!client.initialize().contains("error"));
    CHECK(!client.spawn().contains("error"));

    const nlohmann::json capture = client.callTool("sdl_capture");
    CHECK(capture["result"]["isError"] == false);
    const nlohmann::json &content = capture["result"]["content"];
    CHECK(content.size() == 2);
    const std::string text = content[0]["text"].get<std::string>();
    CHECK(text.rfind("160x96 PNG", 0) == 0);
    CHECK(pngHasSize(decodeBase64(content[1]["data"].get<std::string>()), 160, 96));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/*
  sdl_input now answers with the frame that follows its batch. The fake child
  sends the raw framing plus the text naming the applied frame, and the proxy
  must turn that into exactly the same text-plus-image result it builds for
  sdl_capture, then stay in step for the next call.
*/
void testInputFrameThroughProxy(const std::string &supervisor,
                                const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--input-frame",
                              "--capture-width", "32", "--capture-height", "16"});
    CHECK(!client.initialize().contains("error"));
    CHECK(!client.spawn().contains("error"));

    const nlohmann::json input =
        client.callTool("sdl_input", nlohmann::json{{"actions", nlohmann::json::array()}});
    CHECK(!input.contains("error"));
    CHECK(input["result"]["isError"] == false);

    const nlohmann::json &content = input["result"]["content"];
    CHECK(content.is_array());
    CHECK(content.size() == 2);
    CHECK(content[0]["type"] == "text");
    const std::string inputText = content[0]["text"].get<std::string>();
    CHECK(inputText.find("applied at frame 7") != std::string::npos);
    CHECK(inputText.find("frame 9 attached") != std::string::npos);
    CHECK(inputText.find("rendered after the actions were delivered") !=
          std::string::npos);
    CHECK(content[1]["type"] == "image");
    CHECK(content[1]["mimeType"] == "image/png");
    CHECK(pngHasSize(decodeBase64(content[1]["data"].get<std::string>()), 32, 16));

    /* The payload was consumed whole: the next call's line parses normally. */
    const nlohmann::json info = client.callTool("sdl_info");
    CHECK(info["result"]["isError"] == false);
    const nlohmann::json payload = nlohmann::json::parse(spawnText(info));
    CHECK(payload["window"]["title"] == "Fake Child Window");

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/*
  sdl_input waits for the apply and then for the frame, so the proxy's default
  deadline must be longer than one frame timeout. A child that answers after
  2.5 s would have been detached by the old 2 s default.
*/
void testInputForwardDeadline(const std::string &supervisor,
                              const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--input-delay-ms", "2500"});
    CHECK(!client.initialize().contains("error"));
    CHECK(!client.spawn().contains("error"));

    const auto started = std::chrono::steady_clock::now();
    const nlohmann::json input = client.callTool(
        "sdl_input", nlohmann::json{{"actions", nlohmann::json::array()}}, 10000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    CHECK(!input.contains("error"));
    CHECK(input["result"]["isError"] == false);
    CHECK(elapsed >= 2500);
    CHECK(elapsed < 9000);

    /* The session survived the slow answer. */
    CHECK(client.callTool("sdl_info")["result"]["isError"] == false);

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

void testChildExitWhileRunning(const std::string &supervisor, const std::string &fakeChild)
{
    SupervisorClient client;
    client.start(supervisor, {"--target", fakeChild, "--exit-after-ms", "400"});
    CHECK(!client.initialize().contains("error"));

    const nlohmann::json spawn = client.spawn();
    CHECK(!spawn.contains("error"));
    CHECK(spawn["result"]["isError"] == false);

    /* The exit is silent: the advertised list never included the child's state. */
    const std::string runningList = client.toolsList()["result"].dump();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(client.notifications.empty());
    CHECK(client.toolsList()["result"].dump() == runningList);
    CHECK((toolNames(client.toolsList()) ==
           std::vector<std::string>{"spawn", "sdl_info", "sdl_input", "sdl_capture",
                                    "sdl_terminate"}));

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

    /* The failed startup changes nothing a client can list. */
    CHECK(client.listChangedCount() == 0);

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == childToolNames()));

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
    CHECK((names == childToolNames()));

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
    CHECK((names == childToolNames()));

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
    CHECK((names == childToolNames()));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/*
  The app answers sdl_terminate and exits; the supervisor waits for that exit
  and publishes Idle exactly once. No watchdog kill is needed.
*/
void testTerminateNormal(const std::string &supervisor,
                         const std::string &fakeChild,
                         const std::string &pidFile)
{
    SupervisorClient client;
    client.start(supervisor, {"--terminate-timeout-ms", "5000", "--target", fakeChild,
                              "--pid-file", pidFile});
    CHECK(!client.initialize().contains("error"));

    CHECK(!client.spawn().contains("error"));
    const pid_t childPid = readPidFile(pidFile);
    CHECK(childPid > 0);

    const nlohmann::json terminated = client.callTool("sdl_terminate");
    CHECK(terminated["result"]["isError"] == false);
    CHECK(spawnText(terminated) == "terminated");

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == childToolNames()));
    /* Neither the spawn nor the exit changed the list, so nothing was announced. */
    CHECK(client.listChangedCount() == 0);
    CHECK(!processIsAlive(childPid));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/*
  A child that ignores SIGTERM must still be gone: the watchdog SIGTERMs, waits
  its grace, then SIGKILLs, and the result says the termination was forced.
*/
void testTerminateWatchdog(const std::string &supervisor,
                           const std::string &fakeChild,
                           const std::string &pidFile)
{
    SupervisorClient client;
    client.start(supervisor, {"--terminate-timeout-ms", "300", "--target", fakeChild,
                              "--ignore-terminate", "--pid-file", pidFile});
    CHECK(!client.initialize().contains("error"));

    CHECK(!client.spawn().contains("error"));
    const pid_t childPid = readPidFile(pidFile);
    CHECK(childPid > 0);

    const auto started = std::chrono::steady_clock::now();
    const nlohmann::json terminated = client.callTool("sdl_terminate");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    CHECK(terminated["result"]["isError"] == false);
    CHECK(spawnText(terminated).find("termination was forced") != std::string::npos);
    /* The 300 ms budget plus the ~2 s grace, not an unbounded wait. */
    CHECK(elapsed >= 300);
    CHECK(elapsed < 10000);

    const std::vector<std::string> names = toolNames(client.toolsList());
    CHECK((names == childToolNames()));
    CHECK(client.listChangedCount() == 0);
    CHECK(!processIsAlive(childPid));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs));
}

/* SIGINT and SIGTERM end the supervisor without leaving the child behind. */
void testSignalsStopChild(const std::string &supervisor, const std::string &fakeChild)
{
    for (const int signalNumber : {SIGTERM, SIGINT}) {
        const std::string pidFile = makeTempPath();

        SupervisorClient client;
        client.start(supervisor, {"--target", fakeChild, "--pid-file", pidFile});
        CHECK(!client.initialize().contains("error"));
        CHECK(!client.spawn().contains("error"));

        const pid_t childPid = readPidFile(pidFile);
        CHECK(childPid > 0);
        CHECK(client.process.pid > 0);

        CHECK(::kill(client.process.pid, signalNumber) == 0);
        CHECK(client.process.waitForExit(kLineTimeoutMs));
        CHECK(!processIsAlive(childPid));

        ::unlink(pidFile.c_str());
    }
}

/*
  The real example through the proxy: no fake child, real SDL under the dummy
  driver, and the full spawn -> info -> input -> capture -> terminate round trip,
  ending back at `spawn` with no process left behind.
*/
void testRealExample(const std::string &supervisor, const std::string &example)
{
    SupervisorClient client;
    client.start(supervisor, {"--startup-timeout-ms", "15000",
                              "--terminate-timeout-ms", "10000", "--target", example,
                              "--mcp"});
    CHECK(!client.initialize().contains("error"));

    const std::vector<std::string> idleNames = toolNames(client.toolsList());
    CHECK((idleNames == childToolNames()));

    const nlohmann::json spawn = client.spawn(20000);
    CHECK(spawn["result"]["isError"] == false);
    CHECK(spawnText(spawn).find("SDL3MCP demo") != std::string::npos);
    /* The window manager settles the size after the handshake, so no geometry is quoted. */
    CHECK(spawnText(spawn).find(" points") == std::string::npos);
    CHECK(spawnText(spawn).find(" pixels") == std::string::npos);

    const std::vector<std::string> runningNames = toolNames(client.toolsList());
    for (const char *name : {"sdl_info", "sdl_input", "sdl_capture", "sdl_terminate"}) {
        bool found = false;
        for (const std::string &running : runningNames) {
            found = found || running == name;
        }
        CHECK(found);
    }

    const nlohmann::json info = client.callTool("sdl_info");
    CHECK(info["result"]["isError"] == false);
    const nlohmann::json payload = nlohmann::json::parse(spawnText(info));
    CHECK(payload["window"].is_object());
    CHECK(payload["window"]["pixelWidth"].is_number_integer());
    CHECK(payload["window"]["pixelWidth"].get<int>() > 0);
    CHECK(payload["window"]["pixelHeight"].get<int>() > 0);

    const nlohmann::json input = client.callTool(
        "sdl_input", nlohmann::json{{"actions", nlohmann::json::array({
                                                    nlohmann::json{{"type", "key"},
                                                                   {"key", "r"},
                                                                   {"action", "press"}},
                                                })}});
    CHECK(input["result"]["isError"] == false);
    CHECK(spawnText(input).find("applied") != std::string::npos);

    const nlohmann::json capture = client.callTool("sdl_capture");
    CHECK(capture["result"]["isError"] == false);
    CHECK(spawnText(capture).rfind("960x540 PNG", 0) == 0);

    bool sawImage = false;
    for (const nlohmann::json &block : capture["result"]["content"]) {
        if (block.value("type", std::string()) != "image") {
            continue;
        }
        sawImage = true;
        CHECK(block.value("mimeType", std::string()) == "image/png");
        CHECK(block.value("data", std::string()).size() > 100);
    }
    CHECK(sawImage);

    const nlohmann::json terminated = client.callTool("sdl_terminate");
    CHECK(terminated["result"]["isError"] == false);
    CHECK(spawnText(terminated) == "terminated");

    const std::vector<std::string> backToIdle = toolNames(client.toolsList());
    CHECK((backToIdle == childToolNames()));

    client.process.closeStdin();
    CHECK(client.process.waitForExit(kLineTimeoutMs * 2));
    CHECK(waitForNoProcessUsingExecutable(example, 3000));
}

/*
  `readme_check` runs this binary with --dump-tools to inspect the descriptions
  tools/list really carries. Emitting the catalog here keeps the documentation
  guard on the same data as the wire instead of on a copy in the README.
*/
void dumpToolDefinitions()
{
    nlohmann::json tools = nlohmann::json::array();
    for (const sdl3mcp::ToolDefinition &tool : sdl3mcp::toolCatalog()) {
        tools.push_back(nlohmann::json{
            {"name", tool.name},
            {"description", tool.description},
            {"inputSchema", tool.inputSchema},
        });
    }

    tools.push_back(sdl3mcp::spawnToolDefinition());
    tools.push_back(sdl3mcp::waitSessionToolDefinition());
    tools.push_back(sdl3mcp::supervisorCaptureToolDefinition());

    sdl3mcp::SpawnTarget target;
    target.alias = "demo";
    target.argv = {"./demo"};
    tools.push_back(sdl3mcp::spawnerToolDefinition(target));

    std::printf("%s\n", tools.dump().c_str());
}

} // namespace

TEST_CASE("command line")
{
    testCommandLine();
}

TEST_CASE("tool list views")
{
    testToolListViews();
}

TEST_CASE("idle advertises spawn and app tools")
{
    testIdleAdvertisesSpawnAndAppTools(g_supervisor, g_fakeChild);
}

TEST_CASE("spawn and proxy")
{
    const std::string pidFile = makeTempPath();
    testSpawnAndProxy(g_supervisor, g_fakeChild, pidFile);
    ::unlink(pidFile.c_str());
}

TEST_CASE("captures stay at the native size")
{
    testCaptureStaysNativeSize(g_supervisor, g_fakeChild);
}

TEST_CASE("capture region")
{
    testCaptureRegion(g_supervisor, g_fakeChild);
}

TEST_CASE("capture text error")
{
    testCaptureTextError(g_supervisor, g_fakeChild);
}

TEST_CASE("capture rpc error")
{
    testCaptureRpcError(g_supervisor, g_fakeChild);
}

TEST_CASE("capture mismatch")
{
    testCaptureMismatch(g_supervisor, g_fakeChild);
}

TEST_CASE("capture large frame")
{
    testCaptureLargeFrame(g_supervisor, g_fakeChild);
}

TEST_CASE("input frame through the proxy")
{
    testInputFrameThroughProxy(g_supervisor, g_fakeChild);
}

TEST_CASE("input forward deadline")
{
    testInputForwardDeadline(g_supervisor, g_fakeChild);
}

TEST_CASE("child exits while running")
{
    testChildExitWhileRunning(g_supervisor, g_fakeChild);
}

TEST_CASE("child exits during startup")
{
    testChildExitDuringStartup(g_supervisor, g_fakeChild);
}

TEST_CASE("initialize failure")
{
    testInitializeFailure(g_supervisor, g_fakeChild);
}

TEST_CASE("nonexistent target")
{
    testNonexistentTarget(g_supervisor);
}

TEST_CASE("window timeout")
{
    testWindowTimeout(g_supervisor, g_fakeChild);
}

TEST_CASE("terminate normal")
{
    const std::string pidFile = makeTempPath();
    testTerminateNormal(g_supervisor, g_fakeChild, pidFile);
    ::unlink(pidFile.c_str());
}

TEST_CASE("terminate watchdog")
{
    const std::string pidFile = makeTempPath();
    testTerminateWatchdog(g_supervisor, g_fakeChild, pidFile);
    ::unlink(pidFile.c_str());
}

TEST_CASE("signals stop the child")
{
    testSignalsStopChild(g_supervisor, g_fakeChild);
}

/* The second CTest registration adds the real example as argv[3]. */
TEST_CASE("the real example through the proxy")
{
    if (!g_example.empty()) {
        testRealExample(g_supervisor, g_example);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--dump-tools") {
        dumpToolDefinitions();
        return 0;
    }

    /* Doctest owns argv once it runs, so it is given only the binary name. The
       context also makes a failing argument check outside a case abort, as the
       hand-rolled harness did. */
    doctest::Context context(1, argv);
    context.setAsDefaultForAssertsOutOfTestCases();
    CHECK(argc >= 3);

    g_supervisor = argv[1];
    g_fakeChild = argv[2];
    if (argc >= 4) {
        g_example = argv[3];
    }
    return context.run();
}
