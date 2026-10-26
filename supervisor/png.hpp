/*
  The supervisor's PNG and base64 half of presentation. stb_image_write is
  compiled in stb_image_write_impl.cpp, in this directory, because only the
  supervisor turns pixels into an MCP image block; the library never references
  stb.

  A Frame is always tightly packed RGBA8888 with `width * height * 4` bytes and a
  top-left origin, as established by the library's validateFrame(). These helpers
  still check the frame's own invariant, because a malformed frame must not be
  read out of bounds.
*/

#ifndef SDL3MCP_SUPERVISOR_PNG_HPP
#define SDL3MCP_SUPERVISOR_PNG_HPP

#include "frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sdl3mcp
{

/*
  The only interface to stb_image_write. stb_image_write_impl.cpp compiles the
  single stb implementation with STB_IMAGE_WRITE_STATIC, so no stb symbol ever
  leaves that translation unit; this wrapper is the one external function the
  rest of the supervisor calls.

  Encodes tightly packed RGBA8888 pixels (stride width * 4, top-left origin) as
  a PNG written into `out`. Returns false and leaves `out` empty on invalid
  input or encoder failure.
*/
bool encodePngToVector(const uint8_t *pixels,
                       int width,
                       int height,
                       std::vector<uint8_t> &out);

/* Encodes the frame as PNG bytes; empty when the frame is malformed or encoding fails. */
std::vector<uint8_t> encodePng(const Frame &frame);

/* Standard base64 with '+', '/', and '=' padding, no line breaks. */
std::string base64Encode(const uint8_t *data, size_t size);
std::string base64Encode(const std::vector<uint8_t> &data);

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_PNG_HPP */
