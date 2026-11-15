#pragma once

/*
  The numeric vocabulary the configuration shares: how a decimal value is read
  and the bounds every field must satisfy. The value validator and the argument
  appliers read the same numbers from one place, so a bound cannot drift between
  the two.
*/

#include <cstdint>
#include <string_view>

namespace sdl3mcp {

/* Strict decimal digits only; false on empty input or overflow. */
bool parseDecimal(std::string_view text, std::uint64_t& out);

constexpr std::uint32_t kMinConnectTimeoutMs = 1u;
constexpr std::uint32_t kMaxConnectTimeoutMs = 60000u;
constexpr std::uint32_t kMinMaxFrameBytes = 1024u * 1024u;
constexpr std::uint32_t kMaxMaxFrameBytes = 1024u * 1024u * 1024u;
constexpr int kMinLogLevel = -1;
constexpr int kMaxLogLevel = 4;

}  // namespace sdl3mcp
