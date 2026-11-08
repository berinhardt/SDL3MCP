#include "targets.hpp"

#include <nlohmann/json.hpp>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace sdl3mcp
{
namespace
{

constexpr std::string_view kToolPrefix = "app_spawn_";
constexpr std::string_view kJsonExtension = ".json";

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

bool endsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/*
  Reads one JSON object and appends its validated entries. `sources` remembers
  which file first defined each alias, so a later source that repeats one is an
  error instead of a silent replacement.
*/
bool appendTargetsFile(const std::string &path, SpawnTargets &loaded,
                       std::map<std::string, std::string> &sources, std::string &error)
{
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

        const auto existing = sources.find(alias);
        if (existing != sources.end()) {
            error = "targets alias \"" + alias + "\" is defined in both \"" +
                    existing->second + "\" and \"" + path + "\"";
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

        sources.emplace(alias, path);
        loaded.push_back(std::move(target));
    }

    return true;
}

/*
  Resolves one --targets value to the files it contributes: the file itself, or
  every top-level *.json inside a directory, sorted so the result is stable.
*/
bool collectTargetFiles(const std::string &path, std::vector<std::string> &files,
                        std::string &error)
{
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        error = "cannot open targets path \"" + path + "\": " + std::strerror(errno);
        return false;
    }

    if (S_ISREG(info.st_mode)) {
        files.push_back(path);
        return true;
    }
    if (!S_ISDIR(info.st_mode)) {
        error = "targets path \"" + path + "\" is neither a file nor a directory";
        return false;
    }

    DIR *directory = ::opendir(path.c_str());
    if (directory == nullptr) {
        error = "cannot open targets directory \"" + path + "\": " + std::strerror(errno);
        return false;
    }

    std::vector<std::string> found;
    while (const dirent *entry = ::readdir(directory)) {
        const std::string_view name(entry->d_name);
        if (name == "." || name == ".." || !endsWith(name, kJsonExtension)) {
            continue;
        }

        std::string candidate = path;
        if (!candidate.empty() && candidate.back() != '/') {
            candidate.push_back('/');
        }
        candidate.append(name);

        struct stat entryInfo{};
        if (::stat(candidate.c_str(), &entryInfo) == 0 && S_ISDIR(entryInfo.st_mode)) {
            continue;
        }
        found.push_back(std::move(candidate));
    }
    ::closedir(directory);

    if (found.empty()) {
        error = "targets directory \"" + path + "\" contains no *.json files";
        return false;
    }

    std::sort(found.begin(), found.end());
    files.insert(files.end(), found.begin(), found.end());
    return true;
}

} // namespace

bool loadSpawnTargets(const std::vector<std::string> &paths, SpawnTargets &targets,
                      std::string &error)
{
    targets.clear();
    error.clear();

    if (paths.empty()) {
        error = "no targets sources were given";
        return false;
    }

    SpawnTargets loaded;
    std::map<std::string, std::string> sources;
    for (const std::string &path : paths) {
        std::vector<std::string> files;
        if (!collectTargetFiles(path, files, error)) {
            return false;
        }
        for (const std::string &file : files) {
            if (!appendTargetsFile(file, loaded, sources, error)) {
                return false;
            }
        }
    }

    if (loaded.empty()) {
        error = "targets source \"" + paths.front() + "\" defines no aliases";
        return false;
    }

    targets = std::move(loaded);
    return true;
}

bool loadSpawnTargets(const std::string &path, SpawnTargets &targets, std::string &error)
{
    return loadSpawnTargets(std::vector<std::string>{path}, targets, error);
}

} // namespace sdl3mcp
