#include "Input-Arguments.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "Capture.h"
#include "Input-Actions.h"
#include "Input-Properties.h"

namespace sdl3mcp {
namespace {

std::string actionPrefix(std::size_t index) {
   return "actions[" + std::to_string(index) + "]: ";
}

constexpr std::array<std::string_view, 2> kArgumentsProperties{"actions",
                                                               "region"};

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
