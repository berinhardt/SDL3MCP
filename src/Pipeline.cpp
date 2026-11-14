#include "Pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "AppInput.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

/* True when any action in the batch carries text, so text input must be active. */
bool batchHasText(const InputBatch& batch) {
   return std::any_of(batch.actions.begin(), batch.actions.end(),
                      [](const InputAction& action) {
                         return std::holds_alternative<TextInput>(action);
                      });
}

/*
  Starts text input for the lifetime of one batch, because synthetic text needs
  no keyboard focus, and stops it again only when this scope started it, so the
  previous state is restored on every exit path.
*/
class TextInputScope {
  public:
   TextInputScope(SDL_Window* window, bool needed) : window_(window) {
      if (needed && window_ != nullptr && !SDL_TextInputActive(window_)) {
         started_ = SDL_StartTextInput(window_);
      }
   }

   ~TextInputScope() {
      if (started_) {
         SDL_StopTextInput(window_);
      }
   }

   TextInputScope(const TextInputScope&) = delete;
   TextInputScope& operator=(const TextInputScope&) = delete;

  private:
   SDL_Window* window_ = nullptr;
   bool started_ = false;
};

/* True when every event reached SDL's queue; modifiers are mirrored either way. */
bool pushPlanEvents(SDL_Window* window, const KeyReplayer& keyReplayer,
                    AppliedBatchPlan& plan) {
   bool allPushed = true;
   for (std::size_t index = 0; index < plan.eventsByAction.size(); ++index) {
      const std::optional<SDL_FPoint>& warp = plan.warpsByAction[index];
      if (warp.has_value() && window != nullptr) {
         SDL_WarpMouseInWindow(window, warp->x, warp->y);
      }
      for (SDL_Event& event : plan.eventsByAction[index]) {
         if (!SDL_PushEvent(&event)) {
            allPushed = false;
            break;
         }
      }
      if (!allPushed) {
         break;
      }
   }
   SDL_SetModState(keyReplayer.modifiers());
   return allPushed;
}

double elapsedMs(std::chrono::steady_clock::time_point started) {
   return std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - started)
       .count();
}

}  // namespace

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

void Pipeline::applyRegisteredInputs(std::uint64_t frame) {
   for (Request& request : registered_) {
      if (request.isInput) {
         applyInput(request, frame);
      }
   }
   registered_.clear();
}

void Pipeline::applyInput(Request& request, std::uint64_t frame) {
   TextInputScope textInput(window_, batchHasText(request.batch));
   const bool textInputActive =
       window_ != nullptr && SDL_TextInputActive(window_);

   AppliedBatchPlan plan{keyReplayer_, mouseReplayer_};
   std::string error;
   if (!buildBatchPlan(request.batch, keyReplayer_, mouseReplayer_, windowID_,
                       textInputActive, SDL_GetTicksNS(), plan, error)) {
      request.error = std::move(error);
      failed_.push_back(std::move(request));
      return;
   }

   keyReplayer_ = plan.keyState;
   mouseReplayer_ = plan.mouseState;

   if (!pushPlanEvents(window_, keyReplayer_, plan)) {
      request.error = "SDL event queue full";
      failed_.push_back(std::move(request));
      return;
   }

   appliedFrame_ = frame;
   request.appliedFrame = frame;
   pending_.push_back(std::move(request));
}

void Pipeline::completeFailures() {
   for (Request& request : failed_) {
      request.completion->complete(
          ToolResult::text(std::move(request.error), true));
   }
   failed_.clear();
}

void Pipeline::completeWrittenFrame() {
   if (!written_.has_value()) {
      return;
   }

   const Frame frame = std::move(*written_);
   written_.reset();

   std::vector<Request> requests = std::move(pending_);
   pending_.clear();
   for (Request& request : requests) {
      completeRequest(request, frame);
   }
}

void Pipeline::completeRequest(Request& request, const Frame& frame) {
   Frame answer = frame;
   if (request.region.has_value()) {
      std::string error;
      std::optional<Frame> cropped = cropFrame(frame, *request.region, error);
      if (!cropped.has_value()) {
         stats_.lastError = error;
         request.completion->complete(ToolResult::text(error, true));
         return;
      }
      answer = std::move(*cropped);
   }

   ++stats_.count;
   stats_.lastWidth = answer.width;
   stats_.lastHeight = answer.height;
   stats_.lastBytes = answer.pixels.size();
   stats_.lastMs = elapsedMs(request.registeredAt);
   stats_.lastError.clear();

   std::string text;
   if (request.isInput && request.appliedFrame.has_value()) {
      text = std::to_string(request.actionCount) + " actions applied at frame " +
             std::to_string(*request.appliedFrame) + "; frame " +
             std::to_string(answer.frame) + " attached";
   }

   request.completion->complete(ToolResult::rawFrame(
       answer.width, answer.height, answer.frame, std::move(answer.pixels),
       std::move(text)));
}

void Pipeline::writeFrame(const std::uint8_t* data,
                          std::size_t size,
                          int width,
                          int height,
                          std::uint64_t stamp) {
   if (pending_.empty()) {
      return;
   }

   const std::optional<std::string> error =
       validateFrame(data, size, width, height, maxFrameBytes_);
   if (error.has_value()) {
      ++stats_.dropped;
      stats_.lastError = *error;
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
