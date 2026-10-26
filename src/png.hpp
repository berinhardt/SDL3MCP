/*
  The only interface to stb_image_write. stb_image_write_impl.cpp compiles the
  single stb implementation with STB_IMAGE_WRITE_STATIC, so no stb symbol ever
  leaves that translation unit; this wrapper is the one external function the
  rest of the library calls.
*/

#ifndef SDL3MCP_PNG_HPP
#define SDL3MCP_PNG_HPP

#include <cstdint>
#include <vector>

namespace sdl3mcp
{

/*
  Encodes tightly packed RGBA8888 pixels (stride width * 4, top-left origin) as
  a PNG written into `out`. Returns false and leaves `out` empty on invalid
  input or encoder failure.
*/
bool encodePngToVector(const uint8_t *pixels,
                       int width,
                       int height,
                       std::vector<uint8_t> &out);

} // namespace sdl3mcp

#endif /* SDL3MCP_PNG_HPP */
