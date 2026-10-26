/*
  Internal key-name resolver used when an input batch is applied. Names are
  normalized to a lowercase, space-separated canonical form, so "Left-CTRL",
  "left_ctrl" and "left ctrl" all agree, while SDL's literal key names ("-" and
  "Keypad -") keep their separator characters.

  The constructor calls SDL_GetScancodeName, which is documented as not thread
  safe, so a KeyMap must be built on the app thread. find() additionally calls
  SDL_GetScancodeFromKey for single-character fallbacks and is app-thread only.
  canonicalName() only reads constructor-built data.
*/

#ifndef SDL3MCP_KEYMAP_HPP
#define SDL3MCP_KEYMAP_HPP

#include <SDL3/SDL.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sdl3mcp
{

struct KeyBinding
{
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    SDL_Keymod modifiers = SDL_KMOD_NONE;
};

class KeyMap
{
public:
    KeyMap();

    /*
      Resolves a scancode name, an alias, or a single character. Named keys win
      over the character lookup, so "a" and "-" keep their physical scancode
      and carry no modifiers. Chords and multi-codepoint input are not this
      layer's job and return nullopt. App thread only.
    */
    std::optional<KeyBinding> find(std::string_view name) const;

    /* Normalized exact SDL name, or empty for unnamed or out-of-range codes. */
    std::string_view canonicalName(SDL_Scancode scancode) const;

private:
    using CanonicalNames = std::array<std::string, SDL_SCANCODE_COUNT>;
    using NameTable = std::unordered_map<std::string, KeyBinding>;

    static CanonicalNames buildCanonicalNames();
    static NameTable buildNames(const CanonicalNames &canonicalNames);

    CanonicalNames canonicalNames_;
    const NameTable names_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_KEYMAP_HPP */
