#pragma once

/*
  The action dispatch and the actions that need no parser of their own: it reads
  `type`, rejects a member the action's closed schema does not declare, and
  hands the object to the one parser. The keyboard and pointer parsers live in
  their own units with their own headers.
*/

#include <nlohmann/json.hpp>

#include "Input.h"
#include "Result.h"

namespace sdl3mcp {

Result<InputAction, ProtocolError> parseText(const nlohmann::json& value);
Result<InputAction, ProtocolError> parseReleaseAll(const nlohmann::json& value);

Result<InputAction, ProtocolError> parseAction(const nlohmann::json& value);

}  // namespace sdl3mcp
