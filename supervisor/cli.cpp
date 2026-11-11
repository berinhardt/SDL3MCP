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
constexpr std::string_view kWebRootOption = "--web-root";
constexpr std::string_view kWebRootPrefix = "--web-root=";
constexpr std::string_view kWebEntryOption = "--web-entry";
constexpr std::string_view kWebEntryPrefix = "--web-entry=";
constexpr std::string_view kWebPortOption = "--web-port";
constexpr std::string_view kWebPortPrefix = "--web-port=";
constexpr std::string_view kBridgePortOption = "--bridge-port";
constexpr std::string_view kBridgePortPrefix = "--bridge-port=";
constexpr std::string_view kPrintPlanOption = "--print-plan";

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

bool parsePort(std::string_view text, std::uint16_t &port)
{
    if (text.empty()) {
        return false;
    }

    unsigned parsed = 0;
    const char *begin = text.data();
    const char *end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 1 || parsed > 65535) {
        return false;
    }

    port = static_cast<std::uint16_t>(parsed);
    return true;
}

} // namespace

CliParseResult parseCommandLine(const std::vector<std::string> &arguments)
{
    CliParseResult result;
    bool webEntryGiven = false;
    bool webPortGiven = false;
    bool bridgePortGiven = false;
    bool printPlanGiven = false;

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
            if (result.options.web()) {
                result.error = "--web-root and --target cannot be combined";
                return result;
            }
            if (result.options.relay()) {
                result.error = "--listen and --target cannot be combined";
                return result;
            }
            if (result.options.spawner() || !result.options.targetsPaths.empty()) {
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
            if (result.options.web()) {
                result.error = "--web-root and --listen cannot be combined";
                return result;
            }
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
            result.error = "--targets requires a file or directory written as --targets=PATH";
            return result;
        }

        if (argument.rfind(kTargetsPrefix, 0) == 0) {
            if (result.options.web()) {
                result.error = "--web-root and --targets cannot be combined";
                return result;
            }
            const std::size_t valueStart = kTargetsOption.size() + 1;
            const std::string_view value(argument.data() + valueStart,
                                         argument.size() - valueStart);
            if (value.empty()) {
                result.error =
                    "--targets requires a file or directory written as --targets=PATH";
                return result;
            }
            result.options.targetsPaths.emplace_back(value);
            index += 1;
            continue;
        }

        if (argument == kWebRootOption) {
            result.error = "--web-root requires a directory written as --web-root=DIR";
            return result;
        }

        if (argument.rfind(kWebRootPrefix, 0) == 0) {
            if (!result.options.targetsPaths.empty()) {
                result.error = "--web-root and --targets cannot be combined";
                return result;
            }
            if (result.options.relay()) {
                result.error = "--web-root and --listen cannot be combined";
                return result;
            }
            if (!result.options.target.empty()) {
                result.error = "--web-root and --target cannot be combined";
                return result;
            }
            if (!result.options.webRoot.empty()) {
                result.error = "--web-root may be given only once";
                return result;
            }

            const std::size_t valueStart = kWebRootOption.size() + 1;
            const std::string_view value(argument.data() + valueStart,
                                         argument.size() - valueStart);
            if (value.empty()) {
                result.error = "--web-root requires a directory written as --web-root=DIR";
                return result;
            }
            result.options.webRoot.assign(value);
            index += 1;
            continue;
        }

        if (argument == kWebEntryOption) {
            result.error = "--web-entry requires a page name written as --web-entry=NAME";
            return result;
        }

        if (argument.rfind(kWebEntryPrefix, 0) == 0) {
            if (webEntryGiven) {
                result.error = "--web-entry may be given only once";
                return result;
            }

            const std::size_t valueStart = kWebEntryOption.size() + 1;
            const std::string_view value(argument.data() + valueStart,
                                         argument.size() - valueStart);
            if (value.empty()) {
                result.error = "--web-entry requires a page name written as --web-entry=NAME";
                return result;
            }
            result.options.webEntry.assign(value);
            webEntryGiven = true;
            index += 1;
            continue;
        }

        if (argument == kWebPortOption) {
            result.error = "--web-port requires a port written as --web-port=PORT";
            return result;
        }

        if (argument.rfind(kWebPortPrefix, 0) == 0) {
            if (webPortGiven) {
                result.error = "--web-port may be given only once";
                return result;
            }

            const std::size_t valueStart = kWebPortOption.size() + 1;
            const std::string_view value(argument.data() + valueStart,
                                         argument.size() - valueStart);
            if (!parsePort(value, result.options.webPort)) {
                result.error = "invalid value for --web-port: expected 1..65535";
                return result;
            }
            webPortGiven = true;
            index += 1;
            continue;
        }

        if (argument == kBridgePortOption) {
            result.error = "--bridge-port requires a port written as --bridge-port=PORT";
            return result;
        }

        if (argument.rfind(kBridgePortPrefix, 0) == 0) {
            if (bridgePortGiven) {
                result.error = "--bridge-port may be given only once";
                return result;
            }

            const std::size_t valueStart = kBridgePortOption.size() + 1;
            const std::string_view value(argument.data() + valueStart,
                                         argument.size() - valueStart);
            if (!parsePort(value, result.options.bridgePort)) {
                result.error = "invalid value for --bridge-port: expected 1..65535";
                return result;
            }
            bridgePortGiven = true;
            index += 1;
            continue;
        }

        if (argument == kPrintPlanOption) {
            if (printPlanGiven) {
                result.error = "--print-plan may be given only once";
                return result;
            }
            result.options.printPlan = true;
            printPlanGiven = true;
            index += 1;
            continue;
        }

        result.error = "unexpected argument: " + argument;
        return result;
    }

    if (webEntryGiven && result.options.webRoot.empty()) {
        result.error = "--web-entry requires --web-root=DIR";
        return result;
    }
    if (webPortGiven && !result.options.web()) {
        result.error = "--web-port requires --web-root=DIR";
        return result;
    }
    if (bridgePortGiven && !result.options.web()) {
        result.error = "--bridge-port requires --web-root=DIR";
        return result;
    }
    if (printPlanGiven && !result.options.web()) {
        result.error = "--print-plan requires --web-root=DIR";
        return result;
    }

    if (result.options.web()) {
        result.ok = true;
        return result;
    }

    if (!result.options.targetsPaths.empty() && result.options.connectHost.empty()) {
        result.error = "--targets requires --connect=HOST:PORT";
        return result;
    }
    if (!result.options.connectHost.empty() && result.options.targetsPaths.empty()) {
        result.error = "--connect requires --targets=PATH";
        return result;
    }

    if (result.options.spawner()) {
        if (!loadSpawnTargets(result.options.targetsPaths, result.options.targets,
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

    result.error = "missing --target <executable>, --listen=HOST:PORT, "
                   "--connect=HOST:PORT --targets=PATH or --web-root=DIR";
    return result;
}

std::string usageLine()
{
    return "usage: sdl3mcp [--startup-timeout-ms N] [--terminate-timeout-ms N] "
           "(--target <executable> [app args...] | --listen=HOST:PORT | "
           "--connect=HOST:PORT --targets=PATH [--targets=PATH ...] "
           "[--listen=HOST:PORT] | --web-root=DIR [--web-entry=NAME] "
           "[--web-port=PORT] [--bridge-port=PORT] [--connect=HOST:PORT] "
           "[--print-plan])";
}

} // namespace sdl3mcp
