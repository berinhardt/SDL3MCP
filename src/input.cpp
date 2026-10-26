#include "input.hpp"

#include <cmath>
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

/*
  Names the last enumerator of an enum, so a value cast in from the wire is
  checked rather than trusted: an unknown value must not fall through a switch
  into a default and be applied as something else.
*/
template <typename Enum>
bool isKnownEnumValue(Enum value, Enum last) noexcept
{
    const int raw = static_cast<int>(value);
    return raw >= 0 && raw <= static_cast<int>(last);
}

std::optional<std::string> validateKeyInput(const KeyInput &input)
{
    if (!isKnownEnumValue(input.action, KeyAction::Press)) {
        return "unknown key action";
    }
    if (input.key.empty()) {
        return "key must not be empty";
    }
    if (input.key.size() > kMaxKeyBytes) {
        return "key is " + std::to_string(input.key.size()) + " bytes; the maximum is "
               + std::to_string(kMaxKeyBytes);
    }
    if (input.modifiers.size() > kMaxModifiersPerKey) {
        return "key has " + std::to_string(input.modifiers.size()) + " modifiers; the maximum is "
               + std::to_string(kMaxModifiersPerKey);
    }
    /* A held key carries no modifier mask, so modifiers cannot mean anything. */
    if (input.action != KeyAction::Press && !input.modifiers.empty()) {
        return "modifiers are only valid for press";
    }
    return std::nullopt;
}

std::optional<std::string> validateTextInput(const TextInput &input)
{
    if (input.text.empty()) {
        return "text must not be empty";
    }
    /* The limit is bytes; splitting at UTF-8 boundaries is the replay step's job. */
    if (input.text.size() > kMaxTextBytes) {
        return "text is " + std::to_string(input.text.size()) + " bytes; the maximum is "
               + std::to_string(kMaxTextBytes);
    }
    return std::nullopt;
}

std::optional<std::string> validateMouseMoveInput(const MouseMoveInput &input)
{
    if (!isKnownEnumValue(input.mode, MouseMoveInput::Mode::Relative)) {
        return "unknown mouse move mode";
    }
    if (!std::isfinite(input.x)) {
        return "mouse move x must be finite";
    }
    if (!std::isfinite(input.y)) {
        return "mouse move y must be finite";
    }
    return std::nullopt;
}

std::optional<std::string> validateMouseButtonInput(const MouseButtonInput &input)
{
    if (!isKnownEnumValue(input.button, MouseButton::X2)) {
        return "unknown mouse button";
    }
    if (!isKnownEnumValue(input.action, MouseButtonAction::Click)) {
        return "unknown mouse button action";
    }
    if (input.x.has_value() != input.y.has_value()) {
        return "x and y must be provided together";
    }
    if (input.x.has_value() && !std::isfinite(*input.x)) {
        return "mouse button x must be finite";
    }
    if (input.y.has_value() && !std::isfinite(*input.y)) {
        return "mouse button y must be finite";
    }
    if (input.clicks < kMinClicks || input.clicks > kMaxClicks) {
        return "clicks must be between " + std::to_string(kMinClicks) + " and "
               + std::to_string(kMaxClicks);
    }
    return std::nullopt;
}

std::optional<std::string> validateScrollInput(const ScrollInput &input)
{
    if (!std::isfinite(input.x)) {
        return "scroll x must be finite";
    }
    if (!std::isfinite(input.y)) {
        return "scroll y must be finite";
    }
    return std::nullopt;
}

/* Dispatch without a generic lambda, so each rule stays a named function. */
struct ActionValidator
{
    std::optional<std::string> operator()(const KeyInput &input) const
    {
        return validateKeyInput(input);
    }
    std::optional<std::string> operator()(const TextInput &input) const
    {
        return validateTextInput(input);
    }
    std::optional<std::string> operator()(const MouseMoveInput &input) const
    {
        return validateMouseMoveInput(input);
    }
    std::optional<std::string> operator()(const MouseButtonInput &input) const
    {
        return validateMouseButtonInput(input);
    }
    std::optional<std::string> operator()(const ScrollInput &input) const
    {
        return validateScrollInput(input);
    }
    std::optional<std::string> operator()(const ReleaseAllInput &) const { return std::nullopt; }
};

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

std::optional<std::string> validateStructure(const InputBatch &batch)
{
    if (batch.actions.empty()) {
        return "the batch must contain at least one action";
    }
    if (batch.actions.size() > kMaxActionsPerBatch) {
        return "the batch has " + std::to_string(batch.actions.size()) + " actions; the maximum is "
               + std::to_string(kMaxActionsPerBatch);
    }

    for (std::size_t index = 0; index < batch.actions.size(); ++index) {
        const std::optional<std::string> error =
            std::visit(ActionValidator{}, batch.actions[index]);
        if (error.has_value()) {
            return "actions[" + std::to_string(index) + "]: " + *error;
        }
    }
    return std::nullopt;
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
