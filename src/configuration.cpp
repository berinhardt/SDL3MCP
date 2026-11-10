#include "configuration.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace sdl3mcp
{
namespace
{

using Status = Configuration::Status;

constexpr std::string_view kMcpPrefix = "--mcp";
constexpr char kValueSeparator = '=';

constexpr uint32_t kMinFrameTimeoutMs = 1u;
constexpr uint32_t kMaxFrameTimeoutMs = 60000u;
constexpr uint32_t kMinMaxFrameBytes = 1024u * 1024u;
constexpr uint32_t kMaxMaxFrameBytes = 1024u * 1024u * 1024u;
constexpr int kMinLogLevel = -1;
constexpr int kMaxLogLevel = 4;

bool equalsIgnoringCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

bool parseDecimal(std::string_view text, std::uint64_t &out)
{
    constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();

    if (text.empty()) {
        return false;
    }

    std::uint64_t result = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (result > (max - digit) / 10u) {
            return false;
        }
        result = result * 10u + digit;
    }

    out = result;
    return true;
}

bool isValueFlag(std::string_view name)
{
    return name == "--mcp" || name == "--mcp-timeout-ms" || name == "--mcp-max-frame-bytes"
           || name == "--mcp-log-level";
}

constexpr std::size_t kConnectHostCapacity = 64;

/*
  Dialling the client is one concept, implemented with a socket on desktop and
  with a WebSocket in the browser.
*/
#ifdef __EMSCRIPTEN__
constexpr SDL3MCP_Transport kDialTransport = SDL3MCP_TRANSPORT_WEBSOCKET;
#else
constexpr SDL3MCP_Transport kDialTransport = SDL3MCP_TRANSPORT_CONNECT;
#endif

} // namespace

bool parseEndpoint(std::string_view value,
                   std::string_view &host,
                   std::uint16_t &port,
                   bool allowZeroPort)
{
    std::string_view portText;
    if (!value.empty() && value.front() == '[') {
        const std::size_t closing = value.find(']');
        if (closing == std::string_view::npos || closing == 1) {
            return false;
        }
        if (closing + 1 >= value.size() || value[closing + 1] != ':') {
            return false;
        }
        host = value.substr(1, closing - 1);
        portText = value.substr(closing + 2);
    } else {
        const std::size_t separator = value.find(':');
        if (separator == std::string_view::npos || separator == 0) {
            return false;
        }
        if (value.find(':', separator + 1) != std::string_view::npos) {
            return false;
        }
        host = value.substr(0, separator);
        portText = value.substr(separator + 1);
    }

    if (host.empty() || host.size() >= kConnectHostCapacity) {
        return false;
    }

    std::uint64_t parsed = 0;
    const std::uint64_t minimum = allowZeroPort ? 0u : 1u;
    if (!parseDecimal(portText, parsed) || parsed < minimum || parsed > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

Configuration::Configuration() noexcept : value_(SDL3MCP_DefaultConfiguration())
{
}

Configuration::Configuration(const SDL3MCP_Configuration &source) noexcept : value_(source)
{
}

Status Configuration::parseCommandLine(int argc, char **argv)
{
    message_.clear();

    SDL3MCP_Configuration candidate = value_;
    if (argv != nullptr) {
        for (int index = 1; index < argc; ++index) {
            if (argv[index] == nullptr) {
                continue;
            }
            const std::string_view argument(argv[index]);
            if (!argument.starts_with(kMcpPrefix)) {
                continue;
            }
            const Status status = applyArgument(candidate, argument);
            if (status != Status::Continue) {
                return status;
            }
        }
    }

    const Status status = validateValues(candidate);
    if (status != Status::Continue) {
        return status;
    }

    value_ = candidate;
    return Status::Continue;
}

Status Configuration::validate()
{
    message_.clear();
    return validateValues(value_);
}

Status Configuration::applyArgument(SDL3MCP_Configuration &candidate, std::string_view argument)
{
    if (argument == "--mcp") {
        candidate.transport = SDL3MCP_TRANSPORT_STDIO;
        return Status::Continue;
    }
    if (argument == "--mcp-no-warp-cursor") {
        candidate.warp_cursor = false;
        return Status::Continue;
    }
    /*
      A browser receives one synthesised --mcp=<value> argument and has no argv,
      so neither flag can arrive there; only the desktop build carries them.
    */
#ifndef __EMSCRIPTEN__
    if (argument == "--mcp-help") {
        message_ = usageText();
        return Status::ExitSuccess;
    }
    if (argument == "--mcp-version") {
        message_ = versionText();
        return Status::ExitSuccess;
    }
#endif

    const std::size_t separator = argument.find(kValueSeparator);
    if (separator == std::string_view::npos) {
        if (isValueFlag(argument)) {
            return fail("argument '" + std::string(argument) + "' needs a value written as '"
                        + std::string(argument) + "=VALUE'");
        }
        return fail("unknown SDL3MCP argument '" + std::string(argument) + "'");
    }

    const std::string_view name = argument.substr(0, separator);
    const std::string_view value = argument.substr(separator + 1);
    if (name == "--mcp") {
        return applyTransport(argument, value, candidate);
    }
    if (name == "--mcp-timeout-ms") {
        return applyFrameTimeout(argument, value, candidate);
    }
    if (name == "--mcp-max-frame-bytes") {
        return applyMaxFrameBytes(argument, value, candidate);
    }
    if (name == "--mcp-log-level") {
        return applyLogLevel(argument, value, candidate);
    }
    return fail("unknown SDL3MCP argument '" + std::string(argument) + "'");
}

Status Configuration::applyTransport(std::string_view argument,
                                     std::string_view value,
                                     SDL3MCP_Configuration &candidate)
{
    /* A browser has no stdio server, so the value does not exist there. */
#ifndef __EMSCRIPTEN__
    if (equalsIgnoringCase(value, "stdio")) {
        candidate.transport = SDL3MCP_TRANSPORT_STDIO;
        return Status::Continue;
    }
#endif
    if (equalsIgnoringCase(value, "off")) {
        candidate.transport = SDL3MCP_TRANSPORT_NONE;
        return Status::Continue;
    }

    std::string_view host;
    std::uint16_t port = 0;
    if (parseEndpoint(value, host, port)) {
        std::memset(candidate.connect_host, 0, sizeof(candidate.connect_host));
        std::memcpy(candidate.connect_host, host.data(), host.size());
        candidate.connect_port = port;
        candidate.transport = kDialTransport;
        return Status::Continue;
    }

#ifdef __EMSCRIPTEN__
    return fail("invalid value for '" + std::string(argument)
                + "': expected 'off' or HOST:PORT");
#else
    return fail("invalid value for '" + std::string(argument)
                + "': expected 'stdio', 'off', HOST:PORT or [V6]:PORT");
#endif
}

Status Configuration::applyFrameTimeout(std::string_view argument,
                                        std::string_view value,
                                        SDL3MCP_Configuration &candidate)
{
    std::uint64_t parsed = 0;
    if (!parseDecimal(value, parsed) || parsed < kMinFrameTimeoutMs
        || parsed > kMaxFrameTimeoutMs) {
        return fail("invalid value '" + std::string(value) + "' for '" + std::string(argument)
                    + "': expected a decimal number in 1..60000");
    }
    candidate.frame_timeout_ms = static_cast<uint32_t>(parsed);
    return Status::Continue;
}

Status Configuration::applyMaxFrameBytes(std::string_view argument,
                                         std::string_view value,
                                         SDL3MCP_Configuration &candidate)
{
    std::uint64_t parsed = 0;
    if (!parseDecimal(value, parsed) || parsed < kMinMaxFrameBytes
        || parsed > kMaxMaxFrameBytes) {
        return fail("invalid value '" + std::string(value) + "' for '" + std::string(argument)
                    + "': expected a decimal number in 1048576..1073741824");
    }
    candidate.max_frame_bytes = static_cast<uint32_t>(parsed);
    return Status::Continue;
}

Status Configuration::applyLogLevel(std::string_view argument,
                                    std::string_view value,
                                    SDL3MCP_Configuration &candidate)
{
    struct LevelName
    {
        std::string_view name;
        int level;
    };
    constexpr LevelName levels[] = {
        {"off", 0},
        {"error", 1},
        {"warn", 2},
        {"info", 3},
        {"debug", 4},
    };

    for (const LevelName &entry : levels) {
        if (equalsIgnoringCase(value, entry.name)) {
            candidate.log_level = entry.level;
            return Status::Continue;
        }
    }
    return fail("invalid value '" + std::string(value) + "' for '" + std::string(argument)
                + "': expected off, error, warn, info or debug");
}

Status Configuration::validateValues(const SDL3MCP_Configuration &candidate)
{
#ifdef __EMSCRIPTEN__
    if (candidate.transport != SDL3MCP_TRANSPORT_NONE
        && candidate.transport != SDL3MCP_TRANSPORT_WEBSOCKET) {
        return fail("transport must be SDL3MCP_TRANSPORT_NONE or SDL3MCP_TRANSPORT_WEBSOCKET");
    }
#else
    if (candidate.transport != SDL3MCP_TRANSPORT_NONE
        && candidate.transport != SDL3MCP_TRANSPORT_STDIO
        && candidate.transport != SDL3MCP_TRANSPORT_CONNECT) {
        return fail("transport must be SDL3MCP_TRANSPORT_NONE, SDL3MCP_TRANSPORT_STDIO or "
                    "SDL3MCP_TRANSPORT_CONNECT");
    }
#endif
    if (candidate.transport == kDialTransport) {
        const void *terminator =
            std::memchr(candidate.connect_host, '\0', sizeof(candidate.connect_host));
        if (terminator == nullptr) {
            return fail("connect_host must be a non-empty string shorter than 64 bytes");
        }
        const std::size_t hostLength =
            static_cast<std::size_t>(static_cast<const char *>(terminator) - candidate.connect_host);
        if (hostLength == 0) {
            return fail("connect_host must be a non-empty string shorter than 64 bytes");
        }
        if (candidate.connect_port == 0) {
            return fail("connect_port must be in 1..65535, got 0");
        }
    }
    if (candidate.frame_timeout_ms < kMinFrameTimeoutMs
        || candidate.frame_timeout_ms > kMaxFrameTimeoutMs) {
        return fail("frame_timeout_ms must be in 1..60000, got "
                    + std::to_string(candidate.frame_timeout_ms));
    }
    if (candidate.max_frame_bytes < kMinMaxFrameBytes
        || candidate.max_frame_bytes > kMaxMaxFrameBytes) {
        return fail("max_frame_bytes must be in 1048576..1073741824, got "
                    + std::to_string(candidate.max_frame_bytes));
    }
    if (candidate.log_level < kMinLogLevel || candidate.log_level > kMaxLogLevel) {
        return fail("log_level must be in -1..4, got " + std::to_string(candidate.log_level));
    }
    return Status::Continue;
}

Status Configuration::fail(std::string message)
{
    message_ = std::move(message);
    return Status::Error;
}

/* Desktop only: a browser has no argv, so this text has no reader there. */
#ifndef __EMSCRIPTEN__
std::string Configuration::usageText()
{
    return "SDL3MCP command line options:\n"
           "  --mcp                     Start the stdio MCP server (same as --mcp=stdio).\n"
           "  --mcp=stdio               Start the stdio MCP server.\n"
           "  --mcp=off                 Start no server.\n"
           "  --mcp=HOST:PORT           Connect to an MCP client at HOST:PORT.\n"
           "  --mcp=[V6]:PORT           Connect to an IPv6 MCP client.\n"
           "  --mcp-timeout-ms=N        Frame timeout in milliseconds (1..60000).\n"
           "  --mcp-max-frame-bytes=N   Maximum frame size in bytes (1048576..1073741824).\n"
           "  --mcp-no-warp-cursor      Do not warp the cursor for pointer actions.\n"
           "  --mcp-log-level=LEVEL     off, error, warn, info or debug.\n"
           "  --mcp-help                Print this help and exit successfully.\n"
           "  --mcp-version             Print the library version and exit successfully.\n"
           "The MCP server is off by default; launch with --mcp to enable it.";
}

std::string Configuration::versionText()
{
    return "SDL3MCP " SDL3MCP_VERSION_STRING;
}
#endif

} // namespace sdl3mcp
