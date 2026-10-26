/*
  Mouse replay: turns a mouse-move, button, or scroll action into SDL events and
  tracks the virtual cursor and the held buttons.

  The replayer never warps the OS cursor itself. apply() returns a pending warp
  in `warp` for the caller to perform with SDL_WarpMouseInWindow on the app
  thread, so motion replay stays a pure event builder that needs no window.

  Coordinates are window points and clamp to [0, max(0, width - 1)] and
  [0, max(0, height - 1)], so a zero-size window clamps to the origin instead of
  failing. A warp replayer emits zero xrel/yrel because the platform's warp event
  already carries the delta; a non-warping or relative-mode move passes the
  derived delta through.

  apply() validates before it touches state or the event vector, so a failed
  action changes neither. A batch applies its actions to a copy and commits that
  copy only once every action succeeded, so the type is a copyable value type.
*/

#ifndef SDL3MCP_MOUSE_HPP
#define SDL3MCP_MOUSE_HPP

#include "input.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <vector>

namespace sdl3mcp
{

/* Window geometry snapshot taken on the app thread; relative mode is SDL's. */
struct MouseMetrics
{
    float width = 0.0F;
    float height = 0.0F;
    bool relativeMode = false;
    bool warpCursor = true;
};

class MouseReplayer
{
public:
    MouseReplayer(SDL_WindowID windowID, MouseMetrics metrics);

    MouseReplayer(const MouseReplayer &) = default;
    MouseReplayer &operator=(const MouseReplayer &) = default;

    /*
      Appends the motion event for one move. Absolute moves land on the clamped
      target; a warp replayer reports that target in `warp` and leaves the deltas
      at zero, a relative or non-warping move reports the delta from the old
      cursor. Relative moves treat x/y as dx/dy and never warp. Returns false with
      one line of text in `error` when no window is bound.
    */
    bool apply(const MouseMoveInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
               std::optional<SDL_FPoint> &warp, std::string &error);

    /*
      Appends the down, up, or down-then-up events for one button action. An
      optional x/y moves the virtual cursor first, without a motion event. A down
      of a held button and an up of a non-held button are refused.
    */
    bool apply(const MouseButtonInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
               std::string &error);

    /* Appends one wheel event at the current virtual cursor. */
    bool apply(const ScrollInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
               std::string &error);

    /* Releases every held button in reverse press order, ending at none. */
    void releaseAllButtons(Uint64 timestamp, std::vector<SDL_Event> &events);

    /* Forgets held buttons and returns the virtual cursor to (0, 0). */
    void reset() noexcept;

    void setWindowID(SDL_WindowID windowID) noexcept { windowID_ = windowID; }
    void setMetrics(MouseMetrics metrics) noexcept { metrics_ = metrics; }
    void setCursor(float x, float y) noexcept;

    float cursorX() const noexcept { return cursorX_; }
    float cursorY() const noexcept { return cursorY_; }
    const std::vector<MouseButton> &heldButtons() const noexcept { return heldButtons_; }

private:
    bool isHeld(MouseButton button) const noexcept;

    void pressButton(MouseButton button, Uint64 timestamp, int clicks,
                     std::vector<SDL_Event> &events);
    void releaseButton(MouseButton button, Uint64 timestamp, int clicks,
                       std::vector<SDL_Event> &events);
    void appendButtonEvent(std::vector<SDL_Event> &events, MouseButton button, bool down,
                           Uint64 timestamp, int clicks) const;
    void appendMotion(std::vector<SDL_Event> &events, Uint64 timestamp, float x, float y,
                      float xrel, float yrel) const;

    /* The held buttons as SDL's mouse button mask, which motion events carry. */
    SDL_MouseButtonFlags buttonMask() const noexcept;

    float clampX(float x) const noexcept;
    float clampY(float y) const noexcept;

    SDL_WindowID windowID_;
    MouseMetrics metrics_;
    float cursorX_ = 0.0F;
    float cursorY_ = 0.0F;
    std::vector<MouseButton> heldButtons_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_MOUSE_HPP */
