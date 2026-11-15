#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

#include "ToolResult.h"

namespace sdl3mcp {

/* A validated tools/call request: the id to answer, the tool, and its arguments. */
struct ToolOrder {
   nlohmann::json id;
   std::string name;
   nlohmann::json arguments;
};

/*
  The one channel a tool uses to answer its order. The Session implements it, so
  a tool never names a stream, a session, or a request id: it completes, and the
  session pairs the result with the order it is holding.

  complete() is idempotent and a no-op after cancelled(). The delegate outlives
  the tool call; a tool that defers keeps the reference only until it answers.
*/
class ToolCompletionDelegate {
  public:
   virtual ~ToolCompletionDelegate() = default;

   virtual void complete(ToolResult result) = 0;
   virtual void cancelled() = 0;
};

/* One MCP tool: its advertised metadata plus the function that produces results. */
struct Tool {
   std::string name;
   std::string description;
   nlohmann::json inputSchema;
   nlohmann::json annotations;
   std::function<void(ToolOrder, ToolCompletionDelegate&)> invoke;
};

}  // namespace sdl3mcp
