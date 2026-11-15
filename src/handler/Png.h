/*
  The Handler's PNG and base64 half of presentation. stb_image_write is compiled
  in StbImageWrite.cpp, in this directory, because only the Handler turns pixels
  into an MCP image block; the library never references stb and its archive must
  contain no stb symbol.

  A frame is always tightly packed RGBA8888 with width * height * 4 bytes and a
  top-left origin. These helpers still check the invariant, because a malformed
  frame must not be read out of bounds.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sdl3mcp {

/*
  Encodes tightly packed RGBA8888 pixels (stride width * 4, top-left origin) as
  a PNG written into `out`. Returns false and leaves `out` empty on invalid
  input or encoder failure.
*/
bool encodePngToVector(const std::uint8_t* pixels,
                       int width,
                       int height,
                       std::vector<std::uint8_t>& out);

/* Standard base64 with '+', '/', and '=' padding, no line breaks. */
std::string base64Encode(const std::uint8_t* data, std::size_t size);
std::string base64Encode(const std::vector<std::uint8_t>& data);

}  // namespace sdl3mcp
