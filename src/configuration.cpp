#include "configuration.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
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

} // namespace

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
    if (argument == "--mcp-help") {
        message_ = usageText();
        return Status::ExitSuccess;
    }
    if (argument == "--mcp-version") {
        message_ = versionText();
        return Status::ExitSuccess;
    }

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
    if (equalsIgnoringCase(value, "stdio")) {
        candidate.transport = SDL3MCP_TRANSPORT_STDIO;
        return Status::Continue;
    }
    if (equalsIgnoringCase(value, "off")) {
        candidate.transport = SDL3MCP_TRANSPORT_NONE;
        return Status::Continue;
    }
    return fail("invalid value for '" + std::string(argument)
                + "': expected 'stdio' or 'off'");
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
    if (candidate.transport != SDL3MCP_TRANSPORT_NONE
        && candidate.transport != SDL3MCP_TRANSPORT_STDIO) {
        return fail("transport must be SDL3MCP_TRANSPORT_NONE or SDL3MCP_TRANSPORT_STDIO");
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

std::string Configuration::usageText()
{
    return "SDL3MCP command line options:\n"
           "  --mcp                     Start the stdio MCP server (same as --mcp=stdio).\n"
           "  --mcp=stdio               Start the stdio MCP server.\n"
           "  --mcp=off                 Start no server.\n"
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

} // namespace sdl3mcp
