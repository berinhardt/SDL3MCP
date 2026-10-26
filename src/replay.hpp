/*
  Keyboard replay: turns a key action into SDL key events and tracks which keys
  the library holds.

  SDL 3.4 fills a key event's `key` with the unmodified base keycode (the
  key_event=true form of SDL_GetKeyFromScancode) and keeps the live modifiers in
  `mod`. A chord therefore presses only the left-side modifiers that are not
  already down, in Ctrl/Shift/Alt/Gui order, and releases exactly those in
  reverse, so the same batch always produces the same events.

  apply() resolves and validates before it touches state or the event vector, so
  a failed action changes neither. A batch applies its actions to a copy of the
  replayer and commits that copy only once every action succeeded.

  The constructor takes the KeyMap by reference and stores a pointer, so the map
  must outlive the replayer. Every method calls SDL keyboard functions and must
  run on the app thread.
*/

#ifndef SDL3MCP_REPLAY_HPP
#define SDL3MCP_REPLAY_HPP

#include "input.hpp"
#include "keymap.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace sdl3mcp
{

class KeyReplayer
{
public:
    KeyReplayer(const KeyMap &keymap, SDL_WindowID windowID);

    /* A value type: the batch caller copies it, applies actions, then commits. */
    KeyReplayer(const KeyReplayer &) = default;
    KeyReplayer &operator=(const KeyReplayer &) = default;

    /*
      Appends the events for one key action. Returns false with one line of text
      in `error` when the action cannot be applied; `error` is empty on success.
    */
    bool apply(const KeyInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
               std::string &error);

    /* Releases every held key in reverse press order, ending at no modifiers. */
    void releaseAllKeys(Uint64 timestamp, std::vector<SDL_Event> &events);

    /* Forgets held keys and modifiers without emitting events. */
    void reset() noexcept;

    void setWindowID(SDL_WindowID windowID) noexcept { windowID_ = windowID; }

    SDL_Keymod modifiers() const noexcept { return modifiers_; }
    const std::vector<SDL_Scancode> &heldKeys() const noexcept { return heldKeys_; }

private:
    bool applyDown(const KeyInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
                   std::string &error);
    bool applyUp(const KeyInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
                 std::string &error);
    bool applyPress(const KeyInput &input, Uint64 timestamp, std::vector<SDL_Event> &events,
                    std::string &error);

    bool isHeld(SDL_Scancode scancode) const noexcept;

    /* Adds the key to the held list at most once; `repeat` reports an existing entry. */
    void pressKey(SDL_Scancode scancode, Uint64 timestamp, bool repeat,
                  std::vector<SDL_Event> &events);

    /* Caller guarantees the key is held. */
    void releaseKey(SDL_Scancode scancode, Uint64 timestamp, std::vector<SDL_Event> &events);

    /* Uses the current mask as the event's mod and the previous one for the key. */
    void appendKeyEvent(std::vector<SDL_Event> &events, SDL_Scancode scancode, bool down,
                        bool repeat, Uint64 timestamp, SDL_Keymod previousModifiers) const;

    const KeyMap *keymap_;
    SDL_WindowID windowID_;
    std::vector<SDL_Scancode> heldKeys_;
    SDL_Keymod modifiers_ = SDL_KMOD_NONE;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_REPLAY_HPP */
