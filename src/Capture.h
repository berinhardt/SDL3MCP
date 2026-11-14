#pragma once

/*
  The request half of capture: the wire arguments and the statistics sdl_info
  reports. The app-thread bookkeeping lives in Pipeline; this file is pure so a
  malformed region is rejected before any frame is requested.

  The library has no frame or capture timeout in 1.0. `timeout_ms` is therefore
  not a property of either tool: a request carrying one is rejected like any
  other unknown property, and a direct client that needs a bound must impose it.

  `region` is in pixels of the captured frame, top-left origin, all four fields
  required, and must fit inside the frame. sdl_input takes the same object and
  parses it with the same helper, so both tools accept exactly the same region.
*/

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "Frame.h"
#include "Result.h"

namespace sdl3mcp {

/* What sdl_info reports under `capture`. */
struct CaptureInfo {
   bool pending = false;
   std::uint64_t count = 0;
   std::uint64_t dropped = 0;
   /* The last completed request's frame, after any region crop. */
   int lastWidth = 0;
   int lastHeight = 0;
   /* Raw RGBA payload size, width * height * 4. */
   std::uint64_t lastBytes = 0;
   /* Registration to completion: the wait for the frame plus the crop. */
   double lastMs = 0.0;
   std::string lastError;
};

/* The parsed sdl_capture arguments: only the optional crop region. */
struct CaptureArguments {
   std::optional<CaptureRegion> region;
};

/*
  Reads the optional `region` member of a tool-call arguments object. Success
  leaves `region` empty when the member is absent, or filled when it is present;
  a failure carries the one-line reason.
*/
Result<void, ProtocolError> parseRegionArgument(
    const nlohmann::json& arguments, std::optional<CaptureRegion>& region);

/*
  Parses the whole sdl_capture `arguments` object: `region` is the only accepted
  property.
*/
Result<CaptureArguments, ProtocolError> parseCaptureArguments(
    const nlohmann::json& arguments);

}  // namespace sdl3mcp
