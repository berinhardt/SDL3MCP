#pragma once

/*
  The `key` action parser, which reads `key`, `action` and `modifiers` into a
  KeyInput. Key names and chords are not resolved here: that happens on the app
  thread, so the parser stays free of SDL and the KeyMap.
*/

#include <nlohmann/json.hpp>

#include "Input.h"
#include "Result.h"

namespace sdl3mcp {

Result<InputAction, ProtocolError> parseKey(const nlohmann::json& value);

}  // namespace sdl3mcp
