/*
  The arguments sdl_capture and the observation half of sdl_input share: the
  optional crop region and the optional per-call timeout.

  Both tools grew the same two properties, so the parsing lives here once: the
  same client document is accepted or rejected identically, and a range or a
  property name cannot drift between the two schemas. sdl_input adds the action
  batch and sdl_capture the raw framing; neither restates these rules.

  Server-thread only, pure, and free of SDL: it validates wire shape and ranges
  and leaves the crop itself to cropFrame().
*/

#ifndef SDL3MCP_CAPTURE_ARGUMENTS_HPP
#define SDL3MCP_CAPTURE_ARGUMENTS_HPP

#include "frame.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace sdl3mcp
{

/* The published timeout range, shared by sdl_capture and sdl_input. */
constexpr int kCaptureMinTimeoutMs = 1;
constexpr int kCaptureMaxTimeoutMs = 60000;

/*
  Reads the optional `region` member of a tool-call arguments object. On success
  returns true and leaves `region` empty when the member is absent, or filled
  when it is present. On failure returns false and writes one line to `error`.
*/
bool parseRegionArgument(const nlohmann::json &arguments,
                         std::optional<CaptureRegion> &region,
                         std::string &error);

/*
  Reads the optional `timeout_ms` member, requiring an integer in
  1..kCaptureMaxTimeoutMs and falling back to `defaultTimeout` when it is
  absent. On failure returns false and writes one line to `error`.
*/
bool parseTimeoutArgument(const nlohmann::json &arguments,
                          std::chrono::milliseconds defaultTimeout,
                          std::chrono::milliseconds &timeout,
                          std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_CAPTURE_ARGUMENTS_HPP */
