/*
  The input action model and its structural validation, plus chord grammar and
  modifier mapping for key actions.

  One InputAction describes one synthesis step; a batch is applied atomically.
  validateStructure checks only the shape of a batch (counts, byte limits, enum
  values, coordinate pairing, finite numbers) and is therefore pure: it calls no
  SDL function and no KeyMap, so the server thread can reject a malformed batch
  immediately. Resolving key names stays on the app thread.

  A key action names its key twice over: `down`/`up` take exactly one key, while
  `press` also accepts a `modifier+modifier+key` chord. Both forms end up as a
  KeyChord with a complete left-side mask, so replay can press precisely what the
  chord asks for. Both resolvers call KeyMap::find, which uses
  SDL_GetScancodeFromKey and is documented as not thread safe, so they are
  app-thread only.
*/

#ifndef SDL3MCP_INPUT_HPP
#define SDL3MCP_INPUT_HPP

#include "keymap.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sdl3mcp
{

enum class Modifier
{
    Ctrl,
    Shift,
    Alt,
    Gui
};

enum class KeyAction
{
    Down,
    Up,
    Press
};

enum class MouseButton
{
    Left,
    Middle,
    Right,
    X1,
    X2
};

enum class MouseButtonAction
{
    Down,
    Up,
    Click
};

/* One key action: the name is resolved on the app thread by the chord helpers. */
struct KeyInput
{
    std::string key;
    KeyAction action = KeyAction::Press;
    std::vector<Modifier> modifiers;
};

struct TextInput
{
    std::string text;
};

/*
  x/y hold window points for Absolute and deltas for Relative, matching the two
  wire forms (x/y or dx/dy) with a single coordinate pair.
*/
struct MouseMoveInput
{
    enum class Mode
    {
        Absolute,
        Relative
    };

    Mode mode = Mode::Absolute;
    float x = 0.0F;
    float y = 0.0F;
};

struct MouseButtonInput
{
    MouseButton button = MouseButton::Left;
    MouseButtonAction action = MouseButtonAction::Click;
    std::optional<float> x;
    std::optional<float> y;
    int clicks = 1;
};

/* Wheel units, 120 per notch. */
struct ScrollInput
{
    float x = 0.0F;
    float y = 0.0F;
};

/* Releases every key and button the library holds. */
struct ReleaseAllInput
{
};

using InputAction =
    std::variant<KeyInput, TextInput, MouseMoveInput, MouseButtonInput, ScrollInput,
                 ReleaseAllInput>;

struct InputBatch
{
    std::vector<InputAction> actions;
};

constexpr std::size_t kMaxActionsPerBatch = 256;
constexpr std::size_t kMaxKeyBytes = 64;
constexpr std::size_t kMaxTextBytes = 1024;
constexpr std::size_t kMaxModifiersPerKey = 4;
constexpr int kMinClicks = 1;
constexpr int kMaxClicks = 3;

/*
  Server-thread safe check of a batch's shape: action count, key/text byte
  lengths, modifier count and placement, enum values, x/y pairing, finite
  coordinates, and the clicks range. Key names and chord syntax are deliberately
  left to resolveKey/resolveKeyChord, which run on the app thread.

  Returns nullopt when the batch is valid, otherwise one line naming the 0-based
  action index, for example "actions[3]: clicks must be between 1 and 3". The
  count rules have no index because they describe the batch itself.
*/
std::optional<std::string> validateStructure(const InputBatch &batch);

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
