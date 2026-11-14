/*
  The Handler's tool-argument helpers: the timeout_ms budget every call
  carries, read here so the wait, forward, spawn and web units name one
  limit, and the integer reader the frame summary shares.
*/

#pragma once

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "Result.h"

namespace sdl3mcp {

constexpr std::int64_t kMaxTimeoutMs = 60000;

/* Reads a JSON integer, including unsigned values that still fit in int64_t. */
inline bool readInteger(const nlohmann::json& object,
                        const char* name,
                        std::int64_t& value) {
   const auto entry = object.find(name);
   if (entry == object.end() || !entry->is_number_integer()) {
      return false;
   }
   if (entry->is_number_unsigned()) {
      const std::uint64_t raw = entry->get<std::uint64_t>();
      if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
         return false;
      }
      value = static_cast<std::int64_t>(raw);
      return true;
   }
   value = entry->get<std::int64_t>();
   return true;
}

/*
  Reads the optional timeout_ms of one tool call. Absent means the Handler's
  budget; present must be an integer in 1..60000. The error names the tool.
*/
inline Result<std::int64_t, ProtocolError> readTimeoutMs(
    const nlohmann::json& arguments, std::int64_t fallback,
    std::string_view tool) {
   if (!arguments.contains("timeout_ms")) {
      return fallback;
   }
   std::int64_t requested = 0;
   if (!readInteger(arguments, "timeout_ms", requested) || requested < 1 ||
       requested > kMaxTimeoutMs) {
      return ProtocolError{std::string(tool) +
                           " timeout_ms must be in 1..60000"};
   }
   return requested;
}

}  // namespace sdl3mcp
