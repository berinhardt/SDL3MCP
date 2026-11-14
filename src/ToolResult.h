#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sdl3mcp {

class Stream;

/*
  One tool answer, as a wire value with no JSON-RPC id: the Session pairs it with
  the order it answers.

  An empty `payload` is a simple MCP content result: `content` is the content
  array and `isError` is the flag. A non-empty `payload` is a raw RGBA frame:
  `content` is the header naming it and the payload is written immediately after
  the header line with no newline and no base64.
*/
struct ToolResult {
   nlohmann::json content;
   std::vector<uint8_t> payload;
   bool isError = false;
   /*
     A JSON-RPC error object relayed unchanged from a peer. When it is an
     object, encode() writes `{"jsonrpc":"2.0","id":id,"error":...}` instead of a
     result, which is how a Handler passes an application's protocol error on
     without reinterpreting it.
   */
   nlohmann::json rpcError;

   bool isRawFrame() const noexcept { return !payload.empty(); }
   bool isRpcError() const noexcept { return rpcError.is_object(); }

   /* A simple result whose content is one text block. */
   static ToolResult text(std::string text, bool isError = false);

   /* A JSON-RPC error object to relay verbatim. */
   static ToolResult error(nlohmann::json errorObject);

   /* A raw frame; byteLength is derived from the payload, never supplied. */
   static ToolResult rawFrame(int width,
                              int height,
                              std::uint64_t frame,
                              std::vector<uint8_t> payload,
                              std::string text = {});

   /*
     Writes the answer for `id`: one content line, or a raw header line followed
     by exactly payload.size() bytes. False on a write failure; the stream owns
     the diagnostic.
   */
   bool encode(Stream& stream, const nlohmann::json& id) const;

   /*
     Validates a raw-frame header and returns the payload size it announces.
     Called before the payload is read, so a mismatched or hostile byteLength is
     rejected without allocating it. A content result is not a header.
   */
   static bool decodeHeader(const nlohmann::json& result,
                            std::size_t& byteLength,
                            std::string& error);

   /*
     Parses a result object received from a peer. For a raw frame the caller has
     already read `payload`, the header's byteLength bytes. False with `error`
     set when the result is neither a content result nor a well-formed raw
     frame header.
   */
   static bool decode(const nlohmann::json& result,
                      std::vector<uint8_t> payload,
                      ToolResult& out,
                      std::string& error);
};

}  // namespace sdl3mcp
