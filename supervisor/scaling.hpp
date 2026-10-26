/*
  The supervisor's scaling half: box-filtering a raw frame down to a maximum
  width before PNG encoding. It is a value in, value out function over the
  library's Frame, so the pixel math is testable without a window or a process.

  Downscaling used to live in the library. It is presentation policy, and the
  library-to-supervisor link now carries raw pixels with an explicit size, so the
  supervisor owns it alongside PNG and base64.
*/

#ifndef SDL3MCP_SUPERVISOR_SCALING_HPP
#define SDL3MCP_SUPERVISOR_SCALING_HPP

#include "frame.hpp"

namespace sdl3mcp
{

/*
  Box-filters the frame down to at most `maxWidth` pixels wide, preserving the
  aspect ratio. A non-positive maxWidth or one at least the frame width returns
  an unchanged copy: this never upscales, and the result is always at least one
  pixel tall.
*/
Frame downscaleFrame(const Frame &frame, int maxWidth);

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_SCALING_HPP */
