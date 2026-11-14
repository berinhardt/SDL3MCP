#pragma once

/*
  The sdl_input argument adapter: one trusted JSON document in, one InputRequest
  out, or one line of text.

  This is the only place the wire shape of an input request is read. It knows no
  SDL and no KeyMap on purpose: key names, chords, and layout resolution stay on
  the app thread, so this header and its implementation can be exercised without
  a window or a thread. It lives beside Input.h rather than inside it so the
  input model itself stays free of the JSON dependency.

  The defaults mirror the published tool schema, and every property the schema
  does not name is rejected, so a client typo cannot silently fall back to a
  default. Validation splits in two: the parser rejects what the schema itself
  types (missing fields, wrong JSON types, closed objects, coordinate pairing),
  and validateStructure remains the final gate for model-level rules (action
  counts, byte limits, modifier placement, clicks range, finite numbers).

  The observation half (region) is parsed by the helper sdl_capture uses, so
  both tools accept exactly the same region. There is no timeout_ms in 1.0.

  On failure the error is one line. Per-action failures carry the 0-based array
  index ("actions[2]: ..."), which is what a client needs to fix its batch;
  validateStructure's message is passed through unchanged.
*/

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "Frame.h"
#include "Input.h"

namespace sdl3mcp {

/*
  One parsed sdl_input call: the batch to apply and the optional frame
  observation that follows it. The library has no timeout, so the request has
  none; a client that needs one imposes it itself.
*/
struct InputRequest {
   InputBatch batch;
   std::optional<CaptureRegion> region;
};

/*
  Parses the whole sdl_input `arguments` object: `actions` is required and
  `region` is the only optional property. `timeout_ms` is not part of the 1.0
  schema and is rejected like any other unknown property. Structural problems
  are reported before anything is queued, with the 0-based action index; key
  names and chords still resolve at apply time. Pure: no SDL call and no KeyMap.
*/
std::optional<InputRequest> parseInputArguments(const nlohmann::json& arguments,
                                                std::string& error);

}  // namespace sdl3mcp
