/*
  The frame loop inside the Runtime: one session pump, one application of the
  registered batches, and the single SDL_EVENT_QUIT a terminate request pushes.
  The state, the lifecycle and the catalog are in Runtime.cpp and
  Runtime-Catalog.cpp.
*/

#include "Runtime.h"

namespace sdl3mcp {

SDL_AppResult Runtime::iterate() {
   /*
     The frame the app wrote since the last iterate serves every request that was
     registered before it. Completing first keeps a request registered in this
     pump from being answered by the frame the app wrote for the previous one.
     A batch that failed to apply answers here too, one frame after it was
     applied, with an error and no frame.
   */
   pipeline_.completeFailures();
   pipeline_.completeWrittenFrame();

   /*
     The pump is the server loop: every available line is answered here, on the
     app thread. Input and capture register their requests in this pass and are
     applied to the frame below.
   */
   if (session_) {
      session_->pump();
   }

   ++frameCounter_;
   pipeline_.applyRegisteredInputs(frameCounter_);

   /*
     A transport error during the pump retires the session's orders. Drop the
     requests they were going to answer instead of completing into a closed
     stream.
   */
   if (session_ && !session_->isOpen()) {
      pipeline_.discardRequests();
   }

#ifdef __EMSCRIPTEN__
   /*
     The browser's transport is the WebSocket, and a page with no client has
     nothing left to serve: a session that had opened and then ended, or a dial
     that reported an error, quits the app once. The dial is asynchronous, so
     "not open" is also the normal state between AppInit and the first frame;
     only a session that has opened (or failed) counts as an end.
   */
   if (session_) {
      if (session_->isOpen()) {
         sessionOpened_ = true;
      } else if (!sessionClosed_ &&
                 (sessionOpened_ || !session_->lastError().empty())) {
         sessionClosed_ = true;
         terminateRequested_ = true;
      }
   }
#endif

   /*
     One quit request makes one SDL_EVENT_QUIT. A failed push restores the flag
     so the next iterate retries instead of losing the request.
   */
   if (terminateRequested_) {
      terminateRequested_ = false;
      SDL_Event event{};
      event.type = SDL_EVENT_QUIT;
      if (!SDL_PushEvent(&event)) {
         terminateRequested_ = true;
      }
   }

   return SDL_APP_CONTINUE;
}

}  // namespace sdl3mcp
