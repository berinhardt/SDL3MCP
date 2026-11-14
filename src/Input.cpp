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

void setError(std::string& error, std::string message) {
   error = std::move(message);
}

/*
  Layout lookups report required modifiers as the generic CTRL/SHIFT/ALT/GUI
  bits. Replay owns physical keys, so fold them onto the left side: '!' becomes
  LSHIFT rather than "either shift".
*/
std::optional<SDL_Keymod> leftSideModifiers(SDL_Keymod required, std::string& error) {
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
std::optional<KeyChord> toChord(const KeyBinding& binding,
                                SDL_Keymod modifiers,
                                std::string& error) {
   const std::optional<SDL_Keymod> required = leftSideModifiers(binding.modifiers, error);
   if (!required.has_value()) {
      return std::nullopt;
   }
   return KeyChord{binding.scancode, static_cast<SDL_Keymod>(*required | modifiers)};
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
std::optional<KeyChord> resolveChord(std::string_view spec,
                                     SDL_Keymod modifiers,
                                     const KeyMap& keymap,
                                     std::string& error) {
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
bool isKnownEnumValue(Enum value, Enum last) noexcept {
   const int raw = static_cast<int>(value);
   return raw >= 0 && raw <= static_cast<int>(last);
}

std::optional<std::string> validateKeyInput(const KeyInput& input) {
   if (!isKnownEnumValue(input.action, KeyAction::Press)) {
      return "unknown key action";
   }
   if (input.key.empty()) {
      return "key must not be empty";
   }
   if (input.key.size() > kMaxKeyBytes) {
      return "key is " + std::to_string(input.key.size()) + " bytes; the maximum is " + std::to_string(kMaxKeyBytes);
   }
   if (input.modifiers.size() > kMaxModifiersPerKey) {
      return "key has " + std::to_string(input.modifiers.size()) + " modifiers; the maximum is " + std::to_string(kMaxModifiersPerKey);
   }
   /* A held key carries no modifier mask, so modifiers cannot mean anything. */
   if (input.action != KeyAction::Press && !input.modifiers.empty()) {
      return "modifiers are only valid for press";
   }
   return std::nullopt;
}

std::optional<std::string> validateTextInput(const TextInput& input) {
   if (input.text.empty()) {
      return "text must not be empty";
   }
   /* The limit is bytes; splitting at UTF-8 boundaries is the replay step's job. */
   if (input.text.size() > kMaxTextBytes) {
      return "text is " + std::to_string(input.text.size()) + " bytes; the maximum is " + std::to_string(kMaxTextBytes);
   }
   return std::nullopt;
}

std::optional<std::string> validateMouseMoveInput(const MouseMoveInput& input) {
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

std::optional<std::string> validateMouseButtonInput(const MouseButtonInput& input) {
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
      return "clicks must be between " + std::to_string(kMinClicks) + " and " + std::to_string(kMaxClicks);
   }
   return std::nullopt;
}

std::optional<std::string> validateScrollInput(const ScrollInput& input) {
   if (!std::isfinite(input.x)) {
      return "scroll x must be finite";
   }
   if (!std::isfinite(input.y)) {
      return "scroll y must be finite";
   }
   return std::nullopt;
}

/* Dispatch without a generic lambda, so each rule stays a named function. */
struct ActionValidator {
   std::optional<std::string> operator()(const KeyInput& input) const {
      return validateKeyInput(input);
   }
   std::optional<std::string> operator()(const TextInput& input) const {
      return validateTextInput(input);
   }
   std::optional<std::string> operator()(const MouseMoveInput& input) const {
      return validateMouseMoveInput(input);
   }
   std::optional<std::string> operator()(const MouseButtonInput& input) const {
      return validateMouseButtonInput(input);
   }
   std::optional<std::string> operator()(const ScrollInput& input) const {
      return validateScrollInput(input);
   }
   std::optional<std::string> operator()(const ReleaseAllInput&) const { return std::nullopt; }
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

std::optional<std::string> validateStructure(const InputBatch& batch) {
   if (batch.actions.empty()) {
      return "the batch must contain at least one action";
   }
   if (batch.actions.size() > kMaxActionsPerBatch) {
      return "the batch has " + std::to_string(batch.actions.size()) + " actions; the maximum is " + std::to_string(kMaxActionsPerBatch);
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
                                   const KeyMap& keymap,
                                   std::string& error) {
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
               "key \"" + std::string(trimmed) + "\" is a chord; chords are only valid for press");
      return std::nullopt;
   }
   setError(error, "unknown key \"" + std::string(trimmed) + "\"");
   return std::nullopt;
}

std::optional<KeyChord> resolveKeyChord(std::string_view spec,
                                        std::span<const Modifier> explicitModifiers,
                                        const KeyMap& keymap,
                                        std::string& error) {
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
std::optional<std::string> rejectUnknownProperties(
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
         return "unknown property \"" + entry.key() + "\"";
      }
   }
   return std::nullopt;
}

std::optional<std::string> readRequiredString(const json& value,
                                              const char* name,
                                              std::string& error) {
   const auto entry = value.find(name);
   if (entry == value.end()) {
      error = std::string(name) + " is required";
      return std::nullopt;
   }
   if (!entry->is_string()) {
      error = std::string(name) + " must be a string";
      return std::nullopt;
   }
   return entry->get<std::string>();
}

/* A finite number as JSON writes it; the cast to float happens at the call site. */
std::optional<double> readNumber(const json& value,
                                 const char* name,
                                 std::string& error) {
   const auto entry = value.find(name);
   if (entry == value.end() || !entry->is_number()) {
      error = std::string(name) + " must be a number";
      return std::nullopt;
   }
   const double number = entry->get<double>();
   if (!std::isfinite(number)) {
      error = std::string(name) + " must be finite";
      return std::nullopt;
   }
   return number;
}

std::optional<double> readRequiredNumber(const json& value,
                                         const char* name,
                                         std::string& error) {
   if (value.find(name) == value.end()) {
      error = std::string(name) + " is required";
      return std::nullopt;
   }
   return readNumber(value, name, error);
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

std::optional<InputAction> parseKey(const json& value, std::string& error) {
   KeyInput input;

   const std::optional<std::string> key = readRequiredString(value, "key", error);
   if (!key.has_value()) {
      return std::nullopt;
   }
   input.key = *key;

   if (const auto entry = value.find("action"); entry != value.end()) {
      if (!entry->is_string()) {
         error = "action must be a string";
         return std::nullopt;
      }
      const std::string name = entry->get<std::string>();
      const std::optional<KeyAction> action = toKeyAction(name);
      if (!action.has_value()) {
         error = "unknown key action \"" + name + "\"";
         return std::nullopt;
      }
      input.action = *action;
   }

   if (const auto entry = value.find("modifiers"); entry != value.end()) {
      if (!entry->is_array()) {
         error = "modifiers must be an array";
         return std::nullopt;
      }
      if (entry->size() > kMaxModifiersPerKey) {
         error = "modifiers has " + std::to_string(entry->size()) +
                 " entries; the maximum is " +
                 std::to_string(kMaxModifiersPerKey);
         return std::nullopt;
      }
      for (const json& item : *entry) {
         if (!item.is_string()) {
            error = "modifiers must be strings";
            return std::nullopt;
         }
         const std::string name = item.get<std::string>();
         const std::optional<Modifier> modifier = parseModifier(name);
         if (!modifier.has_value()) {
            error = "unknown modifier \"" + name + "\"";
            return std::nullopt;
         }
         input.modifiers.push_back(*modifier);
      }
   }

   return InputAction{std::move(input)};
}

std::optional<InputAction> parseText(const json& value, std::string& error) {
   const std::optional<std::string> text = readRequiredString(value, "text", error);
   if (!text.has_value()) {
      return std::nullopt;
   }
   return InputAction{TextInput{*text}};
}

/*
  The two wire forms are exclusive: x/y are absolute window points, dx/dy are
  relative deltas. A half pair or a mix of both would leave the mode ambiguous,
  so every such shape is refused instead of being guessed at.
*/
std::optional<InputAction> parseMouseMove(const json& value, std::string& error) {
   const bool hasX = value.contains("x");
   const bool hasY = value.contains("y");
   const bool hasDx = value.contains("dx");
   const bool hasDy = value.contains("dy");

   if ((hasX && hasDx) || (hasY && hasDy)) {
      error = "provide either x and y or dx and dy, not both";
      return std::nullopt;
   }
   if (hasX != hasY) {
      error = "x and y must be provided together";
      return std::nullopt;
   }
   if (hasDx != hasDy) {
      error = "dx and dy must be provided together";
      return std::nullopt;
   }
   if (!hasX && !hasDx) {
      error = "mouse_move requires x and y or dx and dy";
      return std::nullopt;
   }

   const std::optional<double> x = readNumber(value, hasX ? "x" : "dx", error);
   if (!x.has_value()) {
      return std::nullopt;
   }
   const std::optional<double> y = readNumber(value, hasX ? "y" : "dy", error);
   if (!y.has_value()) {
      return std::nullopt;
   }

   MouseMoveInput input;
   input.mode = hasX ? MouseMoveInput::Mode::Absolute : MouseMoveInput::Mode::Relative;
   input.x = static_cast<float>(*x);
   input.y = static_cast<float>(*y);
   return InputAction{input};
}

std::optional<InputAction> parseMouseButton(const json& value, std::string& error) {
   MouseButtonInput input;

   const std::optional<std::string> button =
       readRequiredString(value, "button", error);
   if (!button.has_value()) {
      return std::nullopt;
   }
   const std::optional<MouseButton> parsedButton = toMouseButton(*button);
   if (!parsedButton.has_value()) {
      error = "unknown mouse button \"" + *button + "\"";
      return std::nullopt;
   }
   input.button = *parsedButton;

   if (const auto entry = value.find("action"); entry != value.end()) {
      if (!entry->is_string()) {
         error = "action must be a string";
         return std::nullopt;
      }
      const std::string name = entry->get<std::string>();
      const std::optional<MouseButtonAction> action = toMouseButtonAction(name);
      if (!action.has_value()) {
         error = "unknown mouse button action \"" + name + "\"";
         return std::nullopt;
      }
      input.action = *action;
   }

   const bool hasX = value.contains("x");
   const bool hasY = value.contains("y");
   if (hasX != hasY) {
      error = "x and y must be provided together";
      return std::nullopt;
   }
   if (hasX) {
      const std::optional<double> x = readNumber(value, "x", error);
      if (!x.has_value()) {
         return std::nullopt;
      }
      const std::optional<double> y = readNumber(value, "y", error);
      if (!y.has_value()) {
         return std::nullopt;
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
         error = "clicks must be an integer";
         return std::nullopt;
      }
      const double clicks = entry->get<double>();
      if (clicks < static_cast<double>(std::numeric_limits<int>::min()) ||
          clicks > static_cast<double>(std::numeric_limits<int>::max())) {
         error = "clicks is out of range";
         return std::nullopt;
      }
      input.clicks = static_cast<int>(clicks);
   }

   return InputAction{input};
}

std::optional<InputAction> parseScroll(const json& value, std::string& error) {
   ScrollInput input;

   const std::optional<double> y = readRequiredNumber(value, "y", error);
   if (!y.has_value()) {
      return std::nullopt;
   }
   input.y = static_cast<float>(*y);

   if (value.contains("x")) {
      const std::optional<double> x = readNumber(value, "x", error);
      if (!x.has_value()) {
         return std::nullopt;
      }
      input.x = static_cast<float>(*x);
   }

   return InputAction{input};
}

std::optional<InputAction> parseReleaseAll(const json&, std::string&) {
   return InputAction{ReleaseAllInput{}};
}

using ActionParser = std::optional<InputAction> (*)(const json& value,
                                                    std::string& error);

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

std::optional<InputAction> parseAction(const json& value, std::string& error) {
   if (!value.is_object()) {
      error = "action must be an object";
      return std::nullopt;
   }

   const auto typeEntry = value.find("type");
   if (typeEntry == value.end()) {
      error = "type is required";
      return std::nullopt;
   }
   if (!typeEntry->is_string()) {
      error = "type must be a string";
      return std::nullopt;
   }
   const std::string_view type = typeEntry->get_ref<const std::string&>();

   for (const ActionType& candidate : kActionTypes) {
      if (candidate.name != type) {
         continue;
      }
      if (const std::optional<std::string> unknown =
              rejectUnknownProperties(value, candidate.properties);
          unknown.has_value()) {
         error = *unknown;
         return std::nullopt;
      }
      return candidate.parse(value, error);
   }

   error = "unknown action type \"" + std::string(type) + "\"";
   return std::nullopt;
}

}  // namespace

std::optional<InputRequest> parseInputArguments(const nlohmann::json& arguments,
                                                std::string& error) {
   error.clear();

   if (!arguments.is_object()) {
      error = "arguments must be a JSON object";
      return std::nullopt;
   }
   if (const std::optional<std::string> unknown =
           rejectUnknownProperties(arguments, kArgumentsProperties);
       unknown.has_value()) {
      error = *unknown;
      return std::nullopt;
   }

   InputRequest request;
   if (!parseRegionArgument(arguments, request.region, error)) {
      return std::nullopt;
   }

   const auto actionsEntry = arguments.find("actions");
   if (actionsEntry == arguments.end()) {
      error = "actions is required";
      return std::nullopt;
   }
   if (!actionsEntry->is_array()) {
      error = "actions must be an array";
      return std::nullopt;
   }

   request.batch.actions.reserve(actionsEntry->size());
   for (std::size_t index = 0; index < actionsEntry->size(); ++index) {
      std::string actionError;
      std::optional<InputAction> action =
          parseAction((*actionsEntry)[index], actionError);
      if (!action.has_value()) {
         error = actionPrefix(index) + actionError;
         return std::nullopt;
      }
      request.batch.actions.push_back(std::move(*action));
   }

   /*
     Counts, byte limits, modifier placement, and the clicks range are model
     rules, so the parser does not restate them; validateStructure is the one
     authority and its text already names the action index.
   */
   if (const std::optional<std::string> structureError =
           validateStructure(request.batch);
       structureError.has_value()) {
      error = *structureError;
      return std::nullopt;
   }
   return request;
}

}  // namespace sdl3mcp
