#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "JsonRpc.h"
#include "Stream.h"
#include "Tool.h"
#include "ToolResult.h"

namespace sdl3mcp {

class ToolCatalog;

/*
  One peer connection: it owns its Stream, answers the MCP requests a client
  sends, and turns `tools/call` into a ToolOrder.

  Requests are handled one at a time on the one thread, but a tool call that
  defers does not block the ones after it: every available line is dispatched in
  the same pump, and each call carries its own completion token. That is what
  lets several sdl_input/sdl_capture calls registered at frame F share the F+1
  frame. The session holds one PendingOrder per in-flight call; each
  OrderCompletion answers its own order, so an out-of-order completion still
  carries the right id. `complete` is idempotent and a late completion after
  close or cancellation is a no-op.

  The session is single-threaded. It runs on the transport's thread (the app
  thread in the library, the poll loop in the Handler) and holds no lock.
*/
class Session : public StreamHandler {
  public:
   Session(ToolCatalog& catalog, std::unique_ptr<Stream> stream);
   ~Session() override;

   Session(const Session&) = delete;
   Session& operator=(const Session&) = delete;

   Stream& stream() noexcept { return *stream_; }
   const Stream& stream() const noexcept { return *stream_; }

   /*
     Drives the transport: the native arm reads what the socket has, and the
     browser arm drains the queue its callbacks filled. Pumping after a close is
     harmless, and on the browser it is how a close or error that arrived while
     the session was already shutting down still reaches the stream.
   */
   void pump();
   /* Closes the transport and drops every pending order. */
   void close();

   bool isOpen() const noexcept { return open_; }
   bool initialized() const noexcept { return initialized_; }
   bool hasPendingOrder() const noexcept { return pendingCount_ > 0; }
   const std::string& lastError() const noexcept { return lastError_; }

   /* StreamHandler */
   void onOpen() override;
   void onMessage(std::string_view line) override;
   void onPayload(std::vector<uint8_t> payload) override;
   void onError(std::string_view error) override;
   void onClose() override;

  private:
   /*
     One order's answer channel. The session owns it through the pending map
     until it completes; a close retires it so a tool that kept the reference
     can still call complete() safely, where it becomes a no-op.
   */
   class OrderCompletion : public ToolCompletionDelegate,
                           public std::enable_shared_from_this<OrderCompletion> {
     public:
      OrderCompletion(Session& session, std::uint64_t token)
          : session_(session), token_(token) {
      }

      void complete(ToolResult result) override;
      void cancelled() override;

     private:
      friend class Session;

      void cancel() noexcept { cancelled_ = true; }

      Session& session_;
      std::uint64_t token_;
      bool cancelled_ = false;
   };

   struct PendingOrder {
      ToolOrder order;
      std::shared_ptr<OrderCompletion> completion;
      /*
         Set once the order has an answer. The entry stays until the next pump so
         an immediate second complete() finds a live object and is a no-op; the
         pump is the point where a tool that kept the reference has answered.
      */
      bool answered = false;
   };

   void handleLine(std::string_view line);
   void handleRequest(const RpcRequest& request);
   void handleInitialize(const RpcRequest& request);
   void handleToolsList(const RpcRequest& request);
   void handleToolsCall(const RpcRequest& request);
   void completeOrder(std::uint64_t token, ToolResult result);
   void cancelOrder(std::uint64_t token);
   void disconnect();
   void discardAnswered();
   bool writeResponse(const nlohmann::json& response);
   void fail(const nlohmann::json& id,
             RpcErrorCode code,
             std::string_view message);

   ToolCatalog& catalog_;
   std::unique_ptr<Stream> stream_;
   bool open_ = false;
   bool initialized_ = false;
   std::map<std::uint64_t, PendingOrder> pending_;
   std::size_t pendingCount_ = 0;
   /*
     Orders retired by a cancellation or close. Their completions stay alive so
     a deferred tool still holding the reference cannot call into a destroyed
     object.
   */
   std::vector<std::shared_ptr<OrderCompletion>> retired_;
   std::uint64_t nextToken_ = 1;
   std::string lastError_;
};

}  // namespace sdl3mcp
