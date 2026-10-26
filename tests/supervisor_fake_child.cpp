/*
  A fake MCP child for the supervisor tests: it speaks the same JSON-RPC lines as
  the real app but never touches SDL, a window, or a renderer.

  sdl_info reports a window that appears --window-delay-ms after start (or never
  with --no-window). sdl_input returns fixed text, and sdl_capture answers with a
  real binary raw frame: one result line carrying format/width/height/byteLength
  followed by exactly that many RGBA bytes. --capture-width/--capture-height set
  its size, a region in the arguments overrides them, --capture-error and
  --capture-rpc-error reproduce the two child failure shapes, and
  --capture-mismatch announces a byteLength that does not match the dimensions
  and sends no payload. --exit-after-ms / --exit-code / --fail-initialize
  reproduce the process failure paths. Before every deliberate exit it writes one
  recognisable stderr line, so the supervisor's failure text can be checked for
  recent child output.

  sdl_terminate answers "termination requested" and exits, like the real
  library. --ignore-terminate keeps running instead (SIGTERM is ignored until
  SIGKILL), so the supervisor's terminate watchdog can be exercised.

  --pid-file exists only so a test can prove the child is gone after the
  supervisor stops; it writes the child's own pid.
*/

#include <nlohmann/json.hpp>

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr int kPollMs = 25;
constexpr std::string_view kProtocolVersion = "2025-11-25";

struct Options
{
    int windowDelayMs = 0;
    int exitAfterMs = -1;
    int exitCode = 0;
    int captureWidth = 8;
    int captureHeight = 8;
    bool failInitialize = false;
    bool noWindow = false;
    bool ignoreTerminate = false;
    bool captureTextError = false;
    bool captureRpcError = false;
    bool captureMismatch = false;
    std::string pidFile;
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
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
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
  The raw-framing capture result: one header line naming the payload size, then
  exactly that many RGBA bytes with no newline. The mismatch mode announces a
  size one pixel larger and writes nothing, so the supervisor must reject it
  before reading.
*/
void writeCaptureFrame(const nlohmann::json &id, int width, int height, bool mismatched)
{
    const std::size_t byteLength = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4;
    const std::size_t announced = mismatched ? byteLength + 4 : byteLength;

    const nlohmann::json header{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", nlohmann::json{
             {"format", "rgba8888"},
             {"width", width},
             {"height", height},
             {"byteLength", announced},
         }},
    };
    std::string line = header.dump();
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stdout);

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
        std::fwrite(pixels.data(), 1, pixels.size(), stdout);
    }
    std::fflush(stdout);
}

nlohmann::json toolsResult()
{
    nlohmann::json tools = nlohmann::json::array();
    for (const std::string_view name :
         {"sdl_info", "sdl_input", "sdl_capture", "sdl_terminate"}) {
        tools.push_back(nlohmann::json{
            {"name", std::string(name)},
            {"description", "fake child tool"},
            {"inputSchema", nlohmann::json{{"type", "object"}}},
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
        writeCaptureFrame(id, width, height, options.captureMismatch);
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
        descriptor.fd = STDIN_FILENO;
        descriptor.events = POLLIN;

        const int ready = ::poll(&descriptor, 1, kPollMs);
        if (ready < 0) {
            return 1;
        }
        if (ready == 0) {
            continue;
        }

        char chunk[4096];
        const ssize_t count = ::read(STDIN_FILENO, chunk, sizeof chunk);
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
