/*
  The sdl_input argument adapter: one trusted JSON document in, one InputBatch
  out, or one line of text.

  This is the only place the server thread looks at the wire shape of an input
  request. It knows no SDL and no KeyMap on purpose: key names, chords, and
  layout resolution stay on the app thread, so this file can be exercised
  without a window, a thread, or a blocking submit.

  The defaults mirror the published tool schema, and every property the schema
  does not name is rejected, so a client typo cannot silently fall back to a
  default. Validation splits in two: the parser rejects what the schema itself
  types (missing fields, wrong JSON types, closed objects, coordinate pairing),
  and validateStructure remains the final gate for model-level rules (action
  counts, byte limits, modifier placement, clicks range, finite numbers).

  On failure the error is one line. Per-action failures carry the 0-based array
  index ("actions[2]: ..."), which is what a client needs to fix its batch;
  validateStructure's message is passed through unchanged.
*/

#ifndef SDL3MCP_INPUT_JSON_HPP
#define SDL3MCP_INPUT_JSON_HPP

#include "input.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace sdl3mcp
{

/*
  Parses the `arguments` object of an sdl_input call. Returns the batch on
  success and clears `error`; on failure returns nullopt and writes one line to
  `error`. Pure: no SDL call, no KeyMap, no queue access.
*/
std::optional<InputBatch> parseInputArguments(const nlohmann::json &arguments,
                                              std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_INPUT_JSON_HPP */
