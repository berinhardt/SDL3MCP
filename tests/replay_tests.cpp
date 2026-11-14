#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "Replay.h"

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
                          std::vector<SDL_Event> &events)
{
    CHECK(replayer.apply(input, kTimestamp, events).hasValue());
    return events.size();
}

/* Applies one action, requires failure, and leaves the event vector untouched. */
void expectRejected(KeyReplayer &replayer, const KeyInput &input,
                    std::vector<SDL_Event> &events, std::string_view fragment)
{
    const std::size_t before = events.size();
    const auto applied = replayer.apply(input, kTimestamp, events);
    CHECK(!applied);
    CHECK(events.size() == before);
    CHECK(!applied.error().message.empty());
    CHECK(applied.error().message.find('\n') == std::string::npos);
    CHECK(applied.error().message.find(fragment) != std::string::npos);
}

TEST_CASE("down and up")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, keyInput("a", KeyAction::Down), events);
    CHECK(events.size() == 1);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectHeld(replayer, {SDL_SCANCODE_A});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);

    /* A second down repeats and must not duplicate the held entry. */
    expectApplied(replayer, keyInput("a", KeyAction::Down), events);
    CHECK(events.size() == 2);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDL_KMOD_NONE, true);
    expectHeld(replayer, {SDL_SCANCODE_A});

    expectApplied(replayer, keyInput("a", KeyAction::Up), events);
    CHECK(events.size() == 3);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});

    expectRejected(replayer, keyInput("a", KeyAction::Up), events, "key is not held");
    expectHeld(replayer, {});
}

TEST_CASE("press")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, pressInput("a"), events);
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});

    /* Press while a down action holds the key: the down repeats, the up releases. */
    expectApplied(replayer, keyInput("a", KeyAction::Down), events);
    events.clear();
    expectApplied(replayer, pressInput("a"), events);
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, SDL_KMOD_NONE, true);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
}

TEST_CASE("ctrl chord")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, pressInput("ctrl+s"), events);
    CHECK(events.size() == 4);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_S, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, SDL_SCANCODE_S, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_LCTRL, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);
}

TEST_CASE("shift ctrl chord")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, pressInput("shift+ctrl+s"), events);
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

TEST_CASE("alt and gui chords")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, pressInput("alt+f4"), events);
    CHECK(events.size() == 4);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LALT, SDL_KMOD_LALT, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F4, SDL_KMOD_LALT, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, SDL_SCANCODE_F4, SDL_KMOD_LALT, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_LALT, SDL_KMOD_NONE, false);

    events.clear();
    expectApplied(replayer, pressInput("gui+space"), events);
    CHECK(events.size() == 4);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LGUI, SDL_KMOD_LGUI, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, SDL_KMOD_LGUI, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE, SDL_KMOD_LGUI, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_LGUI, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
}

TEST_CASE("held modifier is reused")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, keyInput("left ctrl", KeyAction::Down), events);
    CHECK(events.size() == 1);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, false);

    events.clear();
    expectApplied(replayer, pressInput("ctrl+s"), events);
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
    const auto chord = sdl3mcp::resolveKey("!", map);
    CHECK(chord.hasValue());
    CHECK(chord->modifiers == SDL_KMOD_LSHIFT);
    if (!chord) {
        return std::nullopt;
    }
    return *chord;
}

TEST_CASE("character needing shift")
{
    const KeyMap map;
    const std::optional<KeyChord> bang = resolveBang();
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, pressInput("!"), events);
    CHECK(events.size() == 4);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_DOWN, bang->scancode, SDL_KMOD_LSHIFT, false);
    expectKeyEvent(events[2], SDL_EVENT_KEY_UP, bang->scancode, SDL_KMOD_LSHIFT, false);
    expectKeyEvent(events[3], SDL_EVENT_KEY_UP, SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
}

TEST_CASE("character with held shift")
{
    const KeyMap map;
    const std::optional<KeyChord> bang = resolveBang();
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, keyInput("left shift", KeyAction::Down), events);
    events.clear();
    expectApplied(replayer, pressInput("!"), events);

    /* Shift is already down, so the chord neither presses nor releases it. */
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, bang->scancode, SDL_KMOD_LSHIFT, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, bang->scancode, SDL_KMOD_LSHIFT, false);
    expectHeld(replayer, {SDL_SCANCODE_LSHIFT});
    CHECK(replayer.modifiers() == SDL_KMOD_LSHIFT);
}

TEST_CASE("modifier chord key and lone modifier")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectRejected(replayer, pressInput("ctrl+shift"), events,
                   "a chord key must not be a modifier");
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);

    /* A lone modifier is a key, not a broken chord, so it taps. */
    expectApplied(replayer, pressInput("ctrl"), events);
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_DOWN, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_LCTRL, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);
}

TEST_CASE("down rejects key needing modifiers")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    constexpr char message[] = "down/up require a key that needs no modifiers; use press";
    expectRejected(replayer, keyInput("!", KeyAction::Down), events, message);
    expectRejected(replayer, keyInput("!", KeyAction::Up), events, message);
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);
}

TEST_CASE("release all")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    /* Nothing held: nothing to emit. */
    replayer.releaseAllKeys(kTimestamp, events);
    CHECK(events.empty());

    expectApplied(replayer, keyInput("left ctrl", KeyAction::Down), events);
    expectApplied(replayer, keyInput("a", KeyAction::Down), events);
    CHECK(events.size() == 2);

    events.clear();
    replayer.releaseAllKeys(kTimestamp, events);
    CHECK(events.size() == 2);
    expectKeyEvent(events[0], SDL_EVENT_KEY_UP, SDL_SCANCODE_A, SDL_KMOD_LCTRL, false);
    expectKeyEvent(events[1], SDL_EVENT_KEY_UP, SDL_SCANCODE_LCTRL, SDL_KMOD_NONE, false);
    expectHeld(replayer, {});
    CHECK(replayer.modifiers() == SDL_KMOD_NONE);
}

TEST_CASE("reset")
{
    const KeyMap map;
    KeyReplayer replayer(map, kWindowID);
    std::vector<SDL_Event> events;

    expectApplied(replayer, keyInput("left ctrl", KeyAction::Down), events);
    expectApplied(replayer, keyInput("a", KeyAction::Down), events);
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
TEST_CASE("text input")
{
    const std::string text = "héllo ✓";
    const TextInput input{text};
    std::vector<SDL_Event> events;

    CHECK(sdl3mcp::buildTextEvent(input, kWindowID, true, kTimestamp, events)
              .hasValue());
    CHECK(events.size() == 1);

    const SDL_Event &event = events[0];
    CHECK(event.type == SDL_EVENT_TEXT_INPUT);
    CHECK(event.text.windowID == kWindowID);
    CHECK(event.text.timestamp == kTimestamp);
    /* The whole UTF-8 string, by pointer rather than a copy. */
    CHECK(event.text.text == input.text.c_str());
    CHECK(std::string(event.text.text) == text);
}

TEST_CASE("text input rejected")
{
    const TextInput text{"a"};
    const TextInput empty{""};
    std::vector<SDL_Event> events;

    const auto noWindow =
        sdl3mcp::buildTextEvent(text, 0, true, kTimestamp, events);
    CHECK(!noWindow);
    CHECK(events.empty());
    CHECK(noWindow.error().message == "no window bound");

    const auto inactive =
        sdl3mcp::buildTextEvent(text, kWindowID, false, kTimestamp, events);
    CHECK(!inactive);
    CHECK(events.empty());
    CHECK(inactive.error().message ==
          "text input is not active on the bound window");

    /* A bound, active window is the only way to reach the empty-text check. */
    const auto emptyText =
        sdl3mcp::buildTextEvent(empty, kWindowID, true, kTimestamp, events);
    CHECK(!emptyText);
    CHECK(events.empty());
    CHECK(emptyText.error().message == "text is empty");
}

/*
  A batch applies to a copy and commits it only on success, so a copy that walks
  a valid action and then fails must leave the original's state and events alone.
*/
TEST_CASE("copy rolls back")
{
    const KeyMap map;
    KeyReplayer original(map, kWindowID);
    std::vector<SDL_Event> originalEvents;

    expectApplied(original, keyInput("a", KeyAction::Down), originalEvents);
    CHECK(originalEvents.size() == 1);

    KeyReplayer copy = original;
    std::vector<SDL_Event> copyEvents = originalEvents;

    expectApplied(copy, keyInput("b", KeyAction::Down), copyEvents);
    CHECK(copyEvents.size() == 2);
    expectHeld(copy, {SDL_SCANCODE_A, SDL_SCANCODE_B});

    /* 'c' was never pressed, so the batch would fail here and be discarded. */
    expectRejected(copy, keyInput("c", KeyAction::Up), copyEvents, "key is not held");
    CHECK(copyEvents.size() == 2);

    expectHeld(original, {SDL_SCANCODE_A});
    CHECK(original.modifiers() == SDL_KMOD_NONE);
    CHECK(originalEvents.size() == 1);
}

} // namespace
