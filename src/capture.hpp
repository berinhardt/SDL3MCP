/*
  The request half of capture: one pending frame request shared by the server
  thread (which waits) and the app thread (which answers).

  The protocol is deliberately small. The server calls begin() before it waits
  and submit() is the app's only way to satisfy it. Everything a client might do
  while waiting has a distinct outcome:

  - submit() delivers a frame and clears pending;
  - wait() times out, clears pending, and hands back the first drop reason;
  - cancel() wakes the waiter during shutdown;
  - a submit() that arrives after any of those is ignored, so a late frame can
    never satisfy the next request.

  recordFailure() is for rejected SDL3MCP_WriteFrame data: it keeps the first
  reason, counts one drop, and leaves the request pending so the app may retry
  on the next frame. recordError() only publishes a reason for sdl_info, which is
  what crop and timeout failures need because they are not drops.

  One mutex and one condition variable guard the state. Nothing here calls SDL,
  so every member is legal on either thread.
*/

#ifndef SDL3MCP_CAPTURE_HPP
#define SDL3MCP_CAPTURE_HPP

#include "frame.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace sdl3mcp
{

struct CaptureStats
{
    bool pending = false;
    uint64_t count = 0;
    uint64_t dropped = 0;
    /* The frame the library returned: after any region crop. */
    int lastWidth = 0;
    int lastHeight = 0;
    /* Raw RGBA payload size, width * height * 4; the library never encodes an image. */
    uint64_t lastBytes = 0;
    /* Wait for the frame plus the region crop. */
    double lastMs = 0;
    std::string lastError;
};

class CaptureState
{
public:
    /*
      Starts one request. Fails with a one-line message when another request is
      already pending. A successful begin forgets any earlier frame, drop reason,
      and cancellation.
    */
    bool begin(std::string &error);

    /*
      Blocks until submit() delivers a frame (true) or the timeout/cancel clears
      the request (false). On timeout the request is cleared and `failure` is the
      first recorded drop reason, or empty if WriteFrame was never called.
    */
    bool wait(std::chrono::milliseconds timeout, Frame &frame, std::string &failure);

    /* True while begin() has a request that has not been answered or cleared. */
    bool pending() const;

    /* Delivers the frame and wakes the waiter; ignored when nothing is pending. */
    void submit(Frame frame);

    /* Records a rejected WriteFrame: first reason kept, one drop counted. */
    void recordFailure(std::string reason);

    /* Publishes a failure reason without counting a drop (crop, encode, timeout). */
    void recordError(std::string reason);

    /* Clears the request and wakes a waiter with false. */
    void cancel();

    /* A consistent copy for sdl_info. */
    CaptureStats stats() const;

    /* Publishes a completed capture; this is the only success counter. */
    void updateSuccess(int width, int height, uint64_t bytes, double ms);

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;

    std::optional<Frame> frame_;
    std::string failure_;
    CaptureStats stats_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_CAPTURE_HPP */
