/*
  The pure half of capture: validation and crop. Every function here is a value
  in, value out with no queues, threads, or SDL calls, so the pixel math is
  testable without a window or a server.

  A Frame is always tightly packed RGBA8888 with `width * height * 4` bytes and
  a top-left origin. validateFrame() is the gate that establishes the invariant
  from SDL3MCP_WriteFrame's raw pointer and the bound window size; cropFrame()
  trusts it and only guards its own arguments.

  Presentation (downscaling, PNG, base64) is the supervisor's policy and lives in
  supervisor/, so the library never references stb.

  The region schema in the tool catalog names (x, y, width, height), so
  cropFrame() rejects anything that would read outside the frame instead of
  silently clamping: an agent that asks for a region it was not promised gets an
  error it can correct.
*/

#ifndef SDL3MCP_FRAME_HPP
#define SDL3MCP_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sdl3mcp
{

/* Tightly packed RGBA8888, width * height * 4 bytes, top-left origin. */
struct Frame
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

/* A crop rectangle in frame pixels; only non-empty, in-bounds regions are valid. */
struct CaptureRegion
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/*
  Checks that raw frame data can become a Frame. Returns nullopt when it can, or
  the reason it cannot, checking in order: non-null data, positive dimensions,
  an exact uint64_t(width) * height * 4 byte count, and the max_frame_bytes
  ceiling.
*/
std::optional<std::string> validateFrame(const uint8_t *data,
                                         size_t size,
                                         int width,
                                         int height,
                                         uint32_t maxFrameBytes);

/*
  Copies an in-bounds region out of the frame, one source row at a time. On
  failure returns nullopt and sets `error` to a message naming both the region
  and the frame size.
*/
std::optional<Frame> cropFrame(const Frame &frame,
                               const CaptureRegion &region,
                               std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_FRAME_HPP */
