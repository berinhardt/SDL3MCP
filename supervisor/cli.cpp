#include "cli.hpp"

#include <charconv>
#include <string_view>

namespace sdl3mcp
{
namespace
{

constexpr std::string_view kStartupTimeoutOption = "--startup-timeout-ms";
constexpr std::string_view kTerminateTimeoutOption = "--terminate-timeout-ms";
constexpr std::string_view kTargetOption = "--target";

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

        result.error = "unexpected argument: " + argument;
        return result;
    }

    result.error = "missing --target <executable>";
    return result;
}

std::string usageLine()
{
    return "usage: sdl3mcp-supervisor [--startup-timeout-ms N] "
           "[--terminate-timeout-ms N] --target <executable> [app args...]";
}

} // namespace sdl3mcp
