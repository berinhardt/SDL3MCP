#include "cli.hpp"

#include "configuration.hpp"

#include <charconv>
#include <string>
#include <string_view>

namespace sdl3mcp
{
namespace
{

constexpr std::string_view kStartupTimeoutOption = "--startup-timeout-ms";
constexpr std::string_view kTerminateTimeoutOption = "--terminate-timeout-ms";
constexpr std::string_view kTargetOption = "--target";
constexpr std::string_view kListenOption = "--listen";
constexpr std::string_view kListenPrefix = "--listen=";
constexpr std::string_view kConnectOption = "--connect";
constexpr std::string_view kConnectPrefix = "--connect=";
constexpr std::string_view kTargetsOption = "--targets";
constexpr std::string_view kTargetsPrefix = "--targets=";

bool parsePositiveInteger(std::string_view text, int &value)
{
    if (text.empty()) {
        return false;
    }

    int parsed = 0;
    const char *begin = text.data();
    const char *end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 1) {
        return false;
    }

    value = parsed;
    return true;
}

} // namespace

CliParseResult parseCommandLine(const std::vector<std::string> &arguments)
{
    CliParseResult result;

    size_t index = 0;
    while (index < arguments.size()) {
        const std::string &argument = arguments[index];

        if (argument == kStartupTimeoutOption) {
            if (index + 1 >= arguments.size()) {
                result.error = "--startup-timeout-ms requires a value";
                return result;
            }
            if (!parsePositiveInteger(arguments[index + 1], result.options.startupTimeoutMs)) {
                result.error = "--startup-timeout-ms requires a positive integer";
                return result;
            }
            index += 2;
            continue;
        }

        if (argument == kTerminateTimeoutOption) {
            if (index + 1 >= arguments.size()) {
                result.error = "--terminate-timeout-ms requires a value";
                return result;
            }
            if (!parsePositiveInteger(arguments[index + 1],
                                      result.options.terminateTimeoutMs)) {
                result.error = "--terminate-timeout-ms requires a positive integer";
                return result;
            }
            index += 2;
            continue;
        }

        if (argument == kTargetOption) {
            if (result.options.relay()) {
                result.error = "--listen and --target cannot be combined";
                return result;
            }
            if (result.options.spawner() || !result.options.targetsPath.empty()) {
                result.error = "--connect and --target cannot be combined";
                return result;
            }
            if (index + 1 >= arguments.size() || arguments[index + 1].empty()) {
                result.error = "--target requires an executable";
                return result;
            }
            result.options.target = arguments[index + 1];
            result.options.targetArguments.assign(arguments.begin() + static_cast<long>(index) + 2,
                                                  arguments.end());
            result.ok = true;
            return result;
        }

        if (argument == kListenOption) {
            result.error = "--listen requires an address written as --listen=HOST:PORT";
            return result;
        }

        if (argument.rfind(kListenPrefix, 0) == 0) {
            if (!result.options.listenHost.empty()) {
                result.error = "--listen may be given only once";
                return result;
            }

            const std::size_t valueStart = kListenOption.size() + 1;
            const std::string_view value(argument.data() + valueStart,
                                         argument.size() - valueStart);
            std::string_view host;
            std::uint16_t port = 0;
            /* Port 0 asks the kernel for an ephemeral port; the bound one is printed. */
            if (!parseEndpoint(value, host, port, true)) {
                result.error = "invalid value for --listen: expected HOST:PORT or [V6]:PORT";
                return result;
            }
            result.options.listenHost.assign(host.data(), host.size());
            result.options.listenPort = port;
            index += 1;
            continue;
        }

        if (argument == kConnectOption) {
            result.error = "--connect requires an address written as --connect=HOST:PORT";
            return result;
        }

        if (argument.rfind(kConnectPrefix, 0) == 0) {
            if (!result.options.connectHost.empty()) {
                result.error = "--connect may be given only once";
                return result;
            }

            const std::size_t valueStart = kConnectOption.size() + 1;
            const std::string_view value(argument.data() + valueStart,
                                         argument.size() - valueStart);
            std::string_view host;
            std::uint16_t port = 0;
            if (!parseEndpoint(value, host, port)) {
                result.error = "invalid value for --connect: expected HOST:PORT or [V6]:PORT";
                return result;
            }
            result.options.connectHost.assign(host.data(), host.size());
            result.options.connectPort = port;
            index += 1;
            continue;
        }

        if (argument == kTargetsOption) {
            result.error = "--targets requires a file written as --targets=FILE";
            return result;
        }

        if (argument.rfind(kTargetsPrefix, 0) == 0) {
            if (!result.options.targetsPath.empty()) {
                result.error = "--targets may be given only once";
                return result;
            }

            const std::size_t valueStart = kTargetsOption.size() + 1;
            const std::string_view value(argument.data() + valueStart,
                                         argument.size() - valueStart);
            if (value.empty()) {
                result.error = "--targets requires a file written as --targets=FILE";
                return result;
            }
            result.options.targetsPath.assign(value.data(), value.size());
            index += 1;
            continue;
        }

        result.error = "unexpected argument: " + argument;
        return result;
    }

    if (!result.options.targetsPath.empty() && result.options.connectHost.empty()) {
        result.error = "--targets requires --connect=HOST:PORT";
        return result;
    }
    if (!result.options.connectHost.empty() && result.options.targetsPath.empty()) {
        result.error = "--connect requires --targets=FILE";
        return result;
    }

    if (result.options.spawner()) {
        if (!loadSpawnTargets(result.options.targetsPath, result.options.targets,
                              result.error)) {
            return result;
        }
        result.ok = true;
        return result;
    }

    if (result.options.relay()) {
        result.ok = true;
        return result;
    }

    result.error = "missing --target <executable>, --listen=HOST:PORT or "
                   "--connect=HOST:PORT --targets=FILE";
    return result;
}

std::string usageLine()
{
    return "usage: sdl3mcp [--startup-timeout-ms N] [--terminate-timeout-ms N] "
           "(--target <executable> [app args...] | --listen=HOST:PORT | "
           "--connect=HOST:PORT --targets=FILE [--listen=HOST:PORT])";
}

} // namespace sdl3mcp
