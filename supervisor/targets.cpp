#include "targets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace sdl3mcp
{
namespace
{

constexpr std::string_view kToolPrefix = "app_spawn_";

bool isAliasCharacter(char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_';
}

bool isValidAlias(const std::string &alias)
{
    return !alias.empty() &&
           std::all_of(alias.begin(), alias.end(), isAliasCharacter);
}

bool isReservedAlias(const std::string &alias)
{
    return alias.size() >= kToolPrefix.size() &&
           alias.compare(0, kToolPrefix.size(), kToolPrefix) == 0;
}

} // namespace

bool loadSpawnTargets(const std::string &path, SpawnTargets &targets, std::string &error)
{
    targets.clear();
    error.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open targets file \"" + path + "\": " + std::strerror(errno);
        return false;
    }

    const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
    if (document.is_discarded()) {
        error = "targets file \"" + path + "\" is not valid JSON";
        return false;
    }
    if (!document.is_object()) {
        error = "targets file \"" + path + "\" must contain a JSON object";
        return false;
    }
    if (document.empty()) {
        error = "targets file \"" + path + "\" defines no aliases";
        return false;
    }

    SpawnTargets loaded;
    for (auto entry = document.begin(); entry != document.end(); ++entry) {
        const std::string alias = entry.key();
        if (isReservedAlias(alias)) {
            error = "targets entry \"" + alias +
                    "\" must not use the reserved app_spawn_ prefix";
            return false;
        }
        if (!isValidAlias(alias)) {
            error = "targets alias \"" + alias + "\" must match [a-z0-9_]+";
            return false;
        }
        if (!entry->is_array() || entry->empty()) {
            error = "targets entry \"" + alias +
                    "\" must be a non-empty array of strings";
            return false;
        }

        SpawnTarget target;
        target.alias = alias;
        for (const nlohmann::json &value : *entry) {
            if (!value.is_string()) {
                error = "targets entry \"" + alias +
                        "\" must be a non-empty array of strings";
                return false;
            }
            std::string argument = value.get<std::string>();
            if (argument.find("--mcp") != std::string::npos) {
                error = "targets entry \"" + alias + "\" already contains --mcp in \"" +
                        argument + "\"";
                return false;
            }
            target.argv.push_back(std::move(argument));
        }

        loaded.push_back(std::move(target));
    }

    targets = std::move(loaded);
    return true;
}

} // namespace sdl3mcp
