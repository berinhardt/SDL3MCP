#include "replay.hpp"

#include "test_util.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using sdl3mcp::KeyAction;
using sdl3mcp::KeyChord;
using sdl3mcp::KeyInput;
using sdl3mcp::KeyMap;
using sdl3mcp::KeyReplayer;
using sdl3mcp::TextInput;

constexpr SDL_WindowID kWindowID = 42;
constexpr Uint64 kTimestamp = 123456789;

/*
  Every field is asserted from the event itself. The expected keycode is derived
  from the scancode with key_event=true, so the tests hold on any layout.
*/
void expectKeyEvent(const SDL_Event &event, SDL_EventType type, SDL_Scancode scancode,
                    SDL_Keymod modifiers, bool repeat)
{
    CHECK(event.type == type);
    CHECK(event.key.down == (type == SDL_EVENT_KEY_DOWN));
    CHECK(event.key.scancode == scancode);
    CHECK(event.key.key == SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true));
    CHECK(event.key.mod == modifiers);
    CHECK(event.key.repeat == repeat);
    CHECK(event.key.raw == 0);
    CHECK(event.key.which == 0);
    CHECK(event.key.windowID == kWindowID);
    CHECK(event.key.timestamp == kTimestamp);
}

void expectHeld(const KeyReplayer &replayer, std::initializer_list<SDL_Scancode> keys)
{
    CHECK(replayer.heldKeys().size() == keys.size());
    CHECK(std::equal(keys.begin(), keys.end(), replayer.heldKeys().begin()));
}

KeyInput keyInput(std::string key, KeyAction action)
{
    return KeyInput{std::move(key), action, {}};
}

KeyInput pressInput(std::string key)
{
    return KeyInput{std::move(key), KeyAction::Press, {}};
}

/* Applies one action and requires it to succeed, returning the new event count. */
std::size_t expectApplied(KeyReplayer &replayer, const KeyInput &input,
                          std::vector<SDL_Event> &events, std::string &error)
{
    CHECK(replayer.apply(input, kTimestamp, events, error));
    CHECK(error.empty());
    return events.size();
}

/* Applies one action, requires failure, and leaves the event vector untouched. */
void expectRejected(KeyReplayer &replayer, const KeyInput &input,
                    std::vector<SDL_Event> &events, std::string &error,
                    std::string_view fragment)
{
    const std::size_t before = events.size();
    CHECK(!replayer.apply(input, kTimestamp, events, error));
    CHECK(events.size() == before);
    CHECK(!error.empty());
    CHECK(error.find('\n') == std::string::npos);
    CHECK(error.find(fragment) != std::string::npos);
}

void testDownAndUp()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, keyInput("a", KeyAction::Down), events, error);
    CHECK(events.size() == 1);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectHeld(replayer, {SDL_SCANCODE_A});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);

    /* A second down repeats and must not duplicate the held entry. */
    expectApplied(replayer, keyInput("a", KeyAction::Down), events, error);
    CHECK(events.size() == 2);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDL_KMOD_NONE, true);
    expectHeld(replayer, {SDL_SCANCODE_A});

    expectApplied(replayer, keyInput("a", KeyAction::Up), events, error);
    CHECK(events.size() == 3);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});

    expectRejected(replayer, keyInput("a", KeyAction::Up), events, error, "key is not held");
    expectHeld(replayer, {});
}

void testPress()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, pressInput("a"), events, error);
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});

    /* Press while a down action holds the key: the down repeats, the up releases. */
    expectApplied(replayer, keyInput("a", KeyAction::Down), events, error);
    events.clear();
    expectApplied(replayer, pressInput("a"), events, error);
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDL_KMOD_NONE, true);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
}

void testCtrlChord()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, pressInput("ctrl+s"), events, error);
    CHECK(events.size() == 4);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_S, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, SDL_SCANCODE_S, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_LCTRL, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);
}

void testShiftCtrlChord()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, pressInput("shift+ctrl+s"), events, error);
    CHECK(events.size() == 6);

    const SDL_Keymod both = static_cast<SDL_Keymod>(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    /* Canonical order puts Ctrl first even though the chord names Shift first. */
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LSHIFT, both, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_S, both, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_S, both, false);
    /* Release is the reverse of the press order. */
    expectKeyEvent(events[4], SDL_EVENT_KEY_UP, SDL_SCANCODE_LSHIFT, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[5], SDL_EVENT_KEY_UP, SDL_SCANCODE_LCTRL, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
}

void testAltAndGuiChords()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, pressInput("alt+f4"), events, error);
    CHECK(events.size() == 4);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LALT, SDL_KMOD_LALT, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F4, SDL_KMOD_LALT, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, SDL_SCANCODE_F4, SDL_KMOD_LALT, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_LALT, SDL_KMOD_NONE, false);

    events.clear();
    expectApplied(replayer, pressInput("gui+space"), events, error);
    CHECK(events.size() == 4);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LGUI, SDL_KMOD_LGUI, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, SDL_KMOD_LGUI, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, SDL_KMOD_LGUI, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_LGUI, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
}

void testHeldModifierIsReused()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, keyInput("left ctrl", KeyAction::Down), events, error);
    CHECK(events.size() == 1);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, false);

    events.clear();
    expectApplied(replayer, pressInput("ctrl+s"), events, error);
    /* No extra Ctrl events: only the key pair, with the pre-existing mask. */
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_S, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_S, SDL_KMOD_LCTRL, false);
    expectHeld(replayer, {SDL_SCANCODE_LCTRL});
    CHECK(replayer.modifiers() == SDL_KMOD_LCTRL);
}

/* The layout decides which scancode produces '!'; SDL reports that it needs Shift. */
std::optional<KeyChord> resolveBang()
{
    const KeyMap map;
    std::string error;
    const std::optional<KeyChord> chord = sdl3mcp::resolveKey("!", map, error);
    CHECK(chord.has_value());
    CHECK(error.empty());
    CHECK(chord->modifiers == SDL_KMOD_LSHIFT);
    return chord;
}

void testCharacterNeedingShift()
{
    const KeyMap map;
    const std::optional<KeyChord> bang = resolveBang();
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, pressInput("!"), events, error);
    CHECK(events.size() == 4);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, bang->scancode, SDL_KMOD_LSHIFT, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, bang->scancode, SDL_KMOD_LSHIFT, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
}

void testCharacterWithHeldShift()
{
    const KeyMap map;
    const std::optional<KeyChord> bang = resolveBang();
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, keyInput("left shift", KeyAction::Down), events, error);
    events.clear();
    expectApplied(replayer, pressInput("!"), events, error);

    /* Shift is already down, so the chord neither presses nor releases it. */
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, bang->scancode, SDL_KMOD_LSHIFT, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, bang->scancode, SDL_KMOD_LSHIFT, false);
    expectHeld(replayer, {SDL_SCANCODE_LSHIFT});
    CHECK(replayer.modifiers() == SDL_KMOD_LSHIFT);
}

void testModifierChordKeyAndLoneModifier()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectRejected(replayer, pressInput("ctrl+shift"), events, error,
                   "a chord key must not be a modifier");
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);

    /* A lone modifier is a key, not a broken chord, so it taps. */
    expectApplied(replayer, pressInput("ctrl"), events, error);
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_LCTRL, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);
}

void testDownRejectsKeyNeedingModifiers()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    constexpr char message[] = "down/up require a key that needs no modifiers; use press";
    expectRejected(replayer, keyInput("!", KeyAction::Down), events, error, message);
    expectRejected(replayer, keyInput("!", KeyAction::Up), events, error, message);
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);
}

void testReleaseAll()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    /* Nothing held: nothing to emit. */
    replayer.releaseAllKeys(kTimestamp, events);
    CHECK(events.empty());

    expectApplied(replayer, keyInput("left ctrl", KeyAction::Down), events, error);
    expectApplied(replayer, keyInput("a", KeyAction::Down), events, error);
    CHECK(events.size() == 2);

    events.clear();
    replayer.releaseAllKeys(kTimestamp, events);
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_LCTRL, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);
}

void testReset()
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;
    std::string error;

    expectApplied(replayer, keyInput("left ctrl", KeyAction::Down), events, error);
    expectApplied(replayer, keyInput("a", KeyAction::Down), events, error);
    CHECK(replayer.modifiers() == SDL_KMOD_LCTRL);

    events.clear();
    replayer.reset();
    CHECK(events.empty());
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);

    /* With nothing tracked, release is a no-op rather than a stale pair of ups. */
    replayer.releaseAllKeys(kTimestamp, events);
    CHECK(events.empty());
}

/*
  Text replay borrows the string: the event stores input.text.c_str(), so the
  test keeps the TextInput alive while it inspects the event.
*/
void testTextInput()
{
    const std::string text = "héllo ✓";
    const TextInput input{text};
    std::vector<SDL_Event> events;
    std::string error;

    CHECK(sdl3mcp::buildTextEvent(input, kWindowID, true, kTimestamp, events, error));
    CHECK(error.empty());
    CHECK(events.size() == 1);

    const SDL_Event &event = events[0];
    CHECK(event.type == SDL_EVENT_TEXT_INPUT);
    CHECK(event.text.windowID == kWindowID);
    CHECK(event.text.timestamp == kTimestamp);
    /* The whole UTF-8 string, by pointer rather than a copy. */
    CHECK(event.text.text == input.text.c_str());
    CHECK(std::string(event.text.text) == text);
}

void testTextInputRejected()
{
    const TextInput text{"a"};
    const TextInput empty{""};
    std::vector<SDL_Event> events;
    std::string error;

    CHECK(!sdl3mcp::buildTextEvent(text, 0, true, kTimestamp, events, error));
    CHECK(events.empty());
    CHECK(error == "no window bound");

    CHECK(!sdl3mcp::buildTextEvent(text, kWindowID, false, kTimestamp, events, error));
    CHECK(events.empty());
    CHECK(error == "text input is not active on the bound window");

    /* A bound, active window is the only way to reach the empty-text check. */
    CHECK(!sdl3mcp::buildTextEvent(empty, kWindowID, true, kTimestamp, events, error));
    CHECK(events.empty());
    CHECK(error == "text is empty");
}

/*
  A batch applies to a copy and commits it only on success, so a copy that walks
  a valid action and then fails must leave the original's state and events alone.
*/
void testCopyRollsBack()
{
    const KeyMap map;
    KeyReplayer original(map, kWindowID);
    std::vector<SDL_Event> originalEvents;
    std::string error;

    expectApplied(original, keyInput("a", KeyAction::Down), originalEvents, error);
    CHECK(originalEvents.size() == 1);

    KeyReplayer copy = original;
    std::vector<SDL_Event> copyEvents = originalEvents;

    expectApplied(copy, keyInput("b", KeyAction::Down), copyEvents, error);
    CHECK(copyEvents.size() == 2);
    expectHeld(copy, {SDL_SCANCODE_A, SDL_SCANCODE_B});

    /* 'c' was never pressed, so the batch would fail here and be discarded. */
    expectRejected(copy, keyInput("c", KeyAction::Up), copyEvents, error, "key is not held");
    CHECK(copyEvents.size() == 2);

    expectHeld(original, {SDL_SCANCODE_A});
    CHECK(original.modifiers() == SDL_KMOD_NONE);
    CHECK(originalEvents.size() == 1);
}

} // namespace

int main()
{
    testDownAndUp();
    testPress();
    testCtrlChord();
    testShiftCtrlChord();
    testAltAndGuiChords();
    testHeldModifierIsReused();
    testCharacterNeedingShift();
    testCharacterWithHeldShift();
    testModifierChordKeyAndLoneModifier();
    testDownRejectsKeyNeedingModifiers();
    testReleaseAll();
    testReset();
    testTextInput();
    testTextInputRejected();
    testCopyRollsBack();
    return 0;
}
