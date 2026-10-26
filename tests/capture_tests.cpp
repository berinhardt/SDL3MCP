/*
  CaptureState is the request state between the server and the app thread, so
  every outcome is exercised with a real waiter: a frame arrives, the clock runs
  out, a drop is retried, and shutdown cancels. Nothing here touches SDL, a
  window, or a renderer.
*/

#include "capture.hpp"

#include "test_util.hpp"

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

void testSubmitDeliversFrame()
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

void testTimeoutClearsRequestAndIgnoresLateFrame()
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

void testFailureIsRetriedAndCountedOnce()
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

void testCancelWakesWaiter()
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

void testStatsUpdateOnlyOnSuccess()
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

void testPendingTransitions()
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

} // namespace

int main()
{
    testSubmitDeliversFrame();
    testTimeoutClearsRequestAndIgnoresLateFrame();
    testFailureIsRetriedAndCountedOnce();
    testCancelWakesWaiter();
    testStatsUpdateOnlyOnSuccess();
    testPendingTransitions();

    std::fprintf(stderr, "capture tests passed\n");
    return 0;
}
