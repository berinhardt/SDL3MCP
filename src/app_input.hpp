/*
  The app-thread batch plan: one InputBatch turned into SDL events without
  pushing any of them.

  AppIterate needs three things from a batch before it can touch SDL's queue:
  the events per action (so a warp can be performed just before its action), the
  key and mouse state the batch ends in, and the guarantee that a partial batch
  never leaks into the app. buildBatchPlan therefore applies the actions to
  copies of both replayers, collects the events into per-action vectors, and
  commits everything into `plan` only after the last action succeeded.

  The plan is a value: the caller can inspect it, and on failure it still holds
  the state it was constructed with. Text events borrow the batch's strings
  (SDL's text event is a pointer), so the caller must keep the InputBatch alive
  until the app has polled the events.

  Everything here calls SDL or the KeyMap, so it is app-thread only.
*/

#ifndef SDL3MCP_APP_INPUT_HPP
#define SDL3MCP_APP_INPUT_HPP

#include "input.hpp"
#include "mouse.hpp"
#include "replay.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <vector>

namespace sdl3mcp
{

struct AppliedBatchPlan
{
    /*
      The replayers seed the plan with the live state, so a failed build leaves
      the plan exactly as the caller prepared it.
    */
    AppliedBatchPlan(const KeyReplayer &keyReplayer, const MouseReplayer &mouseReplayer)
        : keyState(keyReplayer), mouseState(mouseReplayer)
    {
    }

    /* One entry per action, in batch order; empty vectors for silent actions. */
    std::vector<std::vector<SDL_Event>> eventsByAction;

    /* The warp for that action, if it asked for one; never set for other actions. */
    std::vector<std::optional<SDL_FPoint>> warpsByAction;

    KeyReplayer keyState;
    MouseReplayer mouseState;
};

/*
  Builds the plan for `batch` from copies of the two replayers. `windowID` and
  `textInputActive` describe the window the events target; `timestamp` is stamped
  on every event. Returns false with one line in `error`, prefixed with the
  0-based action index, and leaves `plan` untouched when any action fails.
*/
bool buildBatchPlan(const InputBatch &batch, const KeyReplayer &keyReplayer,
                    const MouseReplayer &mouseReplayer, SDL_WindowID windowID,
                    bool textInputActive, Uint64 timestamp, AppliedBatchPlan &plan,
                    std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_APP_INPUT_HPP */
