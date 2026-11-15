/*
  The pipeline's state and registration: the window binding, the request
  queues, the frame the app writes, and the input it releases. Applying a batch
  is in Pipeline-Input.cpp; answering a written frame is in Pipeline-Frames.cpp.
*/

#include "Pipeline.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "Frame.h"

namespace sdl3mcp {

Pipeline::Pipeline(const KeyMap& keyMap, std::uint32_t maxFrameBytes)
    : maxFrameBytes_(maxFrameBytes),
      keyReplayer_(keyMap, 0),
      mouseReplayer_(0, MouseMetrics{}) {
}

Pipeline::~Pipeline() = default;

void Pipeline::setWindow(SDL_Window* window,
                         SDL_WindowID windowID,
                         MouseMetrics metrics) {
   window_ = window;
   windowID_ = windowID;

   keyReplayer_.setWindowID(windowID);
   keyReplayer_.reset();

   mouseReplayer_.setWindowID(windowID);
   mouseReplayer_.setMetrics(metrics);
   mouseReplayer_.reset();

   if (window_ == nullptr) {
      return;
   }

   float mouseX = 0.0F;
   float mouseY = 0.0F;
   SDL_GetMouseState(&mouseX, &mouseY);
   mouseReplayer_.setCursor(mouseX, mouseY);
}

void Pipeline::clearWindow() {
   window_ = nullptr;
   windowID_ = 0;
   keyReplayer_.setWindowID(0);
   keyReplayer_.reset();
   mouseReplayer_.setWindowID(0);
   mouseReplayer_.reset();
}

void Pipeline::registerCapture(std::optional<CaptureRegion> region,
                               ToolCompletionDelegate& completion) {
   Request request;
   request.completion = &completion;
   request.region = std::move(region);
   request.registeredAt = std::chrono::steady_clock::now();
   pending_.push_back(std::move(request));
}

void Pipeline::registerInput(InputBatch batch,
                             std::optional<CaptureRegion> region,
                             ToolCompletionDelegate& completion) {
   Request request;
   request.completion = &completion;
   request.region = std::move(region);
   request.actionCount = batch.actions.size();
   request.isInput = true;
   request.registeredAt = std::chrono::steady_clock::now();
   request.batch = std::move(batch);
   registered_.push_back(std::move(request));
}

void Pipeline::writeFrame(const std::uint8_t* data,
                          std::size_t size,
                          int width,
                          int height,
                          std::uint64_t stamp) {
   if (pending_.empty()) {
      return;
   }

   const Result<void, ProtocolError> valid =
       validateFrame(data, size, width, height, maxFrameBytes_);
   if (!valid) {
      ++stats_.dropped;
      stats_.lastError = valid.error().message;
      return;
   }

   Frame frame;
   frame.width = width;
   frame.height = height;
   frame.frame = stamp;
   frame.pixels.assign(data, data + size);
   written_ = std::move(frame);
}

void Pipeline::discardRequests() {
   registered_.clear();
   failed_.clear();
   pending_.clear();
   written_.reset();
}

void Pipeline::releaseHeldInput() {
   std::vector<SDL_Event> events;
   const Uint64 timestamp = SDL_GetTicksNS();
   keyReplayer_.releaseAllKeys(timestamp, events);
   mouseReplayer_.releaseAllButtons(timestamp, events);

   if (windowID_ != 0) {
      for (SDL_Event& event : events) {
         SDL_PushEvent(&event);
      }
   }
   SDL_SetModState(keyReplayer_.modifiers());
}

CaptureInfo Pipeline::captureInfo() const {
   CaptureInfo info = stats_;
   info.pending = frameRequested();
   return info;
}

}  // namespace sdl3mcp
