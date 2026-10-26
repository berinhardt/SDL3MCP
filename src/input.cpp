#include "input.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace sdl3mcp
{
namespace
{

constexpr char kChordSeparator = '+';

/* Lock keys (caps/num/scroll) and AltGr cannot be synthesized, so reject them. */
constexpr SDL_Keymod kSupportedModifiers =
    SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI;

bool isAsciiWhitespace(char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r'
           || character == '\f' || character == '\v';
}

char toAsciiLower(char character) noexcept
{
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

std::string_view trimWhitespace(std::string_view text) noexcept
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isAsciiWhitespace(text[begin])) {
        ++begin;
    }
    while (end > begin && isAsciiWhitespace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

void setError(std::string &error, std::string message)
{
    error = std::move(message);
}

/*
  Layout lookups report required modifiers as the generic CTRL/SHIFT/ALT/GUI
  bits. Replay owns physical keys, so fold them onto the left side: '!' becomes
  LSHIFT rather than "either shift".
*/
std::optional<SDL_Keymod> leftSideModifiers(SDL_Keymod required, std::string &error)
{
    const auto unsupported = static_cast<SDL_Keymod>(required & ~kSupportedModifiers);
    if (unsupported != SDL_KMOD_NONE) {
        setError(error, "key requires an unsupported modifier");
        return std::nullopt;
    }

    SDL_Keymod leftSide = SDL_KMOD_NONE;
    if ((required & SDL_KMOD_CTRL) != 0) {
        leftSide |= SDL_KMOD_LCTRL;
    }
    if ((required & SDL_KMOD_SHIFT) != 0) {
        leftSide |= SDL_KMOD_LSHIFT;
    }
    if ((required & SDL_KMOD_ALT) != 0) {
        leftSide |= SDL_KMOD_LALT;
    }
    if ((required & SDL_KMOD_GUI) != 0) {
        leftSide |= SDL_KMOD_LGUI;
    }
    return leftSide;
}

/* `modifiers` is already left-side only; the binding adds what the key needs. */
std::optional<KeyChord> toChord(const KeyBinding &binding,
                                SDL_Keymod modifiers,
                                std::string &error)
{
    const std::optional<SDL_Keymod> required = leftSideModifiers(binding.modifiers, error);
    if (!required.has_value()) {
        return std::nullopt;
    }
    return KeyChord{binding.scancode, static_cast<SDL_Keymod>(*required | modifiers)};
}

SDL_Keymod modifierBits(std::span<const Modifier> modifiers) noexcept
{
    SDL_Keymod bits = SDL_KMOD_NONE;
    for (const Modifier modifier : modifiers) {
        bits |= modifierBit(modifier);
    }
    return bits;
}

/*
  Splits the spec at '+'. Every segment but the last must name a modifier; the
  last is the key. Repeated bits merge idempotently because the mask is an OR.
*/
std::optional<KeyChord> resolveChord(std::string_view spec,
                                     SDL_Keymod modifiers,
                                     const KeyMap &keymap,
                                     std::string &error)
{
    std::string_view keySegment;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t separator = spec.find(kChordSeparator, begin);
        const std::size_t length =
            separator == std::string_view::npos ? std::string_view::npos : separator - begin;
        const std::string_view segment = trimWhitespace(spec.substr(begin, length));
        if (segment.empty()) {
            setError(error, "empty segment in key chord \"" + std::string(spec) + "\"");
            return std::nullopt;
        }

        if (separator == std::string_view::npos) {
            keySegment = segment;
            break;
        }

        const std::optional<Modifier> modifier = parseModifier(segment);
        if (!modifier.has_value()) {
            setError(error, "unknown modifier \"" + std::string(segment) + "\"");
            return std::nullopt;
        }
        modifiers |= modifierBit(*modifier);
        begin = separator + 1;
    }

    const std::optional<KeyBinding> binding = keymap.find(keySegment);
    if (!binding.has_value()) {
        setError(error, "unknown key \"" + std::string(keySegment) + "\"");
        return std::nullopt;
    }
    return toChord(*binding, modifiers, error);
}

} // namespace

std::optional<Modifier> parseModifier(std::string_view name)
{
    std::string normalized;
    normalized.reserve(name.size());
    for (const char character : name) {
        normalized.push_back(toAsciiLower(character));
    }

    if (normalized == "ctrl") {
        return Modifier::Ctrl;
    }
    if (normalized == "shift") {
        return Modifier::Shift;
    }
    if (normalized == "alt") {
        return Modifier::Alt;
    }
    if (normalized == "gui") {
        return Modifier::Gui;
    }
    return std::nullopt;
}

SDL_Keymod modifierBit(Modifier modifier)
{
    switch (modifier) {
    case Modifier::Ctrl:
        return SDL_KMOD_LCTRL;
    case Modifier::Shift:
        return SDL_KMOD_LSHIFT;
    case Modifier::Alt:
        return SDL_KMOD_LALT;
    case Modifier::Gui:
        return SDL_KMOD_LGUI;
    }
    return SDL_KMOD_NONE;
}

std::optional<KeyChord> resolveKey(std::string_view spec,
                                   const KeyMap &keymap,
                                   std::string &error)
{
    const std::string_view trimmed = trimWhitespace(spec);
    if (trimmed.empty()) {
        setError(error, "key name is empty");
        return std::nullopt;
    }

    /* The whole name first: "keypad +" is a key, not a malformed chord. */
    const std::optional<KeyBinding> binding = keymap.find(trimmed);
    if (binding.has_value()) {
        return toChord(*binding, SDL_KMOD_NONE, error);
    }

    if (trimmed.find(kChordSeparator) != std::string_view::npos) {
        setError(error,
                 "key \"" + std::string(trimmed)
                     + "\" is a chord; chords are only valid for press");
        return std::nullopt;
    }
    setError(error, "unknown key \"" + std::string(trimmed) + "\"");
    return std::nullopt;
}

std::optional<KeyChord> resolveKeyChord(std::string_view spec,
                                        std::span<const Modifier> explicitModifiers,
                                        const KeyMap &keymap,
                                        std::string &error)
{
    const std::string_view trimmed = trimWhitespace(spec);
    if (trimmed.empty()) {
        setError(error, "key name is empty");
        return std::nullopt;
    }

    const SDL_Keymod modifiers = modifierBits(explicitModifiers);

    /* Keep names that literally contain '+' ("keypad +") out of the splitter. */
    const std::optional<KeyBinding> whole = keymap.find(trimmed);
    if (whole.has_value()) {
        return toChord(*whole, modifiers, error);
    }
    return resolveChord(trimmed, modifiers, keymap, error);
}

} // namespace sdl3mcp
