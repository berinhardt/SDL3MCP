#include "Targets.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace sdl3mcp {
namespace {

constexpr std::string_view kToolPrefix = "app_spawn_";
constexpr std::string_view kJsonExtension = ".json";

bool isAliasCharacter(char character) {
   return (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '_';
}

bool isValidAlias(const std::string& alias) {
   return !alias.empty() &&
          std::all_of(alias.begin(), alias.end(), isAliasCharacter);
}

bool isReservedAlias(const std::string& alias) {
   return alias.size() >= kToolPrefix.size() &&
          alias.compare(0, kToolPrefix.size(), kToolPrefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) {
   return text.size() >= suffix.size() &&
          text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/*
  Reads one JSON object and appends its validated entries. `sources` remembers
  which file first defined each alias, so a later source that repeats one is an
  error instead of a silent replacement.
*/
Result<void, ProtocolError> appendTargetsFile(
    const std::string& path,
    SpawnTargets& loaded,
    std::map<std::string, std::string>& sources) {
   std::ifstream file(path, std::ios::binary);
   if (!file) {
      return ProtocolError{"cannot open targets file \"" + path +
                           "\": " + std::strerror(errno)};
   }

   const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
   if (document.is_discarded()) {
      return ProtocolError{"targets file \"" + path +
                           "\" is not valid JSON"};
   }
   if (!document.is_object()) {
      return ProtocolError{"targets file \"" + path +
                           "\" must contain a JSON object"};
   }
   if (document.empty()) {
      return ProtocolError{"targets file \"" + path +
                           "\" defines no aliases"};
   }

   for (auto entry = document.begin(); entry != document.end(); ++entry) {
      const std::string alias = entry.key();
      if (isReservedAlias(alias)) {
         return ProtocolError{"targets entry \"" + alias +
                              "\" must not use the reserved app_spawn_ prefix"};
      }
      if (!isValidAlias(alias)) {
         return ProtocolError{"targets alias \"" + alias +
                              "\" must match [a-z0-9_]+"};
      }
      if (!entry->is_array() || entry->empty()) {
         return ProtocolError{"targets entry \"" + alias +
                              "\" must be a non-empty array of strings"};
      }

      const auto existing = sources.find(alias);
      if (existing != sources.end()) {
         return ProtocolError{"targets alias \"" + alias +
                              "\" is defined in both \"" + existing->second +
                              "\" and \"" + path + "\""};
      }

      SpawnTarget target;
      target.alias = alias;
      target.source = path;
      for (const nlohmann::json& value : *entry) {
         if (!value.is_string()) {
            return ProtocolError{"targets entry \"" + alias +
                                 "\" must be a non-empty array of strings"};
         }
         std::string argument = value.get<std::string>();
         if (argument.find("--mcp") != std::string::npos) {
            return ProtocolError{"targets entry \"" + alias +
                                 "\" already contains --mcp in \"" +
                                 argument + "\""};
         }
         target.argv.push_back(std::move(argument));
      }

      sources.emplace(alias, path);
      loaded.push_back(std::move(target));
   }

   return {};
}

/*
  Resolves one --targets value to the files it contributes: the file itself, or
  every top-level *.json inside a directory, sorted so the result is stable.
*/
Result<void, ProtocolError> collectTargetFiles(
    const std::string& path, std::vector<std::string>& files) {
   struct stat info{};
   if (::stat(path.c_str(), &info) != 0) {
      return ProtocolError{"cannot open targets path \"" + path +
                           "\": " + std::strerror(errno)};
   }

   if (S_ISREG(info.st_mode)) {
      files.push_back(path);
      return {};
   }
   if (!S_ISDIR(info.st_mode)) {
      return ProtocolError{"targets path \"" + path +
                           "\" is neither a file nor a directory"};
   }

   DIR* directory = ::opendir(path.c_str());
   if (directory == nullptr) {
      return ProtocolError{"cannot open targets directory \"" + path +
                           "\": " + std::strerror(errno)};
   }

   std::vector<std::string> found;
   while (const dirent* entry = ::readdir(directory)) {
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
      if (::stat(candidate.c_str(), &entryInfo) == 0 &&
          S_ISDIR(entryInfo.st_mode)) {
         continue;
      }
      found.push_back(std::move(candidate));
   }
   ::closedir(directory);

   if (found.empty()) {
      return ProtocolError{"targets directory \"" + path +
                           "\" contains no *.json files"};
   }

   std::sort(found.begin(), found.end());
   files.insert(files.end(), found.begin(), found.end());
   return {};
}

}  // namespace

Result<SpawnTargets, ProtocolError> loadSpawnTargets(
    const std::vector<std::string>& paths) {
   if (paths.empty()) {
      return ProtocolError{"no targets sources were given"};
   }

   SpawnTargets loaded;
   std::map<std::string, std::string> sources;
   for (const std::string& path : paths) {
      std::vector<std::string> files;
      const Result<void, ProtocolError> collected =
          collectTargetFiles(path, files);
      if (!collected) {
         return collected.error();
      }
      for (const std::string& file : files) {
         const Result<void, ProtocolError> appended =
             appendTargetsFile(file, loaded, sources);
         if (!appended) {
            return appended.error();
         }
      }
   }

   if (loaded.empty()) {
      return ProtocolError{"targets source \"" + paths.front() +
                           "\" defines no aliases"};
   }

   return loaded;
}

Result<SpawnTargets, ProtocolError> loadSpawnTargets(const std::string& path) {
   return loadSpawnTargets(std::vector<std::string>{path});
}

std::vector<std::string> targetCommand(const SpawnTarget& target,
                                       std::string_view endpoint) {
   std::vector<std::string> command = target.argv;
   command.push_back("--mcp=" + std::string(endpoint));
   return command;
}

}  // namespace sdl3mcp
