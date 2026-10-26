/*
  Chord grammar and modifier mapping for the input model. A key action names its
  key twice over: `down`/`up` take exactly one key, while `press` also accepts a
  `modifier+modifier+key` chord. Both forms end up as a KeyChord with a complete
  left-side mask, so replay can press precisely what the chord asks for.

  Both resolvers call KeyMap::find, which uses SDL_GetScancodeFromKey and is
  documented as not thread safe, so they are app-thread only.
*/

#ifndef SDL3MCP_INPUT_HPP
#define SDL3MCP_INPUT_HPP

#include "keymap.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sdl3mcp
{

enum class Modifier
{
    Ctrl,
    Shift,
    Alt,
    Gui
};

/*
  Exact "ctrl", "shift", "alt" or "gui", matched case-insensitively. Surrounding
  whitespace is not trimmed: chord segments are trimmed before this is called,
  and a raw request carrying spaces is a malformed modifier.
*/
std::optional<Modifier> parseModifier(std::string_view name);

/* The physical left-side bit a modifier is held with: LCTRL/LSHIFT/LALT/LGUI. */
SDL_Keymod modifierBit(Modifier modifier);

/* One key plus the complete left-side modifier mask to hold around it. */
struct KeyChord
{
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    SDL_Keymod modifiers = SDL_KMOD_NONE;
};

/*
  Resolves the key operand of a down/up action: exactly one key, never a chord.
  Names may contain '+', so "keypad +" still resolves, but "ctrl+s" is an error
  because a chord only makes sense for press. On failure returns nullopt and
  writes one line of text to `error`. App thread only.
*/
std::optional<KeyChord> resolveKey(std::string_view spec,
                                   const KeyMap &keymap,
                                   std::string &error);

/*
  Resolves the key operand of a press action: a single key or a
  "modifier+modifier+key" chord, merged with the action's explicit `modifiers`.
  The whole spec is tried as a key name first, so "keypad +" is a single key
  rather than a malformed chord. On failure returns nullopt and writes one line
  of text to `error`. App thread only.
*/
std::optional<KeyChord> resolveKeyChord(std::string_view spec,
                                        std::span<const Modifier> explicitModifiers,
                                        const KeyMap &keymap,
                                        std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_INPUT_HPP */
