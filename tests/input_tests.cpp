#include "input.hpp"

#include "test_util.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace
{

using sdl3mcp::KeyChord;
using sdl3mcp::KeyMap;
using sdl3mcp::Modifier;

constexpr SDL_Keymod kLeftSideMask =
    SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT | SDL_KMOD_LALT | SDL_KMOD_LGUI;

void expectModifier(std::string_view name, Modifier expected)
{
    const std::optional<Modifier> modifier = sdl3mcp::parseModifier(name);
    CHECK(modifier.has_value());
    CHECK(*modifier == expected);
}

void expectNotModifier(std::string_view name)
{
    CHECK(!sdl3mcp::parseModifier(name).has_value());
}

void expectKey(const KeyMap &map,
               std::string_view spec,
               SDL_Scancode scancode,
               SDL_Keymod modifiers = SDL_KMOD_NONE)
{
    std::string error;
    const std::optional<KeyChord> chord = sdl3mcp::resolveKey(spec, map, error);
    CHECK(chord.has_value());
    CHECK(error.empty());
    CHECK(chord->scancode == scancode);
    CHECK(chord->modifiers == modifiers);
}

void expectKeyError(const KeyMap &map, std::string_view spec, std::string_view fragment)
{
    std::string error;
    CHECK(!sdl3mcp::resolveKey(spec, map, error).has_value());
    CHECK(!error.empty());
    CHECK(error.find('\n') == std::string::npos);
    CHECK(error.find(fragment) != std::string::npos);
}

void expectChord(const KeyMap &map,
                 std::string_view spec,
                 std::span<const Modifier> explicitModifiers,
                 SDL_Scancode scancode,
                 SDL_Keymod modifiers = SDL_KMOD_NONE)
{
    std::string error;
    const std::optional<KeyChord> chord =
        sdl3mcp::resolveKeyChord(spec, explicitModifiers, map, error);
    CHECK(chord.has_value());
    CHECK(error.empty());
    CHECK(chord->scancode == scancode);
    CHECK(chord->modifiers == modifiers);
}

void expectChordError(const KeyMap &map,
                      std::string_view spec,
                      std::span<const Modifier> explicitModifiers,
                      std::string_view fragment)
{
    std::string error;
    CHECK(!sdl3mcp::resolveKeyChord(spec, explicitModifiers, map, error).has_value());
    CHECK(!error.empty());
    CHECK(error.find('\n') == std::string::npos);
    CHECK(error.find(fragment) != std::string::npos);
}

void testParseModifier()
{
    expectModifier("ctrl", Modifier::Ctrl);
    expectModifier("CTRL", Modifier::Ctrl);
    expectModifier("Ctrl", Modifier::Ctrl);
    expectModifier("shift", Modifier::Shift);
    expectModifier("alt", Modifier::Alt);
    expectModifier("gui", Modifier::Gui);

    expectNotModifier("control");
    expectNotModifier("cmd");
    expectNotModifier("win");
    expectNotModifier("");
    expectNotModifier(" ctrl");
    expectNotModifier("ctrl ");
    expectNotModifier("ctrl shift");
    expectNotModifier("hyper");
}

void testModifierBit()
{
    CHECK(sdl3mcp::modifierBit(Modifier::Ctrl) == SDL_KMOD_LCTRL);
    CHECK(sdl3mcp::modifierBit(Modifier::Shift) == SDL_KMOD_LSHIFT);
    CHECK(sdl3mcp::modifierBit(Modifier::Alt) == SDL_KMOD_LALT);
    CHECK(sdl3mcp::modifierBit(Modifier::Gui) == SDL_KMOD_LGUI);
}

void testResolveKey()
{
    const KeyMap map;
    expectKey(map, "s", SDL_SCANCODE_S);
    expectKey(map, "S", SDL_SCANCODE_S);
    expectKey(map, "a", SDL_SCANCODE_A);
    expectKey(map, "A", SDL_SCANCODE_A);
    expectKey(map, "left ctrl", SDL_SCANCODE_LCTRL);
    expectKey(map, "keypad +", SDL_SCANCODE_KP_PLUS);
    expectKey(map, " return ", SDL_SCANCODE_RETURN);
}

/*
  Symbols such as '!' are layout-dependent, so no US scancode is assumed: the
  binding only has to reproduce its character from the returned mask.
*/
void testResolveKeyCharacter()
{
    const KeyMap map;
    std::string error;
    const std::optional<KeyChord> bang = sdl3mcp::resolveKey("!", map, error);
    CHECK(bang.has_value());
    CHECK(error.empty());
    CHECK(SDL_GetKeyFromScancode(bang->scancode, bang->modifiers, false) == '!');
}

void testResolveKeyRejectsChords()
{
    const KeyMap map;
    expectKeyError(map, "ctrl+s", "only valid for press");
    expectKeyError(map, "", "empty");
    expectKeyError(map, "   ", "empty");
    expectKeyError(map, "notakey", "notakey");
}

void testResolveKeyChordKeys()
{
    const KeyMap map;
    expectChord(map, "s", {}, SDL_SCANCODE_S);
    expectChord(map, "ctrl+s", {}, SDL_SCANCODE_S, SDL_KMOD_LCTRL);
    expectChord(map, "shift+ctrl+s", {},
                SDL_SCANCODE_S, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    expectChord(map, "alt+f4", {}, SDL_SCANCODE_F4, SDL_KMOD_LALT);
    expectChord(map, "gui+space", {}, SDL_SCANCODE_SPACE, SDL_KMOD_LGUI);
    expectChord(map, "CTRL+S", {}, SDL_SCANCODE_S, SDL_KMOD_LCTRL);

    /* A lone modifier name is a key, not a broken chord. */
    expectChord(map, "ctrl", {}, SDL_SCANCODE_LCTRL);
    expectChord(map, "ctrl+shift", {}, SDL_SCANCODE_LSHIFT, SDL_KMOD_LCTRL);
}

void testResolveKeyChordKeypadPlus()
{
    const KeyMap map;
    expectChord(map, "keypad plus", {}, SDL_SCANCODE_KP_PLUS);
    expectChord(map, "ctrl+keypad plus", {}, SDL_SCANCODE_KP_PLUS, SDL_KMOD_LCTRL);
    /* A name that contains '+' must not be split. */
    expectChord(map, "keypad +", {}, SDL_SCANCODE_KP_PLUS);
}

void testExplicitModifiers()
{
    const KeyMap map;
    constexpr Modifier ctrlShift[] = {Modifier::Ctrl, Modifier::Shift};
    constexpr Modifier ctrl[] = {Modifier::Ctrl};
    constexpr Modifier ctrlDuplicate[] = {Modifier::Ctrl, Modifier::Ctrl};
    constexpr Modifier alt[] = {Modifier::Alt};

    expectChord(map, "s", ctrlShift, SDL_SCANCODE_S, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    expectChord(map, "ctrl+s", alt, SDL_SCANCODE_S, SDL_KMOD_LCTRL | SDL_KMOD_LALT);

    /* Repeats merge idempotently in the chord and in the explicit list. */
    expectChord(map, "ctrl+ctrl+s", ctrl, SDL_SCANCODE_S, SDL_KMOD_LCTRL);
    expectChord(map, "ctrl+s", ctrlDuplicate, SDL_SCANCODE_S, SDL_KMOD_LCTRL);
}

/*
  The binding for '!' carries whichever modifier the layout needs; the chord
  adds Ctrl on top of it and the mask must still produce '!'.
*/
void testCharacterChord()
{
    const KeyMap map;
    std::string error;
    const std::optional<KeyChord> single = sdl3mcp::resolveKey("!", map, error);
    CHECK(single.has_value());

    const std::optional<KeyChord> chord =
        sdl3mcp::resolveKeyChord("ctrl+!", {}, map, error);
    CHECK(chord.has_value());
    CHECK(error.empty());
    CHECK(chord->scancode == single->scancode);
    CHECK(chord->modifiers == (single->modifiers | SDL_KMOD_LCTRL));
    CHECK((chord->modifiers & SDL_KMOD_LCTRL) != 0);
    CHECK(SDL_GetKeyFromScancode(chord->scancode, chord->modifiers, false) == '!');
}

void testChordErrors()
{
    const KeyMap map;
    expectChordError(map, "", {}, "empty");
    expectChordError(map, "   ", {}, "empty");
    expectChordError(map, "ctrl+", {}, "empty segment");
    expectChordError(map, "+s", {}, "empty segment");
    expectChordError(map, "x+s", {}, "unknown modifier");
    expectChordError(map, "x+s", {}, "\"x\"");
    expectChordError(map, "ctrl+notakey", {}, "notakey");
    expectChordError(map, "a+b+c", {}, "modifier");
    expectChordError(map, "ctrl+shift+", {}, "empty segment");
}

/* Whatever the spec, the mask only ever holds the four left-side bits. */
void testOnlyLeftSideBits()
{
    const KeyMap map;
    constexpr std::string_view specs[] = {
        "s",          "ctrl+s",     "shift+ctrl+s", "alt+f4",  "gui+space",
        "ctrl+shift", "ctrl+!",     "keypad plus",  "ctrl+keypad plus",
    };
    for (const std::string_view spec : specs) {
        std::string error;
        const std::optional<KeyChord> chord = sdl3mcp::resolveKeyChord(spec, {}, map, error);
        CHECK(chord.has_value());
        CHECK((chord->modifiers & ~kLeftSideMask) == 0);
    }
}

std::string encodeUtf8(std::uint32_t codepoint)
{
    std::string text;
    if (codepoint < 0x80) {
        text.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return text;
}

/*
  A binding that needs AltGr or a lock key depends on the live layout, so it
  cannot be requested by name. Scan a slice of Unicode and, when a character
  does report such a bit, require the documented refusal; a layout that never
  produces one simply skips the case.
*/
void testUnsupportedModifierIfProducible()
{
    const KeyMap map;
    for (std::uint32_t codepoint = 0xA0; codepoint <= 0x2FF; ++codepoint) {
        const std::string character = encodeUtf8(codepoint);
        std::string error;
        if (sdl3mcp::resolveKey(character, map, error).has_value()) {
            continue;
        }
        if (error.find("unsupported modifier") == std::string::npos) {
            continue;
        }
        CHECK(error == "key requires an unsupported modifier");
        return;
    }
}

} // namespace

int main()
{
    testParseModifier();
    testModifierBit();
    testResolveKey();
    testResolveKeyCharacter();
    testResolveKeyRejectsChords();
    testResolveKeyChordKeys();
    testResolveKeyChordKeypadPlus();
    testExplicitModifiers();
    testCharacterChord();
    testChordErrors();
    testOnlyLeftSideBits();
    testUnsupportedModifierIfProducible();
    return 0;
}
