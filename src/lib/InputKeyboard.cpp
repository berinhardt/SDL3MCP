#include "InputKeyboard.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "InputProperties.h"

namespace sdl3mcp {
namespace {

using nlohmann::json;

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

}  // namespace

Result<InputAction, ProtocolError> parseKey(const nlohmann::json& value) {
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

}  // namespace sdl3mcp
