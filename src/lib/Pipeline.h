#pragma once

/*
  The single app-thread input and capture pipeline.

  There is no InputQueue and no CaptureState: the library is single-threaded, so
  the pipeline is plain state on the app thread. A capture is a no-op input: a
  request with no actions that still needs the next frame. Both tools add a
  request, and one frame serves every request registered before it.

  A request registered during the pump at frame F is served by the frame
  rendered at F+1. AppIterate pumps first, so a request registered there is
  answered by a frame the loop has not produced yet; the app writes one frame
  with SDL3MCP_WriteFrame, and the pipeline crops it per request and completes
  every request at F+1. The frame is stamped with frameCounter + 1 by the C
  boundary, which is the index of the frame whose pixels it holds.

  All available sdl_input batches in a pump are applied in order at F; each
  batch is atomic. The batch stays alive until F+1 because SDL's text events
  borrow its strings. A failed batch completes with an error and no frame at
  F+1, and does not disturb the other requests sharing the frame.

  A dropped WriteFrame leaves the request pending: FrameRequested stays true and
  the app may retry. There is no library timeout, so a request that never gets a
  frame stays pending until the session closes.

  Every method is app-thread only. Nothing here calls SDL except setWindow,
  releaseHeldInput and the batch application, which are app-thread work by
  definition.
*/

#include <SDL3/SDL.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Capture.h"
#include "Frame.h"
#include "Input.h"
#include "KeyMap.h"
#include "Mouse.h"
#include "Replay.h"
#include "Tool.h"

namespace sdl3mcp {

class Pipeline {
  public:
   Pipeline(const KeyMap& keyMap, std::uint32_t maxFrameBytes);
   ~Pipeline();

   Pipeline(const Pipeline&) = delete;
   Pipeline& operator=(const Pipeline&) = delete;

   /* Binds the window synthetic input targets; a null window clears it. */
   void setWindow(SDL_Window* window, SDL_WindowID windowID, MouseMetrics metrics);
   void clearWindow();

   /*
     Applies every input batch registered since the last call, in registration
     order, at `frame`. A batch that fails is held for completeFailures(); a
     successful one joins the frame requests.
   */
   void applyRegisteredInputs(std::uint64_t frame);

   /*
     Completes every pending request with the frame written since the last
     completion. A frame written for an earlier request is consumed once; a
     missing frame leaves the requests pending.
   */
   void completeWrittenFrame();

   /* Completes the batches that failed to apply, one frame after they did. */
   void completeFailures();

   /*
     Registers a request for the next frame. The completion outlives the call
     until the pipeline answers or discards it; it must not be destroyed while
     the request is pending.
   */
   void registerCapture(std::optional<CaptureRegion> region,
                        ToolCompletionDelegate& completion);
   void registerInput(InputBatch batch,
                      std::optional<CaptureRegion> region,
                      ToolCompletionDelegate& completion);

   /*
     The app's answer to the pending requests. Ignored when nothing is pending.
     `stamp` is the frame index the pixels belong to (frameCounter + 1 at the C
     boundary). A rejected frame counts one drop and leaves the requests
     pending.
   */
   void writeFrame(const std::uint8_t* data,
                   std::size_t size,
                   int width,
                   int height,
                   std::uint64_t stamp);

   /* True while any request is registered or waiting for a frame. */
   bool frameRequested() const noexcept {
      return !registered_.empty() || !pending_.empty();
   }

   /* Drops every request without answering it; used when the session closes. */
   void discardRequests();

   /* Releases held keys and buttons as events on the bound window. */
   void releaseHeldInput();

   CaptureInfo captureInfo() const;
   std::uint64_t appliedFrame() const noexcept { return appliedFrame_; }

   const std::vector<SDL_Scancode>& heldKeys() const noexcept {
      return keyReplayer_.heldKeys();
   }
   const std::vector<MouseButton>& heldButtons() const noexcept {
      return mouseReplayer_.heldButtons();
   }

  private:
   struct Request {
      ToolCompletionDelegate* completion = nullptr;
      std::optional<CaptureRegion> region;
      /* The batch an input request applies; empty for a capture. */
      InputBatch batch;
      /* Set for an input request once its batch has been applied. */
      std::optional<std::uint64_t> appliedFrame;
      std::size_t actionCount = 0;
      bool isInput = false;
      /* The apply error a failed batch reports at F+1. */
      std::string error;
      std::chrono::steady_clock::time_point registeredAt;
   };

   void applyInput(Request& request, std::uint64_t frame);
   void completeRequest(Request& request, const Frame& frame);

   std::uint32_t maxFrameBytes_;

   KeyReplayer keyReplayer_;
   MouseReplayer mouseReplayer_;

   SDL_Window* window_ = nullptr;
   SDL_WindowID windowID_ = 0;

   /* Input batches registered during this pump, in arrival order. */
   std::vector<Request> registered_;
   /* Batches that failed to apply and answer at the next frame. */
   std::vector<Request> failed_;
   /* Requests waiting for the next written frame. */
   std::vector<Request> pending_;
   /* The frame written since the last completion, if any. */
   std::optional<Frame> written_;

   std::uint64_t appliedFrame_ = 0;
   CaptureInfo stats_;
};

}  // namespace sdl3mcp
