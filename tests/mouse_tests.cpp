#include "mouse.hpp"

#include "test_util.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace
{

using sdl3mcp::MouseButton;
using sdl3mcp::MouseButtonAction;
using sdl3mcp::MouseButtonInput;
using sdl3mcp::MouseMetrics;
using sdl3mcp::MouseMoveInput;
using sdl3mcp::MouseReplayer;
using sdl3mcp::ScrollInput;

constexpr SDL_WindowID kWindowID = 9;
constexpr Uint64 kTimestamp = 42424242;

constexpr SDL_MouseButtonFlags kLeftMask = SDL_BUTTON_LMASK;
constexpr SDL_MouseButtonFlags kRightMask = SDL_BUTTON_RMASK;

/* A 100x50 window whose cursor warps, the default configuration. */
MouseReplayer makeReplayer()
{
    return MouseReplayer(kWindowID, MouseMetrics{100.0F, 50.0F, false, true});
}

MouseReplayer makeRelativeModeReplayer()
{
    return MouseReplayer(kWindowID, MouseMetrics{100.0F, 50.0F, true, true});
}

MouseReplayer makeNonWarpingReplayer()
{
    return MouseReplayer(kWindowID, MouseMetrics{100.0F, 50.0F, false, false});
}

MouseMoveInput absoluteMove(float x, float y)
{
    return MouseMoveInput{MouseMoveInput::Mode::Absolute, x, y};
}

MouseMoveInput relativeMove(float dx, float dy)
{
    return MouseMoveInput{MouseMoveInput::Mode::Relative, dx, dy};
}

MouseButtonInput buttonInput(MouseButton button, MouseButtonAction action)
{
    return MouseButtonInput{button, action, std::nullopt, std::nullopt, 1};
}

MouseButtonInput buttonInputAt(MouseButton button, MouseButtonAction action, float x, float y,
                               int clicks)
{
    return MouseButtonInput{button, action, x, y, clicks};
}

void expectMotion(const SDL_Event &event, float x, float y, float xrel, float yrel,
                  SDL_MouseButtonFlags state)
{
    CHECK(event.type == SDL_EVENT_MOUSE_MOTION);
    CHECK(event.motion.windowID == kWindowID);
    CHECK(event.motion.which == 0);
    CHECK(event.motion.timestamp == kTimestamp);
    CHECK(event.motion.state == state);
    CHECK(event.motion.x == x);
    CHECK(event.motion.y == y);
    CHECK(event.motion.xrel == xrel);
    CHECK(event.motion.yrel == yrel);
}

void expectButton(const SDL_Event &event, Uint8 button, bool down, Uint8 clicks, float x, float y)
{
    CHECK(event.type == (down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP));
    CHECK(event.button.windowID == kWindowID);
    CHECK(event.button.which == 0);
    CHECK(event.button.timestamp == kTimestamp);
    CHECK(event.button.button == button);
    CHECK(event.button.down == down);
    CHECK(event.button.clicks == clicks);
    CHECK(event.button.x == x);
    CHECK(event.button.y == y);
}

/* Applies one move that must succeed, requiring exactly one motion event. */
SDL_Event expectMove(MouseReplayer &replayer, const MouseMoveInput &input,
                     std::vector<SDL_Event> &events, std::optional<SDL_FPoint> &warp,
                     std::string &error)
{
    events.clear();
    warp.reset();
    CHECK(replayer.apply(input, kTimestamp, events, warp, error));
    CHECK(error.empty());
    CHECK(events.size() == 1);
    return events.front();
}

/* Applies one button action that must fail, leaving events, warp, and state untouched. */
void expectButtonRejected(MouseReplayer &replayer, const MouseButtonInput &input,
                          std::vector<SDL_Event> &events, std::optional<SDL_FPoint> &warp,
                          std::string &error, const std::string &message)
{
    const std::size_t before = events.size();
    const std::size_t heldBefore = replayer.heldButtons().size();
    warp.reset();
    CHECK(!replayer.apply(input, kTimestamp, events, warp, error));
    CHECK(events.size() == before);
    CHECK(replayer.heldButtons().size() == heldBefore);
    CHECK(!warp.has_value());
    CHECK(error == message);
}

void testAbsoluteMoveWarps()
{
    MouseReplayer replayer = makeReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    const SDL_Event event = expectMove(replayer, absoluteMove(20.0F, 15.0F), events, warp, error);

    /* The warp carries the delta, so the synthetic event leaves it at zero. */
    expectMotion(event, 20.0F, 15.0F, 0.0F, 0.0F, 0);
    CHECK(warp.has_value());
    CHECK(warp->x == 20.0F);
    CHECK(warp->y == 15.0F);
    CHECK(replayer.cursorX() == 20.0F);
    CHECK(replayer.cursorY() == 15.0F);
}

void testAbsoluteMoveWithoutWarp()
{
    MouseReplayer replayer = makeNonWarpingReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    const SDL_Event event = expectMove(replayer, absoluteMove(20.0F, 15.0F), events, warp, error);

    CHECK(!warp.has_value());
    expectMotion(event, 20.0F, 15.0F, 10.0F, 5.0F, 0);
    CHECK(replayer.cursorX() == 20.0F);
    CHECK(replayer.cursorY() == 15.0F);
}

void testAbsoluteMoveInRelativeMode()
{
    MouseReplayer replayer = makeRelativeModeReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    const SDL_Event event = expectMove(replayer, absoluteMove(20.0F, 15.0F), events, warp, error);

    /* Relative mode never warps: the absolute target becomes a delta. */
    CHECK(!warp.has_value());
    expectMotion(event, 20.0F, 15.0F, 10.0F, 5.0F, 0);
    CHECK(replayer.cursorX() == 20.0F);
    CHECK(replayer.cursorY() == 15.0F);
}

void testRelativeMove()
{
    MouseReplayer replayer = makeReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    const SDL_Event event = expectMove(replayer, relativeMove(5.0F, -3.0F), events, warp, error);

    /* Deltas pass through untouched even though this replayer would warp an
       absolute move. */
    CHECK(!warp.has_value());
    expectMotion(event, 15.0F, 7.0F, 5.0F, -3.0F, 0);
    CHECK(replayer.cursorX() == 15.0F);
    CHECK(replayer.cursorY() == 7.0F);
}

void testClamping()
{
    MouseReplayer replayer = makeReplayer();
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    const SDL_Event high = expectMove(replayer, absoluteMove(-5.0F, 200.0F), events, warp, error);
    expectMotion(high, 0.0F, 49.0F, 0.0F, 0.0F, 0);
    CHECK(warp.has_value());
    CHECK(warp->x == 0.0F);
    CHECK(warp->y == 49.0F);

    /* Relative input clamps the cursor but reports the raw delta. */
    const SDL_Event low = expectMove(replayer, relativeMove(-10.0F, -10.0F), events, warp, error);
    expectMotion(low, 0.0F, 39.0F, -10.0F, -10.0F, 0);
    CHECK(replayer.cursorX() == 0.0F);
    CHECK(replayer.cursorY() == 39.0F);

    /* A zero-size window clamps to the origin instead of failing. */
    MouseReplayer emptyWindow(kWindowID, MouseMetrics{0.0F, 0.0F, false, true});
    const SDL_Event collapsed =
        expectMove(emptyWindow, absoluteMove(20.0F, 15.0F), events, warp, error);
    expectMotion(collapsed, 0.0F, 0.0F, 0.0F, 0.0F, 0);
    CHECK(emptyWindow.cursorX() == 0.0F);
    CHECK(emptyWindow.cursorY() == 0.0F);
}

void testMotionCarriesHeldButtons()
{
    MouseReplayer replayer = makeReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    CHECK(replayer.apply(buttonInput(MouseButton::Left, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    CHECK(replayer.apply(buttonInput(MouseButton::Right, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    CHECK(error.empty());

    const SDL_Event both = expectMove(replayer, absoluteMove(10.0F, 10.0F), events, warp, error);
    expectMotion(both, 10.0F, 10.0F, 0.0F, 0.0F, kLeftMask | kRightMask);

    CHECK(replayer.apply(buttonInput(MouseButton::Left, MouseButtonAction::Up), kTimestamp,
                         events, warp, error));
    const SDL_Event rightOnly =
        expectMove(replayer, absoluteMove(10.0F, 10.0F), events, warp, error);
    expectMotion(rightOnly, 10.0F, 10.0F, 0.0F, 0.0F, kRightMask);
}

void testClickMovesCursorAndEmitsPair()
{
    MouseReplayer replayer = makeReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    /* Coordinates make the action a move followed by the pair, in one action. */
    CHECK(replayer.apply(buttonInputAt(MouseButton::Left, MouseButtonAction::Click, 30.0F, 20.0F,
                                      2),
                         kTimestamp, events, warp, error));
    CHECK(error.empty());
    CHECK(events.size() == 3);
    expectMotion(events[0], 30.0F, 20.0F, 0.0F, 0.0F, 0);
    expectButton(events[1], SDL_BUTTON_LEFT, true, 2, 30.0F, 20.0F);
    expectButton(events[2], SDL_BUTTON_LEFT, false, 2, 30.0F, 20.0F);
    CHECK(warp.has_value());
    CHECK(warp->x == 30.0F);
    CHECK(warp->y == 20.0F);
    CHECK(replayer.cursorX() == 30.0F);
    CHECK(replayer.cursorY() == 20.0F);
    CHECK(replayer.heldButtons().empty());

    /* Without coordinates the click lands on the current cursor with no motion. */
    events.clear();
    warp.reset();
    CHECK(replayer.apply(buttonInput(MouseButton::Left, MouseButtonAction::Click), kTimestamp,
                         events, warp, error));
    CHECK(events.size() == 2);
    CHECK(!warp.has_value());
    expectButton(events[0], SDL_BUTTON_LEFT, true, 1, 30.0F, 20.0F);
    expectButton(events[1], SDL_BUTTON_LEFT, false, 1, 30.0F, 20.0F);
}

void testPositionedDownMovesFirst()
{
    MouseReplayer replayer = makeReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    CHECK(replayer.apply(buttonInput(MouseButton::Right, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    CHECK(!warp.has_value());
    events.clear();

    /* The motion lands on the clamped target and carries the held right button. */
    CHECK(replayer.apply(
        buttonInputAt(MouseButton::Left, MouseButtonAction::Down, 200.0F, -5.0F, 1), kTimestamp,
        events, warp, error));
    CHECK(events.size() == 2);
    expectMotion(events[0], 99.0F, 0.0F, 0.0F, 0.0F, kRightMask);
    expectButton(events[1], SDL_BUTTON_LEFT, true, 1, 99.0F, 0.0F);
    CHECK(warp.has_value());
    CHECK(warp->x == 99.0F);
    CHECK(warp->y == 0.0F);
}

void testPositionedButtonInRelativeMode()
{
    MouseReplayer replayer = makeRelativeModeReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    /* Relative mode never warps: the absolute target becomes a delta. */
    CHECK(replayer.apply(buttonInputAt(MouseButton::Left, MouseButtonAction::Click, 25.0F, 30.0F,
                                      1),
                         kTimestamp, events, warp, error));
    CHECK(events.size() == 3);
    CHECK(!warp.has_value());
    expectMotion(events[0], 25.0F, 30.0F, 15.0F, 20.0F, 0);
    expectButton(events[1], SDL_BUTTON_LEFT, true, 1, 25.0F, 30.0F);
    expectButton(events[2], SDL_BUTTON_LEFT, false, 1, 25.0F, 30.0F);
}

void testPositionedButtonWithoutWarp()
{
    MouseReplayer replayer = makeNonWarpingReplayer();
    replayer.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    /* warp_cursor = false still synthesises the motion, with derived deltas. */
    CHECK(replayer.apply(buttonInputAt(MouseButton::Left, MouseButtonAction::Click, 20.0F, 15.0F,
                                      1),
                         kTimestamp, events, warp, error));
    CHECK(events.size() == 3);
    CHECK(!warp.has_value());
    expectMotion(events[0], 20.0F, 15.0F, 10.0F, 5.0F, 0);
    expectButton(events[1], SDL_BUTTON_LEFT, true, 1, 20.0F, 15.0F);
    expectButton(events[2], SDL_BUTTON_LEFT, false, 1, 20.0F, 15.0F);
}

void testButtonDownUpAndHeldState()
{
    MouseReplayer replayer = makeReplayer();
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    CHECK(replayer.apply(buttonInput(MouseButton::Right, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    CHECK(events.size() == 1);
    expectButton(events[0], SDL_BUTTON_RIGHT, true, 1, 0.0F, 0.0F);
    CHECK(replayer.heldButtons().size() == 1);

    /* A down of a held button is refused. */
    expectButtonRejected(replayer, buttonInput(MouseButton::Right, MouseButtonAction::Down),
                         events, warp, error, "mouse button is already held");

    CHECK(replayer.apply(buttonInput(MouseButton::Right, MouseButtonAction::Up), kTimestamp,
                         events, warp, error));
    CHECK(events.size() == 2);
    expectButton(events[1], SDL_BUTTON_RIGHT, false, 1, 0.0F, 0.0F);
    CHECK(replayer.heldButtons().empty());

    /* An up of a non-held button is refused, as is a click on a held one. */
    expectButtonRejected(replayer, buttonInput(MouseButton::Right, MouseButtonAction::Up), events,
                         warp, error, "mouse button is not held");
    CHECK(replayer.apply(buttonInput(MouseButton::Right, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    expectButtonRejected(replayer, buttonInput(MouseButton::Right, MouseButtonAction::Click),
                         events, warp, error, "mouse button is already held");
}

void testButtonMapping()
{
    MouseReplayer replayer = makeReplayer();
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    CHECK(replayer.apply(buttonInput(MouseButton::Middle, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    CHECK(replayer.apply(buttonInput(MouseButton::Middle, MouseButtonAction::Up), kTimestamp,
                         events, warp, error));
    CHECK(replayer.apply(buttonInput(MouseButton::X1, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    CHECK(replayer.apply(buttonInput(MouseButton::X1, MouseButtonAction::Up), kTimestamp,
                         events, warp, error));
    CHECK(replayer.apply(buttonInput(MouseButton::X2, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    CHECK(replayer.apply(buttonInput(MouseButton::X2, MouseButtonAction::Up), kTimestamp,
                         events, warp, error));
    CHECK(events.size() == 6);

    expectButton(events[0], SDL_BUTTON_MIDDLE, true, 1, 0.0F, 0.0F);
    expectButton(events[1], SDL_BUTTON_MIDDLE, false, 1, 0.0F, 0.0F);
    expectButton(events[2], SDL_BUTTON_X1, true, 1, 0.0F, 0.0F);
    expectButton(events[3], SDL_BUTTON_X1, false, 1, 0.0F, 0.0F);
    expectButton(events[4], SDL_BUTTON_X2, true, 1, 0.0F, 0.0F);
    expectButton(events[5], SDL_BUTTON_X2, false, 1, 0.0F, 0.0F);
}

void testScroll()
{
    MouseReplayer replayer = makeReplayer();
    replayer.setCursor(12.0F, 8.0F);
    std::vector<SDL_Event> events;
    std::string error;

    CHECK(replayer.apply(ScrollInput{0.0F, 240.0F}, kTimestamp, events, error));
    CHECK(error.empty());
    CHECK(events.size() == 1);

    const SDL_Event &event = events[0];
    CHECK(event.type == SDL_EVENT_MOUSE_WHEEL);
    CHECK(event.wheel.windowID == kWindowID);
    CHECK(event.wheel.which == 0);
    CHECK(event.wheel.timestamp == kTimestamp);
    CHECK(event.wheel.x == 0.0F);
    CHECK(event.wheel.y == 240.0F);
    CHECK(event.wheel.direction == SDL_MOUSEWHEEL_NORMAL);
    CHECK(event.wheel.integer_x == 0);
    CHECK(event.wheel.integer_y == 0);
    CHECK(event.wheel.mouse_x == 12.0F);
    CHECK(event.wheel.mouse_y == 8.0F);
}

void testReleaseAllButtons()
{
    MouseReplayer replayer = makeReplayer();
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    /* Nothing held: nothing to emit. */
    replayer.releaseAllButtons(kTimestamp, events);
    CHECK(events.empty());

    CHECK(replayer.apply(buttonInput(MouseButton::Left, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    CHECK(replayer.apply(buttonInput(MouseButton::Right, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    events.clear();

    replayer.releaseAllButtons(kTimestamp, events);
    CHECK(events.size() == 2);
    /* Reverse press order: right, then left. */
    expectButton(events[0], SDL_BUTTON_RIGHT, false, 1, 0.0F, 0.0F);
    expectButton(events[1], SDL_BUTTON_LEFT, false, 1, 0.0F, 0.0F);
    CHECK(replayer.heldButtons().empty());

    replayer.releaseAllButtons(kTimestamp, events);
    CHECK(events.size() == 2);
}

void testReset()
{
    MouseReplayer replayer = makeReplayer();
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    CHECK(replayer.apply(buttonInput(MouseButton::Left, MouseButtonAction::Down), kTimestamp,
                         events, warp, error));
    replayer.setCursor(50.0F, 40.0F);
    events.clear();

    replayer.reset();
    CHECK(events.empty());
    CHECK(replayer.heldButtons().empty());
    CHECK(replayer.cursorX() == 0.0F);
    CHECK(replayer.cursorY() == 0.0F);

    replayer.releaseAllButtons(kTimestamp, events);
    CHECK(events.empty());
}

void testNoWindowBound()
{
    MouseReplayer replayer(0, MouseMetrics{100.0F, 50.0F, false, true});
    std::vector<SDL_Event> events;
    std::optional<SDL_FPoint> warp;
    std::string error;

    CHECK(!replayer.apply(absoluteMove(10.0F, 10.0F), kTimestamp, events, warp, error));
    CHECK(events.empty());
    CHECK(error == "no window bound");
    CHECK(!warp.has_value());

    CHECK(!replayer.apply(buttonInput(MouseButton::Left, MouseButtonAction::Click), kTimestamp,
                          events, warp, error));
    CHECK(events.empty());
    CHECK(!warp.has_value());
    CHECK(error == "no window bound");

    CHECK(!replayer.apply(ScrollInput{0.0F, 120.0F}, kTimestamp, events, error));
    CHECK(events.empty());
    CHECK(error == "no window bound");
    CHECK(replayer.heldButtons().empty());
}

/*
  A batch applies to a copy and commits it only on success, so a copy that walks
  a valid action and then fails must leave the original's state and events alone.
*/
void testCopyRollsBack()
{
    MouseReplayer original = makeReplayer();
    original.setCursor(10.0F, 10.0F);
    std::vector<SDL_Event> originalEvents;
    std::optional<SDL_FPoint> warp;
    std::string error;

    CHECK(original.apply(buttonInput(MouseButton::Left, MouseButtonAction::Down), kTimestamp,
                         originalEvents, warp, error));
    CHECK(originalEvents.size() == 1);

    MouseReplayer copy = original;
    std::vector<SDL_Event> copyEvents = originalEvents;

    CHECK(copy.apply(absoluteMove(20.0F, 20.0F), kTimestamp, copyEvents, warp, error));
    CHECK(copyEvents.size() == 2);
    CHECK(copy.cursorX() == 20.0F);

    /* Middle was never pressed, so the batch would fail here and be discarded. */
    CHECK(!copy.apply(buttonInput(MouseButton::Middle, MouseButtonAction::Up), kTimestamp,
                      copyEvents, warp, error));
    CHECK(copyEvents.size() == 2);
    CHECK(!warp.has_value());
    CHECK(error == "mouse button is not held");

    CHECK(original.cursorX() == 10.0F);
    CHECK(original.cursorY() == 10.0F);
    CHECK(originalEvents.size() == 1);
    CHECK(original.heldButtons().size() == 1);
    CHECK(original.heldButtons().front() == MouseButton::Left);
}

} // namespace

int main()
{
    testAbsoluteMoveWarps();
    testAbsoluteMoveWithoutWarp();
    testAbsoluteMoveInRelativeMode();
    testRelativeMove();
    testClamping();
    testMotionCarriesHeldButtons();
    testClickMovesCursorAndEmitsPair();
    testPositionedDownMovesFirst();
    testPositionedButtonInRelativeMode();
    testPositionedButtonWithoutWarp();
    testButtonDownUpAndHeldState();
    testButtonMapping();
    testScroll();
    testReleaseAllButtons();
    testReset();
    testNoWindowBound();
    testCopyRollsBack();
    return 0;
}
