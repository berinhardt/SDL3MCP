#include "Input.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <variant>

namespace sdl3mcp {
namespace {

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

}  // namespace sdl3mcp
