#pragma once

/*
  The closed-schema property readers the sdl_input parsers share: a required
  string or number, a finite number, and the check that rejects every member an
  action's schema does not declare. The entry-point parser and the per-action
  parsers read the same JSON vocabulary from one place, so a typo cannot slip
  through one unit while another rejects it.
*/

#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>

#include "Result.h"

namespace sdl3mcp {

/*
  Rejects the first member the closed schema for this action type does not
  declare. Iteration order is the object's key order, so the reported name is
  stable for a given request.
*/
Result<void, ProtocolError> rejectUnknownProperties(
    const nlohmann::json& value, std::span<const std::string_view> allowed);

Result<std::string, ProtocolError> readRequiredString(
    const nlohmann::json& value, const char* name);

/* A finite number as JSON writes it; the cast to float happens at the call site. */
Result<double, ProtocolError> readNumber(const nlohmann::json& value,
                                         const char* name);

Result<double, ProtocolError> readRequiredNumber(const nlohmann::json& value,
                                                 const char* name);

}  // namespace sdl3mcp
