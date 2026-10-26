/*
  The pure half of capture: validation, crop, downscale, PNG, and base64. Every
  function here is a value in, value out with no queues, threads, or SDL calls,
  so the pixel math is testable without a window or a server.

  A Frame is always tightly packed RGBA8888 with `width * height * 4` bytes and
  a top-left origin. validateFrame() is the gate that establishes the invariant
  from SDL3MCP_WriteFrame's raw pointer and the bound window size; the later
  stages trust it and only guard their own arguments.

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

/*
  Box-filters the frame down to at most `maxWidth` pixels wide, preserving the
  aspect ratio. A non-positive maxWidth or one at least the frame width returns
  an unchanged copy: this never upscales, and the result is always at least one
  pixel tall.
*/
Frame downscaleFrame(const Frame &frame, int maxWidth);

/* Encodes the frame as PNG bytes; empty when the frame is malformed or encoding fails. */
std::vector<uint8_t> encodePng(const Frame &frame);

/* Standard base64 with '+', '/', and '=' padding, no line breaks. */
std::string base64Encode(const uint8_t *data, size_t size);
std::string base64Encode(const std::vector<uint8_t> &data);

} // namespace sdl3mcp

#endif /* SDL3MCP_FRAME_HPP */
