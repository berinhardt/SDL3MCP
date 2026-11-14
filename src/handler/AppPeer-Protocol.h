#pragma once

/*
  The constants both AppPeer units need: the protocol revision the Handler
  announces and the name it announces itself under. start() sends them
  (AppPeer.cpp) and the answers it reads are validated against the same session
  (AppPeer-Protocol.cpp), so they live in one place rather than two.
*/

#include <string_view>

namespace sdl3mcp {

constexpr std::string_view kProtocolVersion = "2025-11-25";
constexpr std::string_view kServerName = "sdl3mcp";

}  // namespace sdl3mcp
