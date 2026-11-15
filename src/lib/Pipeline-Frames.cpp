/*
  Answering the requests a written frame serves: the crop per request, the
  capture statistics, and the failure report a batch that never applied sends
  one frame later. The pipeline's state and registration are in Pipeline.cpp;
  applying a batch is in Pipeline-Input.cpp.
*/

#include "Pipeline.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "Frame.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

double elapsedMs(std::chrono::steady_clock::time_point started) {
   return std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - started)
       .count();
}

}  // namespace

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
      Result<Frame, ProtocolError> cropped = cropFrame(frame, *request.region);
      if (!cropped) {
         stats_.lastError = cropped.error().message;
         request.completion->complete(
             ToolResult::text(std::move(cropped.error().message), true));
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
             std::to_string(answer.frame) +
             " attached, rendered after the actions were delivered";
   }

   request.completion->complete(ToolResult::rawFrame(
       answer.width, answer.height, answer.frame, std::move(answer.pixels),
       std::move(text)));
}

}  // namespace sdl3mcp
