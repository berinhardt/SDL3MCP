/*
  The Runtime's catalog construction: each application tool definition paired
  with the invoke that answers it. The state and lifecycle are in Runtime.cpp;
  the frame loop is in Runtime-Iterate.cpp.
*/

#include "Runtime.h"

#include <utility>

#include "Capture.h"
#include "InputArguments.h"
#include "Tool.h"
#include "ToolDefinitions.h"
#include "ToolResult.h"

namespace sdl3mcp {

void Runtime::buildCatalog() {
   for (const ToolDefinition& definition : applicationToolDefinitions()) {
      Tool tool{definition.name, definition.description, definition.inputSchema, definition.annotations, {}};
      if (definition.name == "sdl_info") {
         tool.invoke = [this](ToolOrder, ToolCompletionDelegate& completion) {
            completion.complete(ToolResult::text(infoToJson(infoSnapshot()).dump(2)));
         };
      } else if (definition.name == "sdl_input") {
         tool.invoke = [this](ToolOrder order, ToolCompletionDelegate& completion) {
            Result<InputRequest, ProtocolError> request =
                parseInputArguments(order.arguments);
            if (!request) {
               completion.complete(
                   ToolResult::text(std::move(request.error().message), true));
               return;
            }
            pipeline_.registerInput(std::move(request->batch),
                                    std::move(request->region), completion);
         };
      } else if (definition.name == "sdl_capture") {
         tool.invoke = [this](ToolOrder order, ToolCompletionDelegate& completion) {
            Result<CaptureArguments, ProtocolError> arguments =
                parseCaptureArguments(order.arguments);
            if (!arguments) {
               completion.complete(
                   ToolResult::text(std::move(arguments.error().message), true));
               return;
            }
            if (window_ == nullptr) {
               completion.complete(ToolResult::text("no window bound", true));
               return;
            }
            pipeline_.registerCapture(std::move(arguments->region), completion);
         };
      } else if (definition.name == "sdl_terminate") {
         tool.invoke = [this](ToolOrder, ToolCompletionDelegate& completion) {
            terminateRequested_ = true;
            completion.complete(ToolResult::text("termination requested"));
         };
      }
      catalog_.add(std::move(tool));
   }
}

}  // namespace sdl3mcp
