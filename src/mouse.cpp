#include "mouse.hpp"

#include <algorithm>

namespace sdl3mcp
{
namespace
{

/* Both this file and replay.cpp refuse a missing window with the same message. */
constexpr char kNoWindowError[] = "no window bound";
constexpr char kButtonHeldError[] = "mouse button is already held";
constexpr char kButtonNotHeldError[] = "mouse button is not held";

/* SDL's button numbers, which run 1..5 in the order left, middle, right, x1, x2. */
Uint8 buttonNumber(MouseButton button) noexcept
{
    switch (button) {
    case MouseButton::Left:
        return SDL_BUTTON_LEFT;
    case MouseButton::Middle:
        return SDL_BUTTON_MIDDLE;
    case MouseButton::Right:
        return SDL_BUTTON_RIGHT;
    case MouseButton::X1:
        return SDL_BUTTON_X1;
    case MouseButton::X2:
        return SDL_BUTTON_X2;
    }
    return SDL_BUTTON_LEFT;
}

} // namespace

MouseReplayer::MouseReplayer(SDL_WindowID windowID, MouseMetrics metrics)
    : windowID_(windowID), metrics_(metrics)
{
}

bool MouseReplayer::apply(const MouseMoveInput &input, Uint64 timestamp,
                          std::vector<SDL_Event> &events, std::optional<SDL_FPoint> &warp,
                          std::string &error)
{
    error.clear();
    warp.reset();
    if (windowID_ == 0) {
        error = kNoWindowError;
        return false;
    }

    if (input.mode == MouseMoveInput::Mode::Relative) {
        const float targetX = clampX(cursorX_ + input.x);
        const float targetY = clampY(cursorY_ + input.y);
        /* Relative input already carries the delta; clamping must not rewrite it. */
        appendMotion(events, timestamp, targetX, targetY, input.x, input.y);
        cursorX_ = targetX;
        cursorY_ = targetY;
        return true;
    }

    const float targetX = clampX(input.x);
    const float targetY = clampY(input.y);
    if (metrics_.warpCursor && !metrics_.relativeMode) {
        appendMotion(events, timestamp, targetX, targetY, 0.0F, 0.0F);
        warp = SDL_FPoint{targetX, targetY};
    } else {
        appendMotion(events, timestamp, targetX, targetY, targetX - cursorX_,
                     targetY - cursorY_);
    }
    cursorX_ = targetX;
    cursorY_ = targetY;
    return true;
}

bool MouseReplayer::apply(const MouseButtonInput &input, Uint64 timestamp,
                          std::vector<SDL_Event> &events, std::string &error)
{
    error.clear();
    if (windowID_ == 0) {
        error = kNoWindowError;
        return false;
    }

    const bool held = isHeld(input.button);
    switch (input.action) {
    case MouseButtonAction::Down:
    case MouseButtonAction::Click:
        if (held) {
            error = kButtonHeldError;
            return false;
        }
        break;
    case MouseButtonAction::Up:
        if (!held) {
            error = kButtonNotHeldError;
            return false;
        }
        break;
    }

    /* A click's coordinates apply before its events so both carry the new cursor. */
    if (input.x.has_value()) {
        cursorX_ = clampX(*input.x);
    }
    if (input.y.has_value()) {
        cursorY_ = clampY(*input.y);
    }

    switch (input.action) {
    case MouseButtonAction::Down:
        pressButton(input.button, timestamp, input.clicks, events);
        break;
    case MouseButtonAction::Up:
        releaseButton(input.button, timestamp, input.clicks, events);
        break;
    case MouseButtonAction::Click:
        pressButton(input.button, timestamp, input.clicks, events);
        releaseButton(input.button, timestamp, input.clicks, events);
        break;
    }
    return true;
}

bool MouseReplayer::apply(const ScrollInput &input, Uint64 timestamp,
                          std::vector<SDL_Event> &events, std::string &error)
{
    error.clear();
    if (windowID_ == 0) {
        error = kNoWindowError;
        return false;
    }

    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.timestamp = timestamp;
    event.wheel.windowID = windowID_;
    event.wheel.which = 0;
    event.wheel.x = input.x;
    event.wheel.y = input.y;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    event.wheel.mouse_x = cursorX_;
    event.wheel.mouse_y = cursorY_;
    event.wheel.integer_x = 0;
    event.wheel.integer_y = 0;
    events.push_back(event);
    return true;
}

void MouseReplayer::releaseAllButtons(Uint64 timestamp, std::vector<SDL_Event> &events)
{
    /* Popping the tail releases in reverse press order without invalidating iterators. */
    while (!heldButtons_.empty()) {
        releaseButton(heldButtons_.back(), timestamp, 1, events);
    }
}

void MouseReplayer::reset() noexcept
{
    heldButtons_.clear();
    cursorX_ = 0.0F;
    cursorY_ = 0.0F;
}

void MouseReplayer::setCursor(float x, float y) noexcept
{
    cursorX_ = clampX(x);
    cursorY_ = clampY(y);
}

bool MouseReplayer::isHeld(MouseButton button) const noexcept
{
    return std::find(heldButtons_.begin(), heldButtons_.end(), button) != heldButtons_.end();
}

void MouseReplayer::pressButton(MouseButton button, Uint64 timestamp, int clicks,
                                std::vector<SDL_Event> &events)
{
    heldButtons_.push_back(button);
    appendButtonEvent(events, button, true, timestamp, clicks);
}

void MouseReplayer::releaseButton(MouseButton button, Uint64 timestamp, int clicks,
                                  std::vector<SDL_Event> &events)
{
    const auto held = std::find(heldButtons_.begin(), heldButtons_.end(), button);
    heldButtons_.erase(held);
    appendButtonEvent(events, button, false, timestamp, clicks);
}

void MouseReplayer::appendButtonEvent(std::vector<SDL_Event> &events, MouseButton button,
                                      bool down, Uint64 timestamp, int clicks) const
{
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.timestamp = timestamp;
    event.button.windowID = windowID_;
    event.button.which = 0;
    event.button.button = buttonNumber(button);
    event.button.down = down;
    event.button.clicks = static_cast<Uint8>(clicks);
    event.button.x = cursorX_;
    event.button.y = cursorY_;
    events.push_back(event);
}

void MouseReplayer::appendMotion(std::vector<SDL_Event> &events, Uint64 timestamp, float x,
                                 float y, float xrel, float yrel) const
{
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.timestamp = timestamp;
    event.motion.windowID = windowID_;
    event.motion.which = 0;
    event.motion.state = buttonMask();
    event.motion.x = x;
    event.motion.y = y;
    event.motion.xrel = xrel;
    event.motion.yrel = yrel;
    events.push_back(event);
}

SDL_MouseButtonFlags MouseReplayer::buttonMask() const noexcept
{
    SDL_MouseButtonFlags mask = 0;
    for (const MouseButton button : heldButtons_) {
        mask |= SDL_BUTTON_MASK(buttonNumber(button));
    }
    return mask;
}

float MouseReplayer::clampX(float x) const noexcept
{
    return std::clamp(x, 0.0F, std::max(0.0F, metrics_.width - 1.0F));
}

float MouseReplayer::clampY(float y) const noexcept
{
    return std::clamp(y, 0.0F, std::max(0.0F, metrics_.height - 1.0F));
}

} // namespace sdl3mcp
