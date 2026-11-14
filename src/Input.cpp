#include "Input.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Capture.h"
#include "InputJson.h"

namespace sdl3mcp {
namespace {

constexpr char kChordSeparator = '+';

/* Lock keys (caps/num/scroll) and AltGr cannot be synthesized, so reject them. */
constexpr SDL_Keymod kSupportedModifiers =
    SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI;

bool isAsciiWhitespace(char character) noexcept {
   return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' || character == '\v';
}

char toAsciiLower(char character) noexcept {
   if (character >= 'A' && character <= 'Z') {
      return static_cast<char>(character - 'A' + 'a');
   }
   return character;
}

std::string_view trimWhitespace(std::string_view text) noexcept {
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

/*
  Layout lookups report required modifiers as the generic CTRL/SHIFT/ALT/GUI
  bits. Replay owns physical keys, so fold them onto the left side: '!' becomes
  LSHIFT rather than "either shift".
*/
Result<SDL_Keymod, ProtocolError> leftSideModifiers(SDL_Keymod required) {
   const auto unsupported = static_cast<SDL_Keymod>(required & ~kSupportedModifiers);
   if (unsupported != SDL_KMOD_NONE) {
      return ProtocolError{"key requires an unsupported modifier"};
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
Result<KeyChord, ProtocolError> toChord(const KeyBinding& binding,
                                        SDL_Keymod modifiers) {
   const Result<SDL_Keymod, ProtocolError> required =
       leftSideModifiers(binding.modifiers);
   if (!required) {
      return required.error();
   }
   return KeyChord{binding.scancode,
                   static_cast<SDL_Keymod>(*required | modifiers)};
}

SDL_Keymod modifierBits(std::span<const Modifier> modifiers) noexcept {
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
Result<KeyChord, ProtocolError> resolveChord(std::string_view spec,
                                             SDL_Keymod modifiers,
                                             const KeyMap& keymap) {
   std::string_view keySegment;
   std::size_t begin = 0;
   for (;;) {
      const std::size_t separator = spec.find(kChordSeparator, begin);
      const std::size_t length =
          separator == std::string_view::npos ? std::string_view::npos : separator - begin;
      const std::string_view segment = trimWhitespace(spec.substr(begin, length));
      if (segment.empty()) {
         return ProtocolError{"empty segment in key chord \"" +
                              std::string(spec) + "\""};
      }

      if (separator == std::string_view::npos) {
         keySegment = segment;
         break;
      }

      const std::optional<Modifier> modifier = parseModifier(segment);
      if (!modifier.has_value()) {
         return ProtocolError{"unknown modifier \"" + std::string(segment) + "\""};
      }
      modifiers |= modifierBit(*modifier);
      begin = separator + 1;
   }

   const std::optional<KeyBinding> binding = keymap.find(keySegment);
   if (!binding.has_value()) {
      return ProtocolError{"unknown key \"" + std::string(keySegment) + "\""};
   }
   return toChord(*binding, modifiers);
}

/*
  Names the last enumerator of an enum, so a value cast in from the wire is
  checked rather than trusted: an unknown value must not fall through a switch
  into a default and be applied as something else.
*/
template <typename Enum>
bool isKnownEnumValue(Enum value, Enum last) noexcept {
   const int raw = static_cast<int>(value);
   return raw >= 0 && raw <= static_cast<int>(last);
}

Result<void, ProtocolError> validateKeyInput(const KeyInput& input) {
   if (!isKnownEnumValue(input.action, KeyAction::Press)) {
      return ProtocolError{"unknown key action"};
   }
   if (input.key.empty()) {
      return ProtocolError{"key must not be empty"};
   }
   if (input.key.size() > kMaxKeyBytes) {
      return ProtocolError{"key is " + std::to_string(input.key.size()) +
                           " bytes; the maximum is " +
                           std::to_string(kMaxKeyBytes)};
   }
   if (input.modifiers.size() > kMaxModifiersPerKey) {
      return ProtocolError{"key has " + std::to_string(input.modifiers.size()) +
                           " modifiers; the maximum is " +
                           std::to_string(kMaxModifiersPerKey)};
   }
   /* A held key carries no modifier mask, so modifiers cannot mean anything. */
   if (input.action != KeyAction::Press && !input.modifiers.empty()) {
      return ProtocolError{"modifiers are only valid for press"};
   }
   return {};
}

Result<void, ProtocolError> validateTextInput(const TextInput& input) {
   if (input.text.empty()) {
      return ProtocolError{"text must not be empty"};
   }
   /* The limit is bytes; splitting at UTF-8 boundaries is the replay step's job. */
   if (input.text.size() > kMaxTextBytes) {
      return ProtocolError{"text is " + std::to_string(input.text.size()) +
                           " bytes; the maximum is " +
                           std::to_string(kMaxTextBytes)};
   }
   return {};
}

Result<void, ProtocolError> validateMouseMoveInput(const MouseMoveInput& input) {
   if (!isKnownEnumValue(input.mode, MouseMoveInput::Mode::Relative)) {
      return ProtocolError{"unknown mouse move mode"};
   }
   if (!std::isfinite(input.x)) {
      return ProtocolError{"mouse move x must be finite"};
   }
   if (!std::isfinite(input.y)) {
      return ProtocolError{"mouse move y must be finite"};
   }
   return {};
}

Result<void, ProtocolError> validateMouseButtonInput(const MouseButtonInput& input) {
   if (!isKnownEnumValue(input.button, MouseButton::X2)) {
      return ProtocolError{"unknown mouse button"};
   }
   if (!isKnownEnumValue(input.action, MouseButtonAction::Click)) {
      return ProtocolError{"unknown mouse button action"};
   }
   if (input.x.has_value() != input.y.has_value()) {
      return ProtocolError{"x and y must be provided together"};
   }
   if (input.x.has_value() && !std::isfinite(*input.x)) {
      return ProtocolError{"mouse button x must be finite"};
   }
   if (input.y.has_value() && !std::isfinite(*input.y)) {
      return ProtocolError{"mouse button y must be finite"};
   }
   if (input.clicks < kMinClicks || input.clicks > kMaxClicks) {
      return ProtocolError{"clicks must be between " + std::to_string(kMinClicks) +
                           " and " + std::to_string(kMaxClicks)};
   }
   return {};
}

Result<void, ProtocolError> validateScrollInput(const ScrollInput& input) {
   if (!std::isfinite(input.x)) {
      return ProtocolError{"scroll x must be finite"};
   }
   if (!std::isfinite(input.y)) {
      return ProtocolError{"scroll y must be finite"};
   }
   return {};
}

/* Dispatch without a generic lambda, so each rule stays a named function. */
struct ActionValidator {
   Result<void, ProtocolError> operator()(const KeyInput& input) const {
      return validateKeyInput(input);
   }
   Result<void, ProtocolError> operator()(const TextInput& input) const {
      return validateTextInput(input);
   }
   Result<void, ProtocolError> operator()(const MouseMoveInput& input) const {
      return validateMouseMoveInput(input);
   }
   Result<void, ProtocolError> operator()(const MouseButtonInput& input) const {
      return validateMouseButtonInput(input);
   }
   Result<void, ProtocolError> operator()(const ScrollInput& input) const {
      return validateScrollInput(input);
   }
   Result<void, ProtocolError> operator()(const ReleaseAllInput&) const {
      return {};
   }
};

}  // namespace

std::optional<Modifier> parseModifier(std::string_view name) {
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

SDL_Keymod modifierBit(Modifier modifier) {
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

Result<void, ProtocolError> validateStructure(const InputBatch& batch) {
   if (batch.actions.empty()) {
      return ProtocolError{"the batch must contain at least one action"};
   }
   if (batch.actions.size() > kMaxActionsPerBatch) {
      return ProtocolError{"the batch has " +
                           std::to_string(batch.actions.size()) +
                           " actions; the maximum is " +
                           std::to_string(kMaxActionsPerBatch)};
   }

   for (std::size_t index = 0; index < batch.actions.size(); ++index) {
      const Result<void, ProtocolError> error =
          std::visit(ActionValidator{}, batch.actions[index]);
      if (!error) {
         return ProtocolError{"actions[" + std::to_string(index) +
                              "]: " + error.error().message};
      }
   }
   return {};
}

Result<KeyChord, ProtocolError> resolveKey(std::string_view spec,
                                           const KeyMap& keymap) {
   const std::string_view trimmed = trimWhitespace(spec);
   if (trimmed.empty()) {
      return ProtocolError{"key name is empty"};
   }

   /* The whole name first: "keypad +" is a key, not a malformed chord. */
   const std::optional<KeyBinding> binding = keymap.find(trimmed);
   if (binding.has_value()) {
      return toChord(*binding, SDL_KMOD_NONE);
   }

   if (trimmed.find(kChordSeparator) != std::string_view::npos) {
      return ProtocolError{"key \"" + std::string(trimmed) +
                           "\" is a chord; chords are only valid for press"};
   }
   return ProtocolError{"unknown key \"" + std::string(trimmed) + "\""};
}

Result<KeyChord, ProtocolError> resolveKeyChord(
    std::string_view spec,
    std::span<const Modifier> explicitModifiers,
    const KeyMap& keymap) {
   const std::string_view trimmed = trimWhitespace(spec);
   if (trimmed.empty()) {
      return ProtocolError{"key name is empty"};
   }

   const SDL_Keymod modifiers = modifierBits(explicitModifiers);

   /* Keep names that literally contain '+' ("keypad +") out of the splitter. */
   const std::optional<KeyBinding> whole = keymap.find(trimmed);
   if (whole.has_value()) {
      return toChord(*whole, modifiers);
   }
   return resolveChord(trimmed, modifiers, keymap);
}

namespace {

using nlohmann::json;

std::string actionPrefix(std::size_t index) {
   return "actions[" + std::to_string(index) + "]: ";
}

/*
  Rejects the first member the closed schema for this action type does not
  declare. Iteration order is the object's key order, so the reported name is
  stable for a given request.
*/
Result<void, ProtocolError> rejectUnknownProperties(
    const json& value, std::span<const std::string_view> allowed) {
   for (auto entry = value.begin(); entry != value.end(); ++entry) {
      const std::string_view name = entry.key();
      bool isAllowed = false;
      for (const std::string_view candidate : allowed) {
         if (name == candidate) {
            isAllowed = true;
            break;
         }
      }
      if (!isAllowed) {
         return ProtocolError{"unknown property \"" + entry.key() + "\""};
      }
   }
   return {};
}

Result<std::string, ProtocolError> readRequiredString(const json& value,
                                                      const char* name) {
   const auto entry = value.find(name);
   if (entry == value.end()) {
      return ProtocolError{std::string(name) + " is required"};
   }
   if (!entry->is_string()) {
      return ProtocolError{std::string(name) + " must be a string"};
   }
   return entry->get<std::string>();
}

/* A finite number as JSON writes it; the cast to float happens at the call site. */
Result<double, ProtocolError> readNumber(const json& value, const char* name) {
   const auto entry = value.find(name);
   if (entry == value.end() || !entry->is_number()) {
      return ProtocolError{std::string(name) + " must be a number"};
   }
   const double number = entry->get<double>();
   if (!std::isfinite(number)) {
      return ProtocolError{std::string(name) + " must be finite"};
   }
   return number;
}

Result<double, ProtocolError> readRequiredNumber(const json& value,
                                                 const char* name) {
   if (value.find(name) == value.end()) {
      return ProtocolError{std::string(name) + " is required"};
   }
   return readNumber(value, name);
}

std::optional<KeyAction> toKeyAction(std::string_view name) {
   if (name == "down") {
      return KeyAction::Down;
   }
   if (name == "up") {
      return KeyAction::Up;
   }
   if (name == "press") {
      return KeyAction::Press;
   }
   return std::nullopt;
}

std::optional<MouseButton> toMouseButton(std::string_view name) {
   if (name == "left") {
      return MouseButton::Left;
   }
   if (name == "middle") {
      return MouseButton::Middle;
   }
   if (name == "right") {
      return MouseButton::Right;
   }
   if (name == "x1") {
      return MouseButton::X1;
   }
   if (name == "x2") {
      return MouseButton::X2;
   }
   return std::nullopt;
}

std::optional<MouseButtonAction> toMouseButtonAction(std::string_view name) {
   if (name == "down") {
      return MouseButtonAction::Down;
   }
   if (name == "up") {
      return MouseButtonAction::Up;
   }
   if (name == "click") {
      return MouseButtonAction::Click;
   }
   return std::nullopt;
}

Result<InputAction, ProtocolError> parseKey(const json& value) {
   KeyInput input;

   const Result<std::string, ProtocolError> key =
       readRequiredString(value, "key");
   if (!key) {
      return key.error();
   }
   input.key = *key;

   if (const auto entry = value.find("action"); entry != value.end()) {
      if (!entry->is_string()) {
         return ProtocolError{"action must be a string"};
      }
      const std::string name = entry->get<std::string>();
      const std::optional<KeyAction> action = toKeyAction(name);
      if (!action.has_value()) {
         return ProtocolError{"unknown key action \"" + name + "\""};
      }
      input.action = *action;
   }

   if (const auto entry = value.find("modifiers"); entry != value.end()) {
      if (!entry->is_array()) {
         return ProtocolError{"modifiers must be an array"};
      }
      if (entry->size() > kMaxModifiersPerKey) {
         return ProtocolError{"modifiers has " + std::to_string(entry->size()) +
                              " entries; the maximum is " +
                              std::to_string(kMaxModifiersPerKey)};
      }
      for (const json& item : *entry) {
         if (!item.is_string()) {
            return ProtocolError{"modifiers must be strings"};
         }
         const std::string name = item.get<std::string>();
         const std::optional<Modifier> modifier = parseModifier(name);
         if (!modifier.has_value()) {
            return ProtocolError{"unknown modifier \"" + name + "\""};
         }
         input.modifiers.push_back(*modifier);
      }
   }

   return InputAction{std::move(input)};
}

Result<InputAction, ProtocolError> parseText(const json& value) {
   const Result<std::string, ProtocolError> text =
       readRequiredString(value, "text");
   if (!text) {
      return text.error();
   }
   return InputAction{TextInput{*text}};
}

/*
  The two wire forms are exclusive: x/y are absolute window points, dx/dy are
  relative deltas. A half pair or a mix of both would leave the mode ambiguous,
  so every such shape is refused instead of being guessed at.
*/
Result<InputAction, ProtocolError> parseMouseMove(const json& value) {
   const bool hasX = value.contains("x");
   const bool hasY = value.contains("y");
   const bool hasDx = value.contains("dx");
   const bool hasDy = value.contains("dy");

   if ((hasX && hasDx) || (hasY && hasDy)) {
      return ProtocolError{"provide either x and y or dx and dy, not both"};
   }
   if (hasX != hasY) {
      return ProtocolError{"x and y must be provided together"};
   }
   if (hasDx != hasDy) {
      return ProtocolError{"dx and dy must be provided together"};
   }
   if (!hasX && !hasDx) {
      return ProtocolError{"mouse_move requires x and y or dx and dy"};
   }

   const Result<double, ProtocolError> x =
       readNumber(value, hasX ? "x" : "dx");
   if (!x) {
      return x.error();
   }
   const Result<double, ProtocolError> y =
       readNumber(value, hasX ? "y" : "dy");
   if (!y) {
      return y.error();
   }

   MouseMoveInput input;
   input.mode = hasX ? MouseMoveInput::Mode::Absolute : MouseMoveInput::Mode::Relative;
   input.x = static_cast<float>(*x);
   input.y = static_cast<float>(*y);
   return InputAction{input};
}

Result<InputAction, ProtocolError> parseMouseButton(const json& value) {
   MouseButtonInput input;

   const Result<std::string, ProtocolError> button =
       readRequiredString(value, "button");
   if (!button) {
      return button.error();
   }
   const std::optional<MouseButton> parsedButton = toMouseButton(*button);
   if (!parsedButton.has_value()) {
      return ProtocolError{"unknown mouse button \"" + *button + "\""};
   }
   input.button = *parsedButton;

   if (const auto entry = value.find("action"); entry != value.end()) {
      if (!entry->is_string()) {
         return ProtocolError{"action must be a string"};
      }
      const std::string name = entry->get<std::string>();
      const std::optional<MouseButtonAction> action = toMouseButtonAction(name);
      if (!action.has_value()) {
         return ProtocolError{"unknown mouse button action \"" + name + "\""};
      }
      input.action = *action;
   }

   const bool hasX = value.contains("x");
   const bool hasY = value.contains("y");
   if (hasX != hasY) {
      return ProtocolError{"x and y must be provided together"};
   }
   if (hasX) {
      const Result<double, ProtocolError> x = readNumber(value, "x");
      if (!x) {
         return x.error();
      }
      const Result<double, ProtocolError> y = readNumber(value, "y");
      if (!y) {
         return y.error();
      }
      input.x = static_cast<float>(*x);
      input.y = static_cast<float>(*y);
   }

   /*
     Only the JSON type is checked here; the 1..3 range belongs to
     validateStructure so both entry points share one set of model rules.
   */
   if (const auto entry = value.find("clicks"); entry != value.end()) {
      if (!entry->is_number_integer()) {
         return ProtocolError{"clicks must be an integer"};
      }
      const double clicks = entry->get<double>();
      if (clicks < static_cast<double>(std::numeric_limits<int>::min()) ||
          clicks > static_cast<double>(std::numeric_limits<int>::max())) {
         return ProtocolError{"clicks is out of range"};
      }
      input.clicks = static_cast<int>(clicks);
   }

   return InputAction{input};
}

Result<InputAction, ProtocolError> parseScroll(const json& value) {
   ScrollInput input;

   const Result<double, ProtocolError> y = readRequiredNumber(value, "y");
   if (!y) {
      return y.error();
   }
   input.y = static_cast<float>(*y);

   if (value.contains("x")) {
      const Result<double, ProtocolError> x = readNumber(value, "x");
      if (!x) {
         return x.error();
      }
      input.x = static_cast<float>(*x);
   }

   return InputAction{input};
}

Result<InputAction, ProtocolError> parseReleaseAll(const json&) {
   return InputAction{ReleaseAllInput{}};
}

using ActionParser = Result<InputAction, ProtocolError> (*)(const json& value);

struct ActionType {
   std::string_view name;
   std::span<const std::string_view> properties;
   ActionParser parse;
};

constexpr std::array<std::string_view, 4> kKeyProperties{"type", "key", "action",
                                                         "modifiers"};
constexpr std::array<std::string_view, 2> kTextProperties{"type", "text"};
constexpr std::array<std::string_view, 5> kMouseMoveProperties{"type", "x", "y",
                                                               "dx", "dy"};
constexpr std::array<std::string_view, 6> kMouseButtonProperties{
    "type", "button", "action", "x", "y", "clicks"};
constexpr std::array<std::string_view, 3> kScrollProperties{"type", "x", "y"};
constexpr std::array<std::string_view, 1> kReleaseAllProperties{"type"};
constexpr std::array<std::string_view, 2> kArgumentsProperties{"actions",
                                                               "region"};

constexpr std::array<ActionType, 6> kActionTypes{{
    {"key", kKeyProperties, parseKey},
    {"text", kTextProperties, parseText},
    {"mouse_move", kMouseMoveProperties, parseMouseMove},
    {"mouse_button", kMouseButtonProperties, parseMouseButton},
    {"scroll", kScrollProperties, parseScroll},
    {"release_all", kReleaseAllProperties, parseReleaseAll},
}};

Result<InputAction, ProtocolError> parseAction(const json& value) {
   if (!value.is_object()) {
      return ProtocolError{"action must be an object"};
   }

   const auto typeEntry = value.find("type");
   if (typeEntry == value.end()) {
      return ProtocolError{"type is required"};
   }
   if (!typeEntry->is_string()) {
      return ProtocolError{"type must be a string"};
   }
   const std::string_view type = typeEntry->get_ref<const std::string&>();

   for (const ActionType& candidate : kActionTypes) {
      if (candidate.name != type) {
         continue;
      }
      const Result<void, ProtocolError> unknown =
          rejectUnknownProperties(value, candidate.properties);
      if (!unknown) {
         return unknown.error();
      }
      return candidate.parse(value);
   }

   return ProtocolError{"unknown action type \"" + std::string(type) + "\""};
}

}  // namespace

Result<InputRequest, ProtocolError> parseInputArguments(
    const nlohmann::json& arguments) {
   if (!arguments.is_object()) {
      return ProtocolError{"arguments must be a JSON object"};
   }
   const Result<void, ProtocolError> unknown =
       rejectUnknownProperties(arguments, kArgumentsProperties);
   if (!unknown) {
      return unknown.error();
   }

   InputRequest request;
   const Result<void, ProtocolError> region =
       parseRegionArgument(arguments, request.region);
   if (!region) {
      return region.error();
   }

   const auto actionsEntry = arguments.find("actions");
   if (actionsEntry == arguments.end()) {
      return ProtocolError{"actions is required"};
   }
   if (!actionsEntry->is_array()) {
      return ProtocolError{"actions must be an array"};
   }

   request.batch.actions.reserve(actionsEntry->size());
   for (std::size_t index = 0; index < actionsEntry->size(); ++index) {
      Result<InputAction, ProtocolError> action =
          parseAction((*actionsEntry)[index]);
      if (!action) {
         return ProtocolError{actionPrefix(index) + action.error().message};
      }
      request.batch.actions.push_back(std::move(*action));
   }

   /*
     Counts, byte limits, modifier placement, and the clicks range are model
     rules, so the parser does not restate them; validateStructure is the one
     authority and its text already names the action index.
   */
   const Result<void, ProtocolError> structure =
       validateStructure(request.batch);
   if (!structure) {
      return structure.error();
   }
   return request;
}

}  // namespace sdl3mcp
