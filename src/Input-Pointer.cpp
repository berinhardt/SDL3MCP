#include "Input-Pointer.h"

#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "Input-Properties.h"

namespace sdl3mcp {
namespace {

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

}  // namespace

/*
  The two wire forms are exclusive: x/y are absolute window points, dx/dy are
  relative deltas. A half pair or a mix of both would leave the mode ambiguous,
  so every such shape is refused instead of being guessed at.
*/
Result<InputAction, ProtocolError> parseMouseMove(const nlohmann::json& value) {
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

Result<InputAction, ProtocolError> parseMouseButton(const nlohmann::json& value) {
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

Result<InputAction, ProtocolError> parseScroll(const nlohmann::json& value) {
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

}  // namespace sdl3mcp
