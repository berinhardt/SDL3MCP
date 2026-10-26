#include "keymap.hpp"

#include "test_util.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <string_view>

namespace
{

using sdl3mcp::KeyBinding;
using sdl3mcp::KeyMap;

void expectBinding(const KeyMap &map,
                   std::string_view name,
                   SDL_Scancode scancode,
                   SDL_Keymod modifiers = SDL_KMOD_NONE)
{
    const std::optional<KeyBinding> binding = map.find(name);
    CHECK(binding.has_value());
    CHECK(binding->scancode == scancode);
    CHECK(binding->modifiers == modifiers);
}

void expectMissing(const KeyMap &map, std::string_view name)
{
    CHECK(!map.find(name).has_value());
}

bool isLowercase(std::string_view text)
{
    for (const char character : text) {
        if (character >= 'A' && character <= 'Z') {
            return false;
        }
    }
    return true;
}

/* Case folding and word separators, including the '-'/'_' rule. */
void testNameVariants()
{
    const KeyMap map;
    constexpr std::string_view variants[] = {
        "left ctrl", "left-ctrl", "left_ctrl", "LEFT CTRL", "  Left---CTRL  ",
    };
    for (const std::string_view variant : variants) {
        expectBinding(map, variant, SDL_SCANCODE_LCTRL);
    }

    expectBinding(map, "a", SDL_SCANCODE_A);
    expectBinding(map, "A", SDL_SCANCODE_A);
    expectBinding(map, "LEFT SHIFT", SDL_SCANCODE_LSHIFT);
    expectBinding(map, "PageUp", SDL_SCANCODE_PAGEUP);
    CHECK(map.canonicalName(SDL_SCANCODE_LCTRL) == "left ctrl");
}

/* SDL's own '-' and 'Keypad -' names must not collapse to a bare word. */
void testLiteralSeparators()
{
    const KeyMap map;
    CHECK(map.canonicalName(SDL_SCANCODE_MINUS) == "-");
    CHECK(map.canonicalName(SDL_SCANCODE_KP_MINUS) == "keypad -");
    expectBinding(map, "-", SDL_SCANCODE_MINUS);
    expectBinding(map, "keypad -", SDL_SCANCODE_KP_MINUS);
}

/* Every name SDL reports must survive normalization and resolve again. */
void testWholeTableRoundTrip()
{
    const KeyMap map;
    for (int index = 0; index < SDL_SCANCODE_COUNT; ++index) {
        const SDL_Scancode scancode = static_cast<SDL_Scancode>(index);
        const char *rawName = SDL_GetScancodeName(scancode);
        const bool hasSdlName = rawName != nullptr && *rawName != '\0';
        const std::string_view canonical = map.canonicalName(scancode);

        if (!hasSdlName) {
            CHECK(canonical.empty());
            continue;
        }

        CHECK(!canonical.empty());
        CHECK(isLowercase(canonical));
        const std::optional<KeyBinding> binding = map.find(canonical);
        CHECK(binding.has_value());
        CHECK(binding->modifiers == SDL_KMOD_NONE);

        if (binding->scancode != scancode) {
            /* RETURN and RETURN2 share the name "Return"; the first wins. */
            CHECK(scancode == SDL_SCANCODE_RETURN2);
            CHECK(binding->scancode == SDL_SCANCODE_RETURN);
        }
    }
}

void testAliases()
{
    const KeyMap map;
    struct AliasCase
    {
        std::string_view name;
        SDL_Scancode scancode;
    };
    constexpr AliasCase cases[] = {
        {"enter", SDL_SCANCODE_RETURN},       {"esc", SDL_SCANCODE_ESCAPE},
        {"del", SDL_SCANCODE_DELETE},         {"ins", SDL_SCANCODE_INSERT},
        {"pgup", SDL_SCANCODE_PAGEUP},        {"pgdn", SDL_SCANCODE_PAGEDOWN},
        {"ctrl", SDL_SCANCODE_LCTRL},         {"control", SDL_SCANCODE_LCTRL},
        {"shift", SDL_SCANCODE_LSHIFT},       {"alt", SDL_SCANCODE_LALT},
        {"option", SDL_SCANCODE_LALT},        {"gui", SDL_SCANCODE_LGUI},
        {"cmd", SDL_SCANCODE_LGUI},           {"command", SDL_SCANCODE_LGUI},
        {"super", SDL_SCANCODE_LGUI},         {"win", SDL_SCANCODE_LGUI},
        {"windows", SDL_SCANCODE_LGUI},       {"keypad plus", SDL_SCANCODE_KP_PLUS},
        {"keypad +", SDL_SCANCODE_KP_PLUS},   {"keypad multiply", SDL_SCANCODE_KP_MULTIPLY},
        {"keypad star", SDL_SCANCODE_KP_MULTIPLY},
        {"keypad divide", SDL_SCANCODE_KP_DIVIDE},
        {"keypad slash", SDL_SCANCODE_KP_DIVIDE},
        {"keypad period", SDL_SCANCODE_KP_PERIOD},
        {"keypad dot", SDL_SCANCODE_KP_PERIOD}, {"keypad comma", SDL_SCANCODE_KP_COMMA},
        {"keypad equals", SDL_SCANCODE_KP_EQUALS},
        {"keypad minus", SDL_SCANCODE_KP_MINUS}, {"keypad_minus", SDL_SCANCODE_KP_MINUS},
        {"keypad-minus", SDL_SCANCODE_KP_MINUS},
    };
    for (const AliasCase &alias : cases) {
        expectBinding(map, alias.name, alias.scancode);
    }
    /* An alias never replaces a real SDL name. */
    CHECK(map.canonicalName(SDL_SCANCODE_LCTRL) == "left ctrl");
}

/* Single characters that are SDL names keep physical scancode semantics. */
void testNamedCharacters()
{
    const KeyMap map;
    expectBinding(map, "a", SDL_SCANCODE_A);
    expectBinding(map, "A", SDL_SCANCODE_A);
    expectBinding(map, "1", SDL_SCANCODE_1);
    expectBinding(map, "-", SDL_SCANCODE_MINUS);
    expectBinding(map, "=", SDL_SCANCODE_EQUALS);
}

/*
  Unnamed symbols need the live layout, so no US scancode is assumed: at least
  one must resolve, and each that does must map back to the same codepoint.
*/
void testFallbackSymbols()
{
    const KeyMap map;
    constexpr std::string_view symbols[] = {"!", "_", "+", "@", "#", "~"};
    std::size_t resolved = 0;
    for (const std::string_view symbol : symbols) {
        const std::optional<KeyBinding> binding = map.find(symbol);
        if (!binding.has_value()) {
            continue;
        }
        ++resolved;
        const auto codepoint =
            static_cast<SDL_Keycode>(static_cast<unsigned char>(symbol.front()));
        CHECK(SDL_GetKeyFromScancode(binding->scancode, binding->modifiers, false) == codepoint);
    }
    CHECK(resolved >= 1);
}

void testRejections()
{
    const KeyMap map;
    /* Chords belong to the chord parser, not the resolver. */
    expectMissing(map, "ctrl+s");
    expectMissing(map, "alt+f4");
    expectMissing(map, "ctrl+shift+s");

    expectMissing(map, "");
    expectMissing(map, " ");
    expectMissing(map, "   ");
    expectMissing(map, "\t\n");

    expectMissing(map, "definitely not a key");
    expectMissing(map, "keypad zzz");
    expectMissing(map, "hyper");

    /* Multi-codepoint punctuation is not a key. */
    expectMissing(map, "!!");
    expectMissing(map, "+-");
}

void testCanonicalNames()
{
    const KeyMap map;
    const SDL_Scancode sample[] = {
        SDL_SCANCODE_A,     SDL_SCANCODE_1,       SDL_SCANCODE_MINUS,
        SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_LCTRL,
        SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT,   SDL_SCANCODE_LGUI,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_SPACE,  SDL_SCANCODE_F4,
        SDL_SCANCODE_PAGEUP, SDL_SCANCODE_KP_PERIOD,
    };
    for (const SDL_Scancode scancode : sample) {
        const std::string_view canonical = map.canonicalName(scancode);
        CHECK(!canonical.empty());
        CHECK(isLowercase(canonical));
        expectBinding(map, canonical, scancode);
    }

    CHECK(map.canonicalName(SDL_SCANCODE_UNKNOWN).empty());
    CHECK(map.canonicalName(SDL_SCANCODE_COUNT).empty());
}

void testReturnDuplicate()
{
    const KeyMap map;
    expectBinding(map, "return", SDL_SCANCODE_RETURN);
    expectBinding(map, "Return", SDL_SCANCODE_RETURN);
    CHECK(map.canonicalName(SDL_SCANCODE_RETURN2) == "return");
}

} // namespace

int main()
{
    testNameVariants();
    testLiteralSeparators();
    testWholeTableRoundTrip();
    testAliases();
    testNamedCharacters();
    testFallbackSymbols();
    testRejections();
    testCanonicalNames();
    testReturnDuplicate();
    return 0;
}
