/*
  CaptureState is the request state between the server and the app thread, so
  every outcome is exercised with a real waiter: a frame arrives, the clock runs
  out, a drop is retried, and shutdown cancels. Nothing here touches SDL, a
  window, or a renderer.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "capture.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

namespace
{

using namespace std::chrono_literals;

using sdl3mcp::CaptureState;
using sdl3mcp::CaptureStats;
using sdl3mcp::Frame;

Frame solidFrame(int width, int height, uint8_t red)
{
    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t offset = 0; offset < frame.pixels.size(); offset += 4) {
        frame.pixels[offset] = red;
        frame.pixels[offset + 1] = 0;
        frame.pixels[offset + 2] = 0;
        frame.pixels[offset + 3] = 255;
    }
    return frame;
}

TEST_CASE("submit delivers frame")
{
    CaptureState capture;
    std::string error = "stale";

    CHECK(!capture.pending());
    CHECK(capture.begin(error));
    CHECK(error.empty());
    CHECK(capture.pending());

    capture.submit(solidFrame(4, 3, 7));
    CHECK(!capture.pending());

    Frame received;
    std::string failure = "stale";
    CHECK(capture.wait(50ms, received, failure));
    CHECK(received.width == 4);
    CHECK(received.height == 3);
    CHECK(received.pixels.size() == 4u * 3u * 4u);
    CHECK(received.pixels[0] == 7);
    CHECK(received.pixels[3] == 255);
    CHECK(failure.empty());
    CHECK(!capture.pending());
}

TEST_CASE("timeout clears request and ignores late frame")
{
    CaptureState capture;
    std::string error;
    CHECK(capture.begin(error));

    Frame received;
    std::string failure = "stale";
    CHECK(!capture.wait(20ms, received, failure));
    CHECK(failure.empty());
    CHECK(!capture.pending());

    /* The app answered too late: that frame must not satisfy a later request. */
    capture.submit(solidFrame(2, 2, 9));
    CHECK(!capture.pending());

    CHECK(capture.begin(error));
    CHECK(!capture.wait(20ms, received, failure));
    CHECK(!capture.pending());
}

TEST_CASE("failure is retried and counted once")
{
    CaptureState capture;
    std::string error;
    CHECK(capture.begin(error));

    capture.recordFailure("size 4 != 8");
    CHECK(capture.pending());
    capture.recordFailure("size 12 != 8");

    CaptureStats stats = capture.stats();
    CHECK(stats.pending);
    CHECK(stats.dropped == 2);
    CHECK(stats.lastError == "size 4 != 8");
    CHECK(stats.count == 0);

    capture.submit(solidFrame(2, 1, 1));

    Frame received;
    std::string failure;
    CHECK(capture.wait(50ms, received, failure));
    CHECK(received.width == 2);
    CHECK(failure.empty());

    stats = capture.stats();
    CHECK(!stats.pending);
    CHECK(stats.dropped == 2);
    CHECK(stats.lastError == "size 4 != 8");
    CHECK(stats.count == 0);
}

TEST_CASE("cancel wakes waiter")
{
    CaptureState capture;
    std::string error;
    CHECK(capture.begin(error));

    std::string secondError;
    CHECK(!capture.begin(secondError));
    CHECK(!secondError.empty());
    CHECK(capture.pending());

    std::atomic<bool> done{false};
    bool waitResult = true;
    Frame received;
    std::string failure = "stale";
    std::thread waiter([&capture, &done, &waitResult, &received, &failure] {
        waitResult = capture.wait(5s, received, failure);
        done.store(true);
    });

    std::this_thread::sleep_for(20ms);
    CHECK(!done.load());

    capture.cancel();
    waiter.join();

    CHECK(done.load());
    CHECK(!waitResult);
    CHECK(!capture.pending());
}

TEST_CASE("stats update only on success")
{
    CaptureState capture;
    std::string error;
    CHECK(capture.begin(error));
    capture.submit(solidFrame(8, 4, 2));

    Frame received;
    std::string failure;
    CHECK(capture.wait(50ms, received, failure));

    CaptureStats stats = capture.stats();
    CHECK(stats.count == 0);
    CHECK(stats.lastWidth == 0);
    CHECK(stats.lastHeight == 0);
    CHECK(stats.lastBytes == 0);
    CHECK(stats.lastMs == 0.0);

    capture.updateSuccess(64, 32, 1234, 2.5);

    stats = capture.stats();
    CHECK(stats.count == 1);
    CHECK(stats.lastWidth == 64);
    CHECK(stats.lastHeight == 32);
    CHECK(stats.lastBytes == 1234);
    CHECK(stats.lastMs == 2.5);
}

TEST_CASE("pending transitions")
{
    CaptureState capture;
    std::string error;

    CHECK(!capture.pending());
    CHECK(capture.begin(error));
    CHECK(capture.pending());

    std::string secondError;
    CHECK(!capture.begin(secondError));
    CHECK(capture.pending());

    capture.submit(solidFrame(1, 1, 0));
    CHECK(!capture.pending());

    CHECK(capture.begin(error));
    CHECK(capture.pending());
    capture.cancel();
    CHECK(!capture.pending());

    CHECK(capture.begin(error));
    CHECK(capture.pending());
    Frame received;
    std::string failure;
    CHECK(!capture.wait(10ms, received, failure));
    CHECK(!capture.pending());
}

/*
  A capture requested before the app has completed its first frame must wait:
  the first rendered frame carries no ImGui draw data, so serving it would hand
  back an untouched scene. The boundary is pinned here because the app-facing
  gate (SDL3MCP_FrameRequested) needs a window and a server to observe.
*/
TEST_CASE("servable frame boundary")
{
    CHECK(sdl3mcp::kFirstServableFrame == 1);
    CHECK(!sdl3mcp::servableFrame(0));
    CHECK(sdl3mcp::servableFrame(1));
    CHECK(sdl3mcp::servableFrame(2));
}

/*
  A declined poll is not a dropped request: CaptureState knows nothing of the
  gate, so the request has to survive it and be answered by the first frame the
  predicate allows.
*/
TEST_CASE("declined poll keeps request pending")
{
    CaptureState capture;
    std::string error;
    CHECK(capture.begin(error));

    /* The app polls at frame 0: the gate declines, the request stays open. */
    CHECK(!sdl3mcp::servableFrame(0));
    CHECK(capture.pending());
    CHECK(capture.stats().dropped == 0);

    /* The next frame is servable, so the app answers the same request. */
    CHECK(sdl3mcp::servableFrame(1));
    CHECK(capture.pending());
    capture.submit(solidFrame(2, 2, 3));
    CHECK(!capture.pending());

    Frame received;
    std::string failure;
    CHECK(capture.wait(50ms, received, failure));
    CHECK(received.width == 2);
    CHECK(received.height == 2);
    CHECK(failure.empty());
}

/*
  The browser arm asks without waiting: a request the app has not answered yet
  yields false, and the frame that answers it is handed back exactly once.
*/
TEST_CASE("try take returns submitted frame once")
{
    CaptureState capture;
    std::string error;
    CHECK(capture.begin(error));

    Frame received;
    CHECK(!capture.tryTake(received));
    CHECK(capture.pending());
    CHECK(received.pixels.empty());

    capture.submit(solidFrame(3, 2, 11));
    CHECK(!capture.pending());

    CHECK(capture.tryTake(received));
    CHECK(received.width == 3);
    CHECK(received.height == 2);
    CHECK(received.pixels.size() == 3u * 2u * 4u);
    CHECK(received.pixels[0] == 11);
    CHECK(received.pixels[3] == 255);
    CHECK(!capture.pending());

    Frame again;
    CHECK(!capture.tryTake(again));
    CHECK(again.pixels.empty());
}

/*
  tryTake only reports a frame submit() delivered while a request was pending.
  An idle state, a cancelled request, and a frame that arrived after either all
  yield false and leave the state ready for the next request.
*/
TEST_CASE("try take ignores idle and cancelled state")
{
    CaptureState capture;
    Frame received;

    CHECK(!capture.tryTake(received));

    std::string error;
    CHECK(capture.begin(error));
    capture.cancel();
    CHECK(!capture.tryTake(received));

    /* A late frame cannot satisfy a cancelled request, so nothing is takeable. */
    capture.submit(solidFrame(2, 2, 4));
    CHECK(!capture.tryTake(received));

    CHECK(capture.begin(error));
    capture.submit(solidFrame(2, 1, 5));
    CHECK(capture.tryTake(received));
    CHECK(received.width == 2);
    CHECK(received.height == 1);
    CHECK(received.pixels[0] == 5);
}

/*
  The frame stamp is app state carried by value, so a crop must copy it. The
  library stamps at WriteFrame and crops on the server thread; losing the index
  there would make every cropped answer anonymous.
*/
TEST_CASE("crop preserves frame stamp")
{
    Frame frame = solidFrame(4, 3, 5);
    frame.frame = 9;

    std::string error;
    const auto cropped =
        sdl3mcp::cropFrame(frame, sdl3mcp::CaptureRegion{1, 1, 2, 2}, error);
    CHECK(cropped.has_value());
    CHECK(error.empty());
    CHECK(cropped->frame == 9);
}

/*
  The attached frame is always later than the applied one because the answer
  waits for the F+1 completion before requesting a capture. The helper is a
  direct assertion of that relation: it must reject equal and earlier indices
  even though the protocol never produces them.
*/
TEST_CASE("frame order helper")
{
    CHECK(!sdl3mcp::checkFrameOrder(3, 5).has_value());
    CHECK(!sdl3mcp::checkFrameOrder(0, 1).has_value());

    const auto same = sdl3mcp::checkFrameOrder(4, 4);
    CHECK(same.has_value());
    CHECK(same->find("frame 4") != std::string::npos);

    const auto earlier = sdl3mcp::checkFrameOrder(7, 6);
    CHECK(earlier.has_value());
    CHECK(earlier->find("frame 6") != std::string::npos);
    CHECK(earlier->find("frame 7") != std::string::npos);
}

} // namespace
