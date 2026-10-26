#include "replay.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace sdl3mcp
{
namespace
{

struct ChordModifier
{
    SDL_Scancode scancode;
    SDL_Keymod bit;
};

/* Canonical press order; every entry is the physical left-side key. */
constexpr ChordModifier kChordModifiers[] = {
    {SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL},
    {SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT},
    {SDL_SCANCODE_LALT, SDL_KMOD_LALT},
    {SDL_SCANCODE_LGUI, SDL_KMOD_LGUI},
};

constexpr char kModifierKeyError[] = "down/up require a key that needs no modifiers; use press";
constexpr char kNotHeldError[] = "key is not held";
constexpr char kChordModifierKeyError[] = "a chord key must not be a modifier";

/*
  A modifier key contributes its own bit, so the right-side keys are as
  trackable as the left ones even though chords never press them.
*/
SDL_Keymod modifierBitForScancode(SDL_Scancode scancode) noexcept
{
    switch (scancode) {
    case SDL_SCANCODE_LCTRL:
        return SDL_KMOD_LCTRL;
    case SDL_SCANCODE_RCTRL:
        return SDL_KMOD_RCTRL;
    case SDL_SCANCODE_LSHIFT:
        return SDL_KMOD_LSHIFT;
    case SDL_SCANCODE_RSHIFT:
        return SDL_KMOD_RSHIFT;
    case SDL_SCANCODE_LALT:
        return SDL_KMOD_LALT;
    case SDL_SCANCODE_RALT:
        return SDL_KMOD_RALT;
    case SDL_SCANCODE_LGUI:
        return SDL_KMOD_LGUI;
    case SDL_SCANCODE_RGUI:
        return SDL_KMOD_RGUI;
    default:
        return SDL_KMOD_NONE;
    }
}

} // namespace

KeyReplayer::KeyReplayer(const KeyMap &keymap, SDL_WindowID windowID)
    : keymap_(&keymap), windowID_(windowID)
{
}

bool KeyReplayer::apply(const KeyInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
                        std::string &error)
{
    error.clear();
    switch (input.action) {
    case KeyAction::Down:
        return applyDown(input, timestamp, events, error);
    case KeyAction::Up:
        return applyUp(input, timestamp, events, error);
    case KeyAction::Press:
        return applyPress(input, timestamp, events, error);
    }
    error = "unknown key action";
    return false;
}

bool KeyReplayer::applyDown(const KeyInput &input, Uint64 timestamp,
                            std::vector<SDL_Event> &events, std::string &error)
{
    const std::optional<KeyChord> chord = resolveKey(input.key, *keymap_, error);
    if (!chord.has_value()) {
        return false;
    }
    /* A key that only exists with Shift, such as '!', has to be pressed as a chord. */
    if (chord->modifiers != SDL_KMOD_NONE) {
        error = kModifierKeyError;
        return false;
    }

    pressKey(chord->scancode, timestamp, isHeld(chord->scancode), events);
    return true;
}

bool KeyReplayer::applyUp(const KeyInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
                          std::string &error)
{
    const std::optional<KeyChord> chord = resolveKey(input.key, *keymap_, error);
    if (!chord.has_value()) {
        return false;
    }
    if (chord->modifiers != SDL_KMOD_NONE) {
        error = kModifierKeyError;
        return false;
    }
    if (!isHeld(chord->scancode)) {
        error = kNotHeldError;
        return false;
    }

    releaseKey(chord->scancode, timestamp, events);
    return true;
}

bool KeyReplayer::applyPress(const KeyInput &input, Uint64 timestamp,
                             std::vector<SDL_Event> &events, std::string &error)
{
    const std::optional<KeyChord> chord =
        resolveKeyChord(input.key, input.modifiers, *keymap_, error);
    if (!chord.has_value()) {
        return false;
    }
    /*
      "ctrl+shift" resolves to a Shift key with a Ctrl mask; that is an ambiguous
      request, so it is refused rather than turned into an accidental chord.
    */
    if (chord->modifiers != SDL_KMOD_NONE
        && modifierBitForScancode(chord->scancode) != SDL_KMOD_NONE) {
        error = kChordModifierKeyError;
        return false;
    }

    /* Only what the chord adds is pressed and later released; held keys stay untouched. */
    const SDL_Keymod missing = static_cast<SDL_Keymod>(chord->modifiers & ~modifiers_);
    std::vector<SDL_Scancode> pressedModifiers;
    for (const ChordModifier &modifier : kChordModifiers) {
        if ((missing & modifier.bit) == 0) {
            continue;
        }
        pressKey(modifier.scancode, timestamp, false, events);
        pressedModifiers.push_back(modifier.scancode);
    }

    pressKey(chord->scancode, timestamp, isHeld(chord->scancode), events);
    releaseKey(chord->scancode, timestamp, events);

    for (auto it = pressedModifiers.rbegin(); it != pressedModifiers.rend(); ++it) {
        releaseKey(*it, timestamp, events);
    }
    return true;
}

void KeyReplayer::releaseAllKeys(Uint64 timestamp, std::vector<SDL_Event> &events)
{
    /* Popping the tail releases in reverse press order without invalidating iterators. */
    while (!heldKeys_.empty()) {
        releaseKey(heldKeys_.back(), timestamp, events);
    }
}

void KeyReplayer::reset() noexcept
{
    heldKeys_.clear();
    modifiers_ = SDL_KMOD_NONE;
}

bool KeyReplayer::isHeld(SDL_Scancode scancode) const noexcept
{
    return std::find(heldKeys_.begin(), heldKeys_.end(), scancode) != heldKeys_.end();
}

void KeyReplayer::pressKey(SDL_Scancode scancode, Uint64 timestamp, bool repeat,
                           std::vector<SDL_Event> &events)
{
    const SDL_Keymod previousModifiers = modifiers_;
    if (!isHeld(scancode)) {
        heldKeys_.push_back(scancode);
        modifiers_ = static_cast<SDL_Keymod>(modifiers_ | modifierBitForScancode(scancode));
    }
    appendKeyEvent(events, scancode, true, repeat, timestamp, previousModifiers);
}

void KeyReplayer::releaseKey(SDL_Scancode scancode, Uint64 timestamp,
                             std::vector<SDL_Event> &events)
{
    const SDL_Keymod previousModifiers = modifiers_;
    const auto held = std::find(heldKeys_.begin(), heldKeys_.end(), scancode);
    heldKeys_.erase(held);
    modifiers_ = static_cast<SDL_Keymod>(modifiers_ & ~modifierBitForScancode(scancode));
    appendKeyEvent(events, scancode, false, false, timestamp, previousModifiers);
}

void KeyReplayer::appendKeyEvent(std::vector<SDL_Event> &events, SDL_Scancode scancode, bool down,
                                 bool repeat, Uint64 timestamp,
                                 SDL_Keymod previousModifiers) const
{
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = timestamp;
    event.key.windowID = windowID_;
    event.key.which = 0;
    event.key.scancode = scancode;
    /*
      key_event=true keeps the base keycode ("1" rather than "!"), matching how
      SDL 3.4 reports key events; the live mask travels in mod instead.
    */
    event.key.key = SDL_GetKeyFromScancode(scancode, previousModifiers, true);
    event.key.mod = modifiers_;
    event.key.raw = 0;
    event.key.down = down;
    event.key.repeat = repeat;
    events.push_back(event);
}

} // namespace sdl3mcp
