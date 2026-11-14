#pragma once

/*
  The pointer action parsers: `mouse_move` takes x/y or dx/dy, `mouse_button`
  takes a button, an optional action, an optional coordinate pair and an
  optional click count, and `scroll` takes y plus an optional x. Range rules
  stay in validateStructure, so the parsers only enforce the JSON shape.
*/

#include <nlohmann/json.hpp>

#include "Input.h"
#include "Result.h"

namespace sdl3mcp {

Result<InputAction, ProtocolError> parseMouseMove(const nlohmann::json& value);
Result<InputAction, ProtocolError> parseMouseButton(const nlohmann::json& value);
Result<InputAction, ProtocolError> parseScroll(const nlohmann::json& value);

}  // namespace sdl3mcp
