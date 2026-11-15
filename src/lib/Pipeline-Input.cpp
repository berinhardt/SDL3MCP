/*
  Applying a registered batch to SDL: the batch plan, the warps and events it
  produces, and the bookkeeping that carries a successful batch to the next
  frame. The pipeline's state and registration are in Pipeline.cpp; answering a
  written frame is in Pipeline-Frames.cpp.
*/

#include "Pipeline.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "AppInput.h"
#include "TextInputScope.h"

namespace sdl3mcp {
namespace {

/* True when any action in the batch carries text, so text input must be active. */
bool batchHasText(const InputBatch& batch) {
   return std::any_of(batch.actions.begin(), batch.actions.end(),
                      [](const InputAction& action) {
                         return std::holds_alternative<TextInput>(action);
                      });
}

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

}  // namespace

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
   const Result<void, ProtocolError> applied =
       buildBatchPlan(request.batch, keyReplayer_, mouseReplayer_, windowID_,
                      textInputActive, SDL_GetTicksNS(), plan);
   if (!applied) {
      request.error = applied.error().message;
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

}  // namespace sdl3mcp
