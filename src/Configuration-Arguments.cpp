#include "Configuration.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "Configuration-Limits.h"

namespace sdl3mcp {
namespace {

using Status = Configuration::Status;

constexpr char kValueSeparator = '=';

bool equalsIgnoringCase(std::string_view left, std::string_view right) {
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

bool isValueFlag(std::string_view name) {
   return name == "--mcp" || name == "--mcp-max-frame-bytes" ||
          name == "--mcp-log-level";
}

}  // namespace

Configuration::Status applyPageQuery(Configuration& configuration,
                                     std::string_view query) {
   const std::optional<std::string> value = mcpQueryValue(query);
   if (!value.has_value()) {
      return configuration.validate();
   }

   char program[] = "page";
   std::string argument = "--mcp=" + *value;
   char* argv[] = {program, argument.data(), nullptr};
   return configuration.parseCommandLine(2, argv);
}

Status Configuration::applyArgument(SDL3MCP_Configuration& candidate,
                                    std::string_view argument) {
   if (argument == "--mcp-no-warp-cursor") {
      candidate.warp_cursor = false;
      return Status::Continue;
   }
#ifndef __EMSCRIPTEN__
   /*
     A browser receives one synthesised --mcp=<value> argument and has no argv,
     so neither flag can arrive there; only the desktop build carries them.
   */
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
         return fail("argument '" + std::string(argument) + "' needs a value written as '" +
                     std::string(argument) + "=VALUE'");
      }
      return fail("unknown SDL3MCP argument '" + std::string(argument) + "'");
   }

   const std::string_view name = argument.substr(0, separator);
   const std::string_view value = argument.substr(separator + 1);
   if (name == "--mcp") {
      return applyEndpoint(argument, value, candidate);
   }
   if (name == "--mcp-max-frame-bytes") {
      return applyMaxFrameBytes(argument, value, candidate);
   }
   if (name == "--mcp-log-level") {
      return applyLogLevel(argument, value, candidate);
   }
   return fail("unknown SDL3MCP argument '" + std::string(argument) + "'");
}

Status Configuration::applyEndpoint(std::string_view argument,
                                    std::string_view value,
                                    SDL3MCP_Configuration& candidate) {
   if (equalsIgnoringCase(value, "off")) {
      std::memset(candidate.connect_host, 0, sizeof(candidate.connect_host));
      candidate.connect_port = 0;
      return Status::Continue;
   }

   std::string_view host;
   std::uint16_t port = 0;
   if (parseEndpoint(value, host, port, false)) {
      std::memset(candidate.connect_host, 0, sizeof(candidate.connect_host));
      std::memcpy(candidate.connect_host, host.data(), host.size());
      candidate.connect_port = port;
      return Status::Continue;
   }

   return fail("invalid value for '" + std::string(argument) +
               "': expected 'off' or HOST:PORT");
}

Status Configuration::applyMaxFrameBytes(std::string_view argument,
                                         std::string_view value,
                                         SDL3MCP_Configuration& candidate) {
   std::uint64_t parsed = 0;
   if (!parseDecimal(value, parsed) || parsed < kMinMaxFrameBytes ||
       parsed > kMaxMaxFrameBytes) {
      return fail("invalid value '" + std::string(value) + "' for '" +
                  std::string(argument) +
                  "': expected a decimal number in 1048576..1073741824");
   }
   candidate.max_frame_bytes = static_cast<std::uint32_t>(parsed);
   return Status::Continue;
}

Status Configuration::applyLogLevel(std::string_view argument,
                                    std::string_view value,
                                    SDL3MCP_Configuration& candidate) {
   struct LevelName {
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

   for (const LevelName& entry : levels) {
      if (equalsIgnoringCase(value, entry.name)) {
         candidate.log_level = entry.level;
         return Status::Continue;
      }
   }
   return fail("invalid value '" + std::string(value) + "' for '" +
               std::string(argument) +
               "': expected off, error, warn, info or debug");
}

}  // namespace sdl3mcp
