#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "JsonRpc.h"
#include "Stream.h"
#include "Tool.h"
#include "ToolResult.h"

namespace sdl3mcp {

class ToolCatalog;

/*
  One peer connection: it owns its Stream, answers the MCP requests a client
  sends, and turns `tools/call` into a ToolOrder.

  Requests are serialized, as the contract requires. While a tool call is
  pending, later lines wait in a queue and are answered after it completes, so a
  slow call delays every later request instead of interleaving with it. The
  session holds one pending order, and it is the completion delegate for that
  order: complete() pairs the result with the order and writes it, so a tool
  never sees a request id.

  The session is single-threaded. It runs on the transport's thread (the app
  thread in the library, the poll loop in the Handler) and holds no lock.
*/
class Session : public StreamHandler, public ToolCompletionDelegate {
  public:
   Session(ToolCatalog& catalog, std::unique_ptr<Stream> stream);
   ~Session() override;

   Session(const Session&) = delete;
   Session& operator=(const Session&) = delete;

   Stream& stream() noexcept { return *stream_; }
   const Stream& stream() const noexcept { return *stream_; }

   /* Drives the transport's non-blocking read; a no-op where it pushes. */
   void pump();
   /* Closes the transport and drops the pending order and queued lines. */
   void close();

   bool isOpen() const noexcept { return open_; }
   bool initialized() const noexcept { return initialized_; }
   bool hasPendingOrder() const noexcept { return pending_.has_value(); }
   const std::string& lastError() const noexcept { return lastError_; }

   /* StreamHandler */
   void onOpen() override;
   void onMessage(std::string_view line) override;
   void onPayload(std::vector<uint8_t> payload) override;
   void onError(std::string_view error) override;
   void onClose() override;

   /* ToolCompletionDelegate */
   void complete(ToolResult result) override;
   void cancelled() override;

  private:
   void handleLine(std::string_view line);
   void handleRequest(const RpcRequest& request);
   void handleInitialize(const RpcRequest& request);
   void handleToolsList(const RpcRequest& request);
   void handleToolsCall(const RpcRequest& request);
   bool writeResponse(const nlohmann::json& response);
   void fail(const nlohmann::json& id,
             RpcErrorCode code,
             std::string_view message);
   void drainQueue();

   ToolCatalog& catalog_;
   std::unique_ptr<Stream> stream_;
   bool open_ = false;
   bool initialized_ = false;
   std::optional<ToolOrder> pending_;
   std::deque<std::string> queued_;
   std::string lastError_;
};

}  // namespace sdl3mcp
