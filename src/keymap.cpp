#include "keymap.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sdl3mcp
{
namespace
{

constexpr char kSeparator = ' ';

struct Alias
{
    std::string_view alias;
    std::string_view canonical;
};

/*
  Only entries that SDL itself does not already provide as a scancode name.
  The target is a canonical name, so the alias inherits its modifiers (none).
*/
constexpr Alias kAliases[] = {
    {"enter", "Return"},
    {"esc", "Escape"},
    {"del", "Delete"},
    {"ins", "Insert"},
    {"pgup", "PageUp"},
    {"pgdn", "PageDown"},
    {"ctrl", "Left Ctrl"},
    {"control", "Left Ctrl"},
    {"shift", "Left Shift"},
    {"alt", "Left Alt"},
    {"option", "Left Alt"},
    {"gui", "Left GUI"},
    {"cmd", "Left GUI"},
    {"command", "Left GUI"},
    {"super", "Left GUI"},
    {"win", "Left GUI"},
    {"windows", "Left GUI"},
    {"keypad plus", "Keypad +"},
    {"keypad minus", "Keypad -"},
    {"keypad multiply", "Keypad *"},
    {"keypad star", "Keypad *"},
    {"keypad divide", "Keypad /"},
    {"keypad slash", "Keypad /"},
    {"keypad period", "Keypad ."},
    {"keypad dot", "Keypad ."},
    {"keypad comma", "Keypad ,"},
    {"keypad equals", "Keypad ="},
};

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

/* A word character is anything that is neither whitespace nor '-' or '_'. */
bool isWordCharacter(char character) noexcept
{
    return !isAsciiWhitespace(character) && character != '-' && character != '_';
}

std::optional<std::size_t> firstWordIndex(std::string_view text) noexcept
{
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (isWordCharacter(text[index])) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> lastWordIndex(std::string_view text) noexcept
{
    for (std::size_t index = text.size(); index > 0; --index) {
        if (isWordCharacter(text[index - 1])) {
            return index - 1;
        }
    }
    return std::nullopt;
}

/*
  Lowercases and folds word separators to single spaces. Whitespace always
  separates; '-' and '_' only separate when a word character exists on both
  sides, which keeps SDL's own '-' and 'Keypad -' names intact.
*/
std::string normalize(std::string_view name)
{
    const std::string_view trimmed = trimWhitespace(name);
    const std::optional<std::size_t> first = firstWordIndex(trimmed);
    const std::optional<std::size_t> last = lastWordIndex(trimmed);

    std::string normalized;
    normalized.reserve(trimmed.size());
    for (std::size_t index = 0; index < trimmed.size(); ++index) {
        const char character = trimmed[index];
        const bool betweenWords = first.has_value() && last.has_value() && *first < index
                                  && index < *last;
        const bool isSeparator = isAsciiWhitespace(character)
                                 || ((character == '-' || character == '_') && betweenWords);
        if (!isSeparator) {
            normalized.push_back(toAsciiLower(character));
            continue;
        }
        if (!normalized.empty() && normalized.back() != kSeparator) {
            normalized.push_back(kSeparator);
        }
    }
    return normalized;
}

std::optional<std::uint32_t> decodeSingleCodepoint(std::string_view text)
{
    if (text.empty()) {
        return std::nullopt;
    }

    const char *cursor = text.data();
    std::size_t remaining = text.size();
    const Uint32 codepoint = SDL_StepUTF8(&cursor, &remaining);
    if (remaining != 0) {
        return std::nullopt;
    }
    if (codepoint == 0 || codepoint == SDL_INVALID_UNICODE_CODEPOINT) {
        return std::nullopt;
    }
    if (codepoint < 0x20 || codepoint == 0x7F) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(codepoint);
}

} // namespace

KeyMap::KeyMap() : canonicalNames_(buildCanonicalNames()), names_(buildNames(canonicalNames_))
{
}

std::optional<KeyBinding> KeyMap::find(std::string_view name) const
{
    const std::string normalized = normalize(name);
    if (!normalized.empty()) {
        const auto named = names_.find(normalized);
        if (named != names_.end()) {
            return named->second;
        }
    }

    /* Layout-dependent fallback for characters SDL has no scancode name for. */
    const std::optional<std::uint32_t> codepoint = decodeSingleCodepoint(trimWhitespace(name));
    if (!codepoint.has_value()) {
        return std::nullopt;
    }

    SDL_Keymod modifiers = SDL_KMOD_NONE;
    const SDL_Scancode scancode =
        SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(*codepoint), &modifiers);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return std::nullopt;
    }
    return KeyBinding{scancode, modifiers};
}

std::string_view KeyMap::canonicalName(SDL_Scancode scancode) const
{
    const int index = static_cast<int>(scancode);
    if (index < 0 || index >= SDL_SCANCODE_COUNT) {
        return {};
    }
    return canonicalNames_[static_cast<std::size_t>(index)];
}

KeyMap::CanonicalNames KeyMap::buildCanonicalNames()
{
    CanonicalNames canonicalNames;
    for (int index = 0; index < SDL_SCANCODE_COUNT; ++index) {
        const char *rawName = SDL_GetScancodeName(static_cast<SDL_Scancode>(index));
        if (rawName != nullptr) {
            canonicalNames[static_cast<std::size_t>(index)] = normalize(rawName);
        }
    }
    return canonicalNames;
}

KeyMap::NameTable KeyMap::buildNames(const CanonicalNames &canonicalNames)
{
    NameTable names;
    for (std::size_t index = 0; index < canonicalNames.size(); ++index) {
        const std::string &canonical = canonicalNames[index];
        if (canonical.empty()) {
            continue;
        }
        /* First binding per name wins: RETURN keeps "return" over RETURN2. */
        names.try_emplace(canonical,
                          KeyBinding{static_cast<SDL_Scancode>(index), SDL_KMOD_NONE});
    }

    for (const Alias &alias : kAliases) {
        const std::string aliasName = normalize(alias.alias);
        const std::string canonical = normalize(alias.canonical);
        const auto target = names.find(canonical);
        if (aliasName.empty() || target == names.end()) {
            continue;
        }
        /* Never replace a real SDL name with an alias. */
        names.try_emplace(aliasName, target->second);
    }
    return names;
}

} // namespace sdl3mcp
