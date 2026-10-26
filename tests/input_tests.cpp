#include "input.hpp"

#include "test_util.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{

using sdl3mcp::InputAction;
using sdl3mcp::InputBatch;
using sdl3mcp::KeyAction;
using sdl3mcp::KeyChord;
using sdl3mcp::KeyInput;
using sdl3mcp::KeyMap;
using sdl3mcp::Modifier;
using sdl3mcp::MouseButton;
using sdl3mcp::MouseButtonAction;
using sdl3mcp::MouseButtonInput;
using sdl3mcp::MouseMoveInput;
using sdl3mcp::ReleaseAllInput;
using sdl3mcp::ScrollInput;
using sdl3mcp::TextInput;

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

/*
  Structural validation is pure: no SDL init, no window, no KeyMap. Every case
  below is therefore a plain call on a hand-built batch.
*/

InputBatch batchWith(const InputAction &action)
{
    return InputBatch{{action}};
}

void expectValidBatch(const InputBatch &batch)
{
    const std::optional<std::string> error = sdl3mcp::validateStructure(batch);
    CHECK(!error.has_value());
}

void expectBatchError(const InputBatch &batch, std::string_view fragment)
{
    const std::optional<std::string> error = sdl3mcp::validateStructure(batch);
    CHECK(error.has_value());
    CHECK(!error->empty());
    CHECK(error->find('\n') == std::string::npos);
    CHECK(error->find(fragment) != std::string::npos);
}

void expectIndexedError(const InputBatch &batch, std::size_t index, std::string_view fragment)
{
    const std::optional<std::string> error = sdl3mcp::validateStructure(batch);
    CHECK(error.has_value());
    const std::string prefix = "actions[" + std::to_string(index) + "]: ";
    CHECK(error->rfind(prefix, 0) == 0);
    CHECK(error->find(fragment) != std::string::npos);
}

void testValidBatch()
{
    InputBatch batch;
    batch.actions = {
        KeyInput{"s", KeyAction::Press, {Modifier::Ctrl}},
        TextInput{"hello"},
        MouseMoveInput{MouseMoveInput::Mode::Absolute, 10.0F, 20.0F},
        MouseButtonInput{MouseButton::Left, MouseButtonAction::Click, 1.0F, 2.0F, 1},
        ScrollInput{0.0F, 120.0F},
        ReleaseAllInput{},
    };
    expectValidBatch(batch);
}

void testActionCountLimits()
{
    const InputBatch empty;
    expectBatchError(empty, "at least one action");
    /* The batch rules describe the array, so they carry no action index. */
    CHECK(sdl3mcp::validateStructure(empty)->rfind("actions[", 0) != 0);

    InputBatch maxBatch;
    maxBatch.actions.assign(sdl3mcp::kMaxActionsPerBatch, InputAction{ReleaseAllInput{}});
    CHECK(maxBatch.actions.size() == 256);
    expectValidBatch(maxBatch);

    maxBatch.actions.push_back(InputAction{ReleaseAllInput{}});
    expectBatchError(maxBatch, "maximum is 256");
    CHECK(sdl3mcp::validateStructure(maxBatch)->rfind("actions[", 0) != 0);
}

void testKeyValidation()
{
    expectBatchError(batchWith(KeyInput{"", KeyAction::Press, {}}), "empty");
    expectBatchError(batchWith(KeyInput{std::string(65, 'k'), KeyAction::Press, {}}),
                     "maximum is 64");
    expectValidBatch(batchWith(KeyInput{std::string(64, 'k'), KeyAction::Press, {}}));

    const std::vector<Modifier> fourModifiers = {Modifier::Ctrl, Modifier::Shift, Modifier::Alt,
                                                 Modifier::Gui};
    const std::vector<Modifier> fiveModifiers = {Modifier::Ctrl, Modifier::Shift, Modifier::Alt,
                                                 Modifier::Gui, Modifier::Ctrl};
    expectValidBatch(batchWith(KeyInput{"s", KeyAction::Press, fourModifiers}));
    expectBatchError(batchWith(KeyInput{"s", KeyAction::Press, fiveModifiers}),
                     "maximum is 4");

    expectBatchError(batchWith(KeyInput{"s", KeyAction::Down, {Modifier::Ctrl}}),
                     "modifiers are only valid for press");
    expectBatchError(batchWith(KeyInput{"s", KeyAction::Up, {Modifier::Ctrl}}),
                     "modifiers are only valid for press");
    expectValidBatch(batchWith(KeyInput{"s", KeyAction::Down, {}}));
    expectValidBatch(batchWith(KeyInput{"s", KeyAction::Up, {}}));

    expectBatchError(batchWith(KeyInput{"s", static_cast<KeyAction>(3), {}}),
                     "unknown key action");

    /* Chord syntax and unknown names belong to the app thread, not here. */
    expectValidBatch(batchWith(KeyInput{"ctrl+s", KeyAction::Press, {}}));
    expectValidBatch(batchWith(KeyInput{"notakey", KeyAction::Press, {}}));
}

void testTextValidation()
{
    expectBatchError(batchWith(TextInput{""}), "empty");
    expectBatchError(batchWith(TextInput{std::string(1025, 'a')}), "maximum is 1024");
    expectValidBatch(batchWith(TextInput{std::string(1024, 'a')}));

    /* The limit is bytes: 512 two-byte characters fill it exactly. */
    std::string twoByteCharacters;
    for (int index = 0; index < 512; ++index) {
        twoByteCharacters += encodeUtf8(0xE9);
    }
    CHECK(twoByteCharacters.size() == 1024);
    expectValidBatch(batchWith(TextInput{twoByteCharacters}));
    expectBatchError(batchWith(TextInput{twoByteCharacters + encodeUtf8(0xE9)}),
                     "maximum is 1024");
}

void testMouseMoveValidation()
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float infinity = std::numeric_limits<float>::infinity();

    expectValidBatch(batchWith(MouseMoveInput{}));
    expectValidBatch(batchWith(MouseMoveInput{MouseMoveInput::Mode::Absolute, 640.0F, 480.0F}));
    expectValidBatch(batchWith(MouseMoveInput{MouseMoveInput::Mode::Relative, -3.0F, 4.0F}));

    expectBatchError(batchWith(MouseMoveInput{MouseMoveInput::Mode::Absolute, nan, 0.0F}),
                     "mouse move x must be finite");
    expectBatchError(batchWith(MouseMoveInput{MouseMoveInput::Mode::Relative, 0.0F, infinity}),
                     "mouse move y must be finite");
    expectBatchError(batchWith(MouseMoveInput{static_cast<MouseMoveInput::Mode>(2), 0.0F, 0.0F}),
                     "unknown mouse move mode");
}

void testMouseButtonValidation()
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();

    expectValidBatch(batchWith(MouseButtonInput{MouseButton::Left, MouseButtonAction::Click,
                                                std::nullopt, std::nullopt, 1}));
    expectValidBatch(
        batchWith(MouseButtonInput{MouseButton::X2, MouseButtonAction::Down, 10.0F, 20.0F, 1}));

    expectBatchError(batchWith(
                         MouseButtonInput{MouseButton::Left, MouseButtonAction::Click, 10.0F,
                                          std::nullopt, 1}),
                     "x and y must be provided together");
    expectBatchError(batchWith(
                         MouseButtonInput{MouseButton::Left, MouseButtonAction::Click, std::nullopt,
                                          20.0F, 1}),
                     "x and y must be provided together");
    expectBatchError(batchWith(MouseButtonInput{MouseButton::Left, MouseButtonAction::Click, nan,
                                                20.0F, 1}),
                     "mouse button x must be finite");
    expectBatchError(batchWith(MouseButtonInput{MouseButton::Left, MouseButtonAction::Up, 10.0F,
                                                nan, 1}),
                     "mouse button y must be finite");

    for (const int clicks : {0, 4}) {
        expectBatchError(batchWith(MouseButtonInput{MouseButton::Left, MouseButtonAction::Click,
                                                    std::nullopt, std::nullopt, clicks}),
                         "clicks must be between 1 and 3");
    }
    for (const int clicks : {1, 3}) {
        expectValidBatch(batchWith(MouseButtonInput{MouseButton::Left, MouseButtonAction::Click,
                                                    std::nullopt, std::nullopt, clicks}));
    }

    expectBatchError(batchWith(MouseButtonInput{static_cast<MouseButton>(5),
                                                MouseButtonAction::Click, std::nullopt,
                                                std::nullopt, 1}),
                     "unknown mouse button");
    expectBatchError(batchWith(MouseButtonInput{MouseButton::Left,
                                                static_cast<MouseButtonAction>(3), std::nullopt,
                                                std::nullopt, 1}),
                     "unknown mouse button action");
}

void testScrollValidation()
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float infinity = std::numeric_limits<float>::infinity();

    expectValidBatch(batchWith(ScrollInput{0.0F, 120.0F}));
    expectValidBatch(batchWith(ScrollInput{-1.5F, -120.0F}));
    expectBatchError(batchWith(ScrollInput{nan, 0.0F}), "scroll x must be finite");
    expectBatchError(batchWith(ScrollInput{0.0F, infinity}), "scroll y must be finite");
}

void testReleaseAllValidation() { expectValidBatch(batchWith(ReleaseAllInput{})); }

/* Each action type reports its own index, using the 0-based array position. */
void testActionIndexes()
{
    InputBatch batch;
    batch.actions.assign(5, InputAction{ReleaseAllInput{}});

    batch.actions[2] = KeyInput{"", KeyAction::Press, {}};
    expectIndexedError(batch, 2, "key must not be empty");

    batch.actions[2] = TextInput{""};
    expectIndexedError(batch, 2, "text must not be empty");

    batch.actions[2] = MouseMoveInput{MouseMoveInput::Mode::Absolute,
                                      std::numeric_limits<float>::quiet_NaN(), 0.0F};
    expectIndexedError(batch, 2, "mouse move x must be finite");

    batch.actions[2] = MouseButtonInput{MouseButton::Left, MouseButtonAction::Click, 1.0F,
                                        std::nullopt, 1};
    expectIndexedError(batch, 2, "x and y must be provided together");

    batch.actions[2] = ScrollInput{0.0F, std::numeric_limits<float>::quiet_NaN()};
    expectIndexedError(batch, 2, "scroll y must be finite");

    for (const std::size_t index : {std::size_t{0}, std::size_t{1}, std::size_t{4}}) {
        InputBatch indexed;
        indexed.actions.assign(6, InputAction{ReleaseAllInput{}});
        indexed.actions[index] = TextInput{""};
        expectIndexedError(indexed, index, "text must not be empty");
    }

    /* The first offending action wins. */
    InputBatch twoBad;
    twoBad.actions = {TextInput{""}, KeyInput{"", KeyAction::Press, {}}};
    expectIndexedError(twoBad, 0, "text must not be empty");
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
    testValidBatch();
    testActionCountLimits();
    testKeyValidation();
    testTextValidation();
    testMouseMoveValidation();
    testMouseButtonValidation();
    testScrollValidation();
    testReleaseAllValidation();
    testActionIndexes();
    return 0;
}
