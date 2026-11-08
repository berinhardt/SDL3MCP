/*
  A fake MCP child for the supervisor tests: it speaks the same JSON-RPC lines as
  the real app but never touches SDL, a window, or a renderer.

  sdl_info reports a window that appears --window-delay-ms after start (or never
  with --no-window). sdl_input returns fixed text, or with --input-frame the same
  raw framing a real application now sends: one result line carrying
  format/width/height/byteLength plus the text naming the applied frame,
  followed by exactly that many RGBA bytes. --input-delay-ms postpones that
  answer so the proxy's forward deadline can be exercised. sdl_capture answers
  with the same binary framing minus the text. --capture-width/--capture-height
  set its size, a region in the arguments overrides them, --capture-error and
  --capture-rpc-error reproduce the two child failure shapes, and
  --capture-mismatch announces a byteLength that does not match the dimensions
  and sends no payload. --exit-after-ms / --exit-code / --fail-initialize
  reproduce the process failure paths. Before every deliberate exit it writes one
  recognisable stderr line, so the supervisor's failure text can be checked for
  recent child output.

  sdl_terminate answers "termination requested" and exits, like the real
  library. --ignore-terminate keeps running instead (SIGTERM is ignored until
  SIGKILL), so the supervisor's terminate watchdog can be exercised.

  --mcp=HOST:PORT makes it dial that endpoint instead of using stdin/stdout, so
  it can stand in for an application spawned by the spawner. --dial-delay-ms
  postpones that dial (a spawner timeout test) and --exit-before-connect exits
  without dialing (a spawner child-exits-first test). --initialize-delay-ms
  postpones the answer to initialize, so a test can prove a spawn waits for the
  handshake instead of returning when the connection is accepted.

  --pid-file exists only so a test can prove the child is gone after the
  supervisor stops; it writes the child's own pid.
*/

#include <nlohmann/json.hpp>

#include "tools.hpp"

#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

constexpr int kPollMs = 25;
constexpr std::string_view kProtocolVersion = "2025-11-25";

/* stdio by default, or the dialed MCP socket when --mcp=HOST:PORT was given. */
int g_inputFd = STDIN_FILENO;
std::FILE *g_output = stdout;

struct Options
{
    int windowDelayMs = 0;
    int exitAfterMs = -1;
    int exitCode = 0;
    int captureWidth = 8;
    int captureHeight = 8;
    int inputDelayMs = 0;
    bool failInitialize = false;
    bool inputFrame = false;
    bool noWindow = false;
    bool ignoreTerminate = false;
    bool captureTextError = false;
    bool captureRpcError = false;
    bool captureMismatch = false;
    std::string pidFile;
    std::string mcpAddress;
    int dialDelayMs = 0;
    int initializeDelayMs = 0;
    bool exitBeforeConnect = false;
};

bool parseInteger(std::string_view text, int &value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parseOptions(int argc, char **argv, Options &options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const bool hasValue = index + 1 < argc;

        if (argument == "--window-delay-ms" && hasValue) {
            if (!parseInteger(argv[++index], options.windowDelayMs)) {
                return false;
            }
        } else if (argument == "--exit-after-ms" && hasValue) {
            if (!parseInteger(argv[++index], options.exitAfterMs)) {
                return false;
            }
        } else if (argument == "--exit-code" && hasValue) {
            if (!parseInteger(argv[++index], options.exitCode)) {
                return false;
            }
        } else if (argument == "--pid-file" && hasValue) {
            options.pidFile = argv[++index];
        } else if (argument.rfind("--mcp=", 0) == 0) {
            options.mcpAddress = std::string(argument.substr(6));
        } else if (argument == "--dial-delay-ms" && hasValue) {
            if (!parseInteger(argv[++index], options.dialDelayMs) ||
                options.dialDelayMs < 0) {
                return false;
            }
        } else if (argument == "--initialize-delay-ms" && hasValue) {
            if (!parseInteger(argv[++index], options.initializeDelayMs) ||
                options.initializeDelayMs < 0) {
                return false;
            }
        } else if (argument == "--exit-before-connect") {
            options.exitBeforeConnect = true;
        } else if (argument == "--input-delay-ms" && hasValue) {
            if (!parseInteger(argv[++index], options.inputDelayMs) ||
                options.inputDelayMs < 0) {
                return false;
            }
        } else if (argument == "--input-frame") {
            options.inputFrame = true;
        } else if (argument == "--capture-width" && hasValue) {
            if (!parseInteger(argv[++index], options.captureWidth) ||
                options.captureWidth <= 0) {
                return false;
            }
        } else if (argument == "--capture-height" && hasValue) {
            if (!parseInteger(argv[++index], options.captureHeight) ||
                options.captureHeight <= 0) {
                return false;
            }
        } else if (argument == "--fail-initialize") {
            options.failInitialize = true;
        } else if (argument == "--no-window") {
            options.noWindow = true;
        } else if (argument == "--ignore-terminate") {
            options.ignoreTerminate = true;
        } else if (argument == "--capture-error") {
            options.captureTextError = true;
        } else if (argument == "--capture-rpc-error") {
            options.captureRpcError = true;
        } else if (argument == "--capture-mismatch") {
            options.captureMismatch = true;
        } else {
            return false;
        }
    }
    return true;
}

/* Splits HOST:PORT or [V6]:PORT; the fake child only ever sees spawner endpoints. */
bool splitAddress(const std::string &text, std::string &host, std::string &port)
{
    if (!text.empty() && text.front() == '[') {
        const std::size_t closing = text.find(']');
        if (closing == std::string::npos || closing + 2 >= text.size() ||
            text[closing + 1] != ':') {
            return false;
        }
        host = text.substr(1, closing - 1);
        port = text.substr(closing + 2);
        return true;
    }

    const std::size_t separator = text.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= text.size()) {
        return false;
    }
    host = text.substr(0, separator);
    port = text.substr(separator + 1);
    return true;
}

int dialAddress(const std::string &text)
{
    std::string host;
    std::string port;
    if (!splitAddress(text, host, port)) {
        return -1;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *addresses = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
        return -1;
    }

    int fd = -1;
    for (const addrinfo *address = addresses; address != nullptr;
         address = address->ai_next) {
        const int candidate =
            ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0) {
            continue;
        }
        if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
            fd = candidate;
            break;
        }
        ::close(candidate);
    }

    ::freeaddrinfo(addresses);
    return fd;
}

void writePidFile(const std::string &path)
{
    if (path.empty()) {
        return;
    }

    std::FILE *file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "%d\n", static_cast<int>(::getpid()));
    std::fclose(file);
}

void writeMessage(const nlohmann::json &message)
{
    std::string line = message.dump();
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), g_output);
    std::fflush(g_output);
}

void writeError(const nlohmann::json &id, int code, std::string_view message)
{
    writeMessage(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", nlohmann::json{{"code", code}, {"message", std::string(message)}}},
    });
}

void writeResult(const nlohmann::json &id, nlohmann::json result)
{
    writeMessage(nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void writeTextResult(const nlohmann::json &id, std::string_view text)
{
    writeResult(id, nlohmann::json{
                        {"content", nlohmann::json::array({
                                        nlohmann::json{{"type", "text"},
                                                       {"text", std::string(text)}},
                                    })},
                        {"isError", false},
                    });
}

/*
  The raw-framing result: one header line naming the payload size, then exactly
  that many RGBA bytes with no newline. `text` is empty for a capture; sdl_input
  supplies the line naming the frame it was applied at. The mismatch mode
  announces a size one pixel larger and writes nothing, so the supervisor must
  reject it before reading.
*/
void writeFrameResult(const nlohmann::json &id, std::string_view text, int width,
                      int height, bool mismatched)
{
    const std::size_t byteLength = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4;
    const std::size_t announced = mismatched ? byteLength + 4 : byteLength;

    nlohmann::json header{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", nlohmann::json{
             {"format", "rgba8888"},
             {"width", width},
             {"height", height},
             {"byteLength", announced},
         }},
    };
    if (!text.empty()) {
        header["result"]["text"] = std::string(text);
    }
    std::string line = header.dump();
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), g_output);

    if (!mismatched) {
        std::vector<uint8_t> pixels(byteLength);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                uint8_t *out = pixels.data() +
                               (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(x)) *
                                   4;
                out[0] = static_cast<uint8_t>((x * 8 + y * 3) & 0xFF);
                out[1] = static_cast<uint8_t>((y * 5) & 0xFF);
                out[2] = static_cast<uint8_t>((x * 2) & 0xFF);
                out[3] = 255;
            }
        }
        std::fwrite(pixels.data(), 1, pixels.size(), g_output);
    }
    std::fflush(g_output);
}

/*
  The library's own catalog, so the fake child looks like a real application: the
  supervisor's merge over its base is then an identity, which is what the spawn
  view tests assert.
*/
nlohmann::json toolsResult()
{
    nlohmann::json tools = nlohmann::json::array();
    for (const sdl3mcp::ToolDefinition &tool : sdl3mcp::toolCatalog()) {
        tools.push_back(nlohmann::json{
            {"name", std::string(tool.name)},
            {"description", std::string(tool.description)},
            {"inputSchema", tool.inputSchema},
            {"annotations", tool.annotations},
        });
    }
    return nlohmann::json{{"tools", std::move(tools)}};
}

nlohmann::json infoText(const Options &options,
                        std::chrono::steady_clock::duration elapsed)
{
    nlohmann::json window = nullptr;
    const bool visible = !options.noWindow &&
                         std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                                 .count() >= options.windowDelayMs;
    if (visible) {
        window = nlohmann::json{
            {"title", "Fake Child Window"},
            {"width", 640},
            {"height", 480},
            {"pixelWidth", 640},
            {"pixelHeight", 480},
            {"pixelDensity", 1.0},
            {"relativeMouseMode", false},
        };
    }

    const nlohmann::json info{
        {"sdlVersion", "fake"},
        {"videoDriver", nullptr},
        {"window", window},
        {"frame", 0},
    };
    return nlohmann::json{
        {"content", nlohmann::json::array({
                        nlohmann::json{{"type", "text"}, {"text", info.dump()}},
                    })},
        {"isError", false},
    };
}

void exitWithLine(std::string_view reason, int code)
{
    std::fprintf(stderr, "fake-child: exiting: %.*s (code %d)\n",
                 static_cast<int>(reason.size()), reason.data(), code);
    std::fflush(stderr);
    std::exit(code);
}

void handleToolsCall(const nlohmann::json &id,
                     const nlohmann::json &params,
                     const Options &options,
                     std::chrono::steady_clock::duration elapsed)
{
    const auto name = params.find("name");
    if (name == params.end() || !name->is_string()) {
        writeError(id, -32602, "tools/call requires a string name");
        return;
    }

    const std::string tool = name->get<std::string>();
    if (tool == "sdl_info") {
        writeResult(id, infoText(options, elapsed));
        return;
    }
    if (tool == "sdl_input") {
        if (options.inputDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.inputDelayMs));
        }
        if (options.inputFrame) {
            writeFrameResult(id, "1 actions applied at frame 7", options.captureWidth,
                             options.captureHeight, false);
            return;
        }
        writeTextResult(id, "fake sdl_input ok");
        return;
    }
    if (tool == "sdl_capture") {
        if (options.captureTextError) {
            writeResult(id, nlohmann::json{
                                {"content", nlohmann::json::array({
                                                nlohmann::json{{"type", "text"},
                                                               {"text", "fake capture failed"}},
                                            })},
                                {"isError", true},
                            });
            return;
        }
        if (options.captureRpcError) {
            writeError(id, -32603, "fake capture rpc failure");
            return;
        }

        int width = options.captureWidth;
        int height = options.captureHeight;
        const auto arguments = params.find("arguments");
        if (arguments != params.end() && arguments->is_object()) {
            const auto region = arguments->find("region");
            if (region != arguments->end() && region->is_object()) {
                const auto regionWidth = region->find("width");
                const auto regionHeight = region->find("height");
                if (regionWidth != region->end() && regionWidth->is_number_integer() &&
                    regionHeight != region->end() && regionHeight->is_number_integer()) {
                    width = regionWidth->get<int>();
                    height = regionHeight->get<int>();
                }
            }
        }
        writeFrameResult(id, {}, width, height, options.captureMismatch);
        return;
    }
    if (tool == "sdl_terminate") {
        writeTextResult(id, "termination requested");
        if (!options.ignoreTerminate) {
            exitWithLine("terminate requested", options.exitCode);
        }
        return;
    }

    writeError(id, -32602, "Unknown tool: " + tool);
}

void handleMessage(const nlohmann::json &message,
                   const Options &options,
                   std::chrono::steady_clock::duration elapsed)
{
    if (!message.is_object()) {
        return;
    }

    const auto method = message.find("method");
    const auto id = message.find("id");
    if (method == message.end() || !method->is_string() || id == message.end()) {
        /* A notification, or something we do not answer. */
        return;
    }

    const std::string name = method->get<std::string>();
    const nlohmann::json params =
        message.contains("params") ? message["params"] : nlohmann::json::object();

    if (name == "initialize") {
        if (options.failInitialize) {
            writeError(*id, -32603, "the fake child cannot initialize");
            return;
        }
        const auto version = params.find("protocolVersion");
        if (version == params.end() || !version->is_string()) {
            writeError(*id, -32602, "initialize requires a string protocolVersion");
            return;
        }
        /* A late answer, so a spawner that waits for the handshake cannot pass early. */
        if (options.initializeDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.initializeDelayMs));
        }
        writeResult(*id, nlohmann::json{
                             {"protocolVersion", std::string(kProtocolVersion)},
                             {"capabilities", nlohmann::json{{"tools", nlohmann::json::object()}}},
                             {"serverInfo", nlohmann::json{
                                  {"name", "sdl3mcp-fake-child"},
                                  {"version", "0.0.0"},
                              }},
                         });
        return;
    }
    if (name == "ping") {
        writeResult(*id, nlohmann::json::object());
        return;
    }
    if (name == "tools/list") {
        writeResult(*id, toolsResult());
        return;
    }
    if (name == "tools/call") {
        handleToolsCall(*id, params, options, elapsed);
        return;
    }

    writeError(*id, -32601, "Method not found: " + name);
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::fprintf(stderr, "fake-child: invalid arguments\n");
        return 2;
    }

    if (options.ignoreTerminate) {
        ::signal(SIGTERM, SIG_IGN);
    }

    writePidFile(options.pidFile);

    if (options.exitBeforeConnect) {
        exitWithLine("exiting before connect", options.exitCode);
    }

    if (!options.mcpAddress.empty()) {
        if (options.dialDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.dialDelayMs));
        }
        const int fd = dialAddress(options.mcpAddress);
        if (fd < 0) {
            std::fprintf(stderr, "fake-child: cannot dial %s\n",
                         options.mcpAddress.c_str());
            return 1;
        }
        g_inputFd = fd;
        g_output = ::fdopen(fd, "w");
        if (g_output == nullptr) {
            return 1;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    std::string pending;

    for (;;) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (options.exitAfterMs >= 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >=
                options.exitAfterMs) {
            exitWithLine("timed exit", options.exitCode);
        }

        pollfd descriptor{};
        descriptor.fd = g_inputFd;
        descriptor.events = POLLIN;

        const int ready = ::poll(&descriptor, 1, kPollMs);
        if (ready < 0) {
            return 1;
        }
        if (ready == 0) {
            continue;
        }

        char chunk[4096];
        const ssize_t count = ::read(g_inputFd, chunk, sizeof chunk);
        if (count < 0) {
            return 1;
        }
        if (count == 0) {
            exitWithLine("stdin closed", options.exitCode);
        }

        pending.append(chunk, static_cast<size_t>(count));
        size_t newline = pending.find('\n');
        while (newline != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);

            const nlohmann::json message =
                nlohmann::json::parse(line, nullptr, false);
            if (!message.is_discarded()) {
                handleMessage(message, options,
                              std::chrono::steady_clock::now() - started);
            }

            newline = pending.find('\n');
        }
    }
}
