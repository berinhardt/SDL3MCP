#pragma once

/*
  The pure half of capture: validation and crop. Every function here is a value
  in, value out with no queues, threads, or SDL calls, so the pixel math is
  testable without a window or a server.

  A Frame is always tightly packed RGBA8888 with `width * height * 4` bytes and
  a top-left origin. validateFrame() is the gate that establishes the invariant
  from SDL3MCP_WriteFrame's raw pointer and the app-supplied dimensions;
  cropFrame() trusts it and only guards its own arguments.

  Presentation (PNG and base64) is the Handler's policy and lives in
  src/handler/Png.cpp, so the library never references stb.

  The region schema in the tool catalog names (x, y, width, height), so
  cropFrame() rejects anything that would read outside the frame instead of
  silently clamping: an agent that asks for a region it was not promised gets an
  error it can correct.
*/

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Result.h"

namespace sdl3mcp {

/* Tightly packed RGBA8888, width * height * 4 bytes, top-left origin. */
struct Frame {
   int width = 0;
   int height = 0;
   std::vector<uint8_t> pixels;
   /*
     The app frame whose rendered pixels these are. SDL3MCP_WriteFrame stamps
     it, and a crop carries it over, so an answer can name the frame it shows
     instead of leaving the caller to guess whether it predates its actions.
   */
   uint64_t frame = 0;
};

/* A crop rectangle in frame pixels; only non-empty, in-bounds regions are valid. */
struct CaptureRegion {
   int x = 0;
   int y = 0;
   int width = 0;
   int height = 0;
};

/*
  Checks that raw frame data can become a Frame. Returns success when it can,
  or the reason it cannot, checking in order: non-null data, positive
  dimensions, an exact uint64_t(width) * height * 4 byte count, and the
  max_frame_bytes ceiling.
*/
Result<void, ProtocolError> validateFrame(const uint8_t* data,
                                          size_t size,
                                          int width,
                                          int height,
                                          uint32_t maxFrameBytes);

/*
  Copies an in-bounds region out of the frame, one source row at a time. On
  failure the error names both the region and the frame size.
*/
Result<Frame, ProtocolError> cropFrame(const Frame& frame,
                                       const CaptureRegion& region);

}  // namespace sdl3mcp
