/*
  The app-thread pipeline: one frame serves every request registered before it,
  a capture is a no-op input, and a dropped frame leaves the request pending.
  These cases drive the pipeline directly with a recording completion, so the
  bookkeeping is pinned without a socket or a session. SDL's dummy video driver
  supplies the window a bound input batch needs.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "Pipeline.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Frame.h"
#include "Info.h"
#include "Input.h"
#include "KeyMap.h"
#include "Mouse.h"
#include "Tool.h"
#include "ToolResult.h"

namespace {

class RecordingCompletion : public sdl3mcp::ToolCompletionDelegate {
  public:
   void complete(sdl3mcp::ToolResult result) override {
      results.push_back(std::move(result));
   }
   void cancelled() override { cancellations++; }

   std::vector<sdl3mcp::ToolResult> results;
   int cancellations = 0;
};

/* A 2x2 frame whose pixels encode their coordinates, so a crop is checkable. */
sdl3mcp::Frame patternFrame(std::uint64_t stamp) {
   sdl3mcp::Frame frame;
   frame.width = 2;
   frame.height = 2;
   frame.frame = stamp;
   frame.pixels = {
       1,
       2,
       3,
       4,  // (0,0)
       5,
       6,
       7,
       8,  // (1,0)
       9,
       10,
       11,
       12,  // (0,1)
       13,
       14,
       15,
       16,  // (1,1)
   };
   return frame;
}

sdl3mcp::InputBatch keyBatch(std::string key) {
   sdl3mcp::KeyInput input;
   input.key = std::move(key);
   input.action = sdl3mcp::KeyAction::Press;
   sdl3mcp::InputBatch batch;
   batch.actions.emplace_back(std::move(input));
   return batch;
}

class DummyVideo {
  public:
   DummyVideo() {
      (void)SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
      CHECK(SDL_Init(SDL_INIT_VIDEO));
      window = SDL_CreateWindow("SDL3MCP pipeline test", 16, 12, 0);
      CHECK(window != nullptr);
   }

   ~DummyVideo() {
      if (window != nullptr) {
         SDL_DestroyWindow(window);
      }
      SDL_Quit();
   }

   SDL_Window* window = nullptr;
};

sdl3mcp::MouseMetrics windowMetrics(SDL_Window* window) {
   int width = 0;
   int height = 0;
   SDL_GetWindowSize(window, &width, &height);
   sdl3mcp::MouseMetrics metrics;
   metrics.width = static_cast<float>(width);
   metrics.height = static_cast<float>(height);
   metrics.relativeMode = SDL_GetWindowRelativeMouseMode(window);
   metrics.warpCursor = true;
   return metrics;
}

}  // namespace

TEST_CASE_FIXTURE(DummyVideo, "a capture completes with the next frame's pixels") {
   sdl3mcp::KeyMap keyMap;
   sdl3mcp::Pipeline pipeline(keyMap, 1024);
   RecordingCompletion completion;

   pipeline.registerCapture(std::nullopt, completion);
   CHECK(pipeline.frameRequested());
   CHECK(pipeline.captureInfo().pending);

   const sdl3mcp::Frame frame = patternFrame(7);
   pipeline.writeFrame(frame.pixels.data(), frame.pixels.size(), frame.width,
                       frame.height, 7);
   CHECK(pipeline.frameRequested());
   pipeline.completeWrittenFrame();

   CHECK(!pipeline.frameRequested());
   REQUIRE(completion.results.size() == 1);
   CHECK(completion.results[0].isRawFrame());
   CHECK(completion.results[0].content["format"] == "rgba8888");
   CHECK(completion.results[0].content["width"] == 2);
   CHECK(completion.results[0].content["height"] == 2);
   CHECK(completion.results[0].content["byteLength"] == 16);
   CHECK(completion.results[0].content["frame"] == 7);
   CHECK(!completion.results[0].content.contains("text"));
   CHECK(completion.results[0].payload == frame.pixels);

   const sdl3mcp::CaptureInfo info = pipeline.captureInfo();
   CHECK(!info.pending);
   CHECK(info.count == 1);
   CHECK(info.dropped == 0);
   CHECK(info.lastWidth == 2);
   CHECK(info.lastHeight == 2);
   CHECK(info.lastBytes == 16);
   CHECK(info.lastError.empty());
   CHECK(info.lastMs >= 0.0);
}

TEST_CASE_FIXTURE(DummyVideo, "multiple requests share one frame and crop per request") {
   sdl3mcp::KeyMap keyMap;
   sdl3mcp::Pipeline pipeline(keyMap, 1024);
   RecordingCompletion whole;
   RecordingCompletion rightColumn;
   RecordingCompletion bottomRow;

   pipeline.registerCapture(std::nullopt, whole);
   pipeline.registerCapture(sdl3mcp::CaptureRegion{1, 0, 1, 2}, rightColumn);
   pipeline.registerCapture(sdl3mcp::CaptureRegion{0, 1, 2, 1}, bottomRow);

   const sdl3mcp::Frame frame = patternFrame(9);
   pipeline.writeFrame(frame.pixels.data(), frame.pixels.size(), frame.width,
                       frame.height, 9);
   pipeline.completeWrittenFrame();

   REQUIRE(whole.results.size() == 1);
   REQUIRE(rightColumn.results.size() == 1);
   REQUIRE(bottomRow.results.size() == 1);

   /* Every request carries the one frame that was written. */
   CHECK(whole.results[0].content["frame"] == 9);
   CHECK(rightColumn.results[0].content["frame"] == 9);
   CHECK(bottomRow.results[0].content["frame"] == 9);

   CHECK(rightColumn.results[0].content["width"] == 1);
   CHECK(rightColumn.results[0].content["height"] == 2);
   CHECK(rightColumn.results[0].payload ==
         std::vector<std::uint8_t>{5, 6, 7, 8, 13, 14, 15, 16});

   CHECK(bottomRow.results[0].content["width"] == 2);
   CHECK(bottomRow.results[0].content["height"] == 1);
   CHECK(bottomRow.results[0].payload ==
         std::vector<std::uint8_t>{9, 10, 11, 12, 13, 14, 15, 16});

   const sdl3mcp::CaptureInfo info = pipeline.captureInfo();
   CHECK(info.count == 3);
   /* last* describes the last completed request, in registration order. */
   CHECK(info.lastWidth == 2);
   CHECK(info.lastHeight == 1);
   CHECK(info.lastBytes == 8);
}

TEST_CASE_FIXTURE(DummyVideo, "a dropped frame leaves the request pending") {
   sdl3mcp::KeyMap keyMap;
   sdl3mcp::Pipeline pipeline(keyMap, 1024);
   RecordingCompletion completion;

   pipeline.registerCapture(std::nullopt, completion);

   const sdl3mcp::Frame frame = patternFrame(3);
   /* One byte short of width * height * 4. */
   pipeline.writeFrame(frame.pixels.data(), frame.pixels.size() - 1, frame.width,
                       frame.height, 3);
   CHECK(completion.results.empty());
   CHECK(pipeline.frameRequested());
   CHECK(pipeline.captureInfo().pending);

   sdl3mcp::CaptureInfo info = pipeline.captureInfo();
   CHECK(info.dropped == 1);
   CHECK(info.count == 0);
   CHECK(!info.lastError.empty());

   pipeline.writeFrame(frame.pixels.data(), frame.pixels.size(), frame.width,
                       frame.height, 3);
   pipeline.completeWrittenFrame();

   REQUIRE(completion.results.size() == 1);
   info = pipeline.captureInfo();
   CHECK(info.dropped == 1);
   CHECK(info.count == 1);
   CHECK(info.lastError.empty());
}

TEST_CASE_FIXTURE(DummyVideo, "a capture and an input registered together share the frame") {
   sdl3mcp::KeyMap keyMap;
   sdl3mcp::Pipeline pipeline(keyMap, 1024);
   pipeline.setWindow(window, SDL_GetWindowID(window), windowMetrics(window));

   RecordingCompletion capture;
   RecordingCompletion input;
   pipeline.registerCapture(std::nullopt, capture);
   pipeline.registerInput(keyBatch("a"), std::nullopt, input);

   pipeline.applyRegisteredInputs(12);
   CHECK(pipeline.appliedFrame() == 12);
   CHECK(pipeline.frameRequested());

   const sdl3mcp::Frame frame = patternFrame(13);
   pipeline.writeFrame(frame.pixels.data(), frame.pixels.size(), frame.width,
                       frame.height, 13);
   pipeline.completeWrittenFrame();

   REQUIRE(capture.results.size() == 1);
   REQUIRE(input.results.size() == 1);
   CHECK(capture.results[0].content["frame"] == 13);
   CHECK(input.results[0].content["frame"] == 13);
   CHECK(input.results[0].content["text"] ==
         "1 actions applied at frame 12; frame 13 attached, rendered after the "
         "actions were delivered");
   CHECK(!capture.results[0].content.contains("text"));
   CHECK(pipeline.captureInfo().count == 2);
}

TEST_CASE_FIXTURE(DummyVideo, "a failed batch reports an error without dropping the capture") {
   sdl3mcp::KeyMap keyMap;
   sdl3mcp::Pipeline pipeline(keyMap, 1024);
   pipeline.setWindow(window, SDL_GetWindowID(window), windowMetrics(window));

   RecordingCompletion capture;
   RecordingCompletion input;
   pipeline.registerCapture(std::nullopt, capture);
   pipeline.registerInput(keyBatch("not-a-real-key"), std::nullopt, input);

   pipeline.applyRegisteredInputs(4);
   /* The failure answers one frame later, like a successful observation. */
   CHECK(input.results.empty());
   CHECK(pipeline.appliedFrame() == 0);
   CHECK(capture.results.empty());
   CHECK(pipeline.frameRequested());

   pipeline.completeFailures();
   REQUIRE(input.results.size() == 1);
   CHECK(input.results[0].isError);
   CHECK(!input.results[0].isRawFrame());
   CHECK(input.results[0].content[0]["text"].get<std::string>().find(
             "unknown key") != std::string::npos);
   CHECK(capture.results.empty());
   CHECK(pipeline.frameRequested());

   const sdl3mcp::Frame frame = patternFrame(5);
   pipeline.writeFrame(frame.pixels.data(), frame.pixels.size(), frame.width,
                       frame.height, 5);
   pipeline.completeWrittenFrame();
   REQUIRE(capture.results.size() == 1);
   CHECK(capture.results[0].content["frame"] == 5);
}

TEST_CASE_FIXTURE(DummyVideo, "a held key is reported until it is released") {
   sdl3mcp::KeyMap keyMap;
   sdl3mcp::Pipeline pipeline(keyMap, 1024);
   pipeline.setWindow(window, SDL_GetWindowID(window), windowMetrics(window));

   sdl3mcp::KeyInput input;
   input.key = "a";
   input.action = sdl3mcp::KeyAction::Down;
   sdl3mcp::InputBatch batch;
   batch.actions.emplace_back(input);

   RecordingCompletion completion;
   pipeline.registerInput(std::move(batch), std::nullopt, completion);
   pipeline.applyRegisteredInputs(3);

   REQUIRE(pipeline.heldKeys().size() == 1);
   CHECK(keyMap.canonicalName(pipeline.heldKeys().front()) == "a");

   pipeline.releaseHeldInput();
   CHECK(pipeline.heldKeys().empty());
}

TEST_CASE_FIXTURE(DummyVideo, "an unbound pipeline ignores a frame and reports no request") {
   sdl3mcp::KeyMap keyMap;
   sdl3mcp::Pipeline pipeline(keyMap, 4);

   CHECK(!pipeline.frameRequested());
   const sdl3mcp::Frame frame = patternFrame(1);
   pipeline.writeFrame(frame.pixels.data(), frame.pixels.size(), frame.width,
                       frame.height, 1);
   pipeline.completeWrittenFrame();
   CHECK(pipeline.captureInfo().count == 0);
   CHECK(pipeline.captureInfo().dropped == 0);
}

TEST_CASE_FIXTURE(DummyVideo, "discard drops pending requests without answering") {
   sdl3mcp::KeyMap keyMap;
   sdl3mcp::Pipeline pipeline(keyMap, 1024);
   RecordingCompletion completion;

   pipeline.registerCapture(std::nullopt, completion);
   pipeline.discardRequests();
   CHECK(!pipeline.frameRequested());

   const sdl3mcp::Frame frame = patternFrame(1);
   pipeline.writeFrame(frame.pixels.data(), frame.pixels.size(), frame.width,
                       frame.height, 1);
   pipeline.completeWrittenFrame();
   CHECK(completion.results.empty());
}
