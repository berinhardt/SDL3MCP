#include "Input-Actions.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "Input-Keyboard.h"
#include "Input-Pointer.h"
#include "Input-Properties.h"

namespace sdl3mcp {
namespace {

using ActionParser = Result<InputAction, ProtocolError> (*)(const nlohmann::json& value);

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

constexpr std::array<ActionType, 6> kActionTypes{{
    {"key", kKeyProperties, parseKey},
    {"text", kTextProperties, parseText},
    {"mouse_move", kMouseMoveProperties, parseMouseMove},
    {"mouse_button", kMouseButtonProperties, parseMouseButton},
    {"scroll", kScrollProperties, parseScroll},
    {"release_all", kReleaseAllProperties, parseReleaseAll},
}};

}  // namespace

Result<InputAction, ProtocolError> parseText(const nlohmann::json& value) {
   const Result<std::string, ProtocolError> text =
       readRequiredString(value, "text");
   if (!text) {
      return text.error();
   }
   return InputAction{TextInput{*text}};
}

Result<InputAction, ProtocolError> parseReleaseAll(const nlohmann::json&) {
   return InputAction{ReleaseAllInput{}};
}

Result<InputAction, ProtocolError> parseAction(const nlohmann::json& value) {
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

}  // namespace sdl3mcp
