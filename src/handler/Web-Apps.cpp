#include "Web-Apps.h"

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

/*
  Reads one JSON object and appends its validated entries. `sources` remembers
  which file first defined each alias, so a later file that repeats one is an
  error instead of a silent replacement.
*/
Result<void, ProtocolError> appendWebFile(
    const std::string& path,
    WebApps& loaded,
    std::map<std::string, std::string>& sources) {
   std::ifstream file(path, std::ios::binary);
   if (!file) {
      return ProtocolError{"cannot open web file \"" + path +
                           "\": " + std::strerror(errno)};
   }

   const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
   if (document.is_discarded()) {
      return ProtocolError{"web file \"" + path + "\" is not valid JSON"};
   }
   if (!document.is_object()) {
      return ProtocolError{"web file \"" + path +
                           "\" must contain a JSON object"};
   }
   if (document.empty()) {
      return ProtocolError{"web file \"" + path + "\" defines no aliases"};
   }

   for (auto entry = document.begin(); entry != document.end(); ++entry) {
      const std::string alias = entry.key();
      if (isReservedAlias(alias)) {
         return ProtocolError{"web entry \"" + alias +
                              "\" must not use the reserved app_spawn_ prefix"};
      }
      if (!isValidAlias(alias)) {
         return ProtocolError{"web alias \"" + alias +
                              "\" must match [a-z0-9_]+"};
      }
      if (!entry->is_string()) {
         return ProtocolError{"web entry \"" + alias +
                              "\" must be a non-empty string"};
      }

      std::string root = entry->get<std::string>();
      if (root.empty()) {
         return ProtocolError{"web entry \"" + alias +
                              "\" must be a non-empty string"};
      }

      const auto existing = sources.find(alias);
      if (existing != sources.end()) {
         return ProtocolError{"web alias \"" + alias +
                              "\" is defined in both \"" + existing->second +
                              "\" and \"" + path + "\""};
      }

      sources.emplace(alias, path);
      loaded.push_back(WebApp{alias, std::move(root), path});
   }

   return {};
}

}  // namespace

Result<WebApps, ProtocolError> loadWebApps(
    const std::vector<std::string>& paths) {
   if (paths.empty()) {
      return ProtocolError{"no web sources were given"};
   }

   WebApps loaded;
   std::map<std::string, std::string> sources;
   for (const std::string& path : paths) {
      struct stat info{};
      if (::stat(path.c_str(), &info) != 0) {
         return ProtocolError{"cannot open web file \"" + path +
                              "\": " + std::strerror(errno)};
      }
      if (!S_ISREG(info.st_mode)) {
         return ProtocolError{"web path \"" + path + "\" is not a file"};
      }
      const Result<void, ProtocolError> appended =
          appendWebFile(path, loaded, sources);
      if (!appended) {
         return appended.error();
      }
   }

   return loaded;
}

}  // namespace sdl3mcp
