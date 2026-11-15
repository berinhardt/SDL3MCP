#include "InputProperties.h"

#include <cmath>
#include <string>
#include <string_view>

namespace sdl3mcp {

Result<void, ProtocolError> rejectUnknownProperties(
    const nlohmann::json& value, std::span<const std::string_view> allowed) {
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

Result<std::string, ProtocolError> readRequiredString(const nlohmann::json& value,
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

Result<double, ProtocolError> readNumber(const nlohmann::json& value,
                                         const char* name) {
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

Result<double, ProtocolError> readRequiredNumber(const nlohmann::json& value,
                                                 const char* name) {
   if (value.find(name) == value.end()) {
      return ProtocolError{std::string(name) + " is required"};
   }
   return readNumber(value, name);
}

}  // namespace sdl3mcp
