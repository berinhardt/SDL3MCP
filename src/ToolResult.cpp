#include "ToolResult.h"

#include <limits>
#include <utility>

#include "JsonRpc.h"
#include "Stream.h"

namespace sdl3mcp {
namespace {

constexpr std::string_view kFrameFormat = "rgba8888";

bool readPositive(const nlohmann::json& value, std::uint64_t& out) {
   if (!value.is_number_integer()) {
      return false;
   }
   if (value.is_number_unsigned()) {
      out = value.get<std::uint64_t>();
      return out > 0;
   }
   const std::int64_t signedValue = value.get<std::int64_t>();
   if (signedValue <= 0) {
      return false;
   }
   out = static_cast<std::uint64_t>(signedValue);
   return true;
}

bool readNonNegative(const nlohmann::json& value, std::uint64_t& out) {
   if (!value.is_number_integer()) {
      return false;
   }
   if (value.is_number_unsigned()) {
      out = value.get<std::uint64_t>();
      return true;
   }
   const std::int64_t signedValue = value.get<std::int64_t>();
   if (signedValue < 0) {
      return false;
   }
   out = static_cast<std::uint64_t>(signedValue);
   return true;
}

}  // namespace

ToolResult ToolResult::text(std::string text, bool isError) {
   ToolResult result;
   result.content =
       nlohmann::json::array({{{"type", "text"}, {"text", std::move(text)}}});
   result.isError = isError;
   return result;
}

ToolResult ToolResult::rawFrame(int width,
                                int height,
                                std::uint64_t frame,
                                std::vector<uint8_t> payload,
                                std::string text) {
   ToolResult result;
   result.content = nlohmann::json{
       {"format", std::string(kFrameFormat)},
       {"width", width},
       {"height", height},
       {"byteLength", payload.size()},
       {"frame", frame},
   };
   if (!text.empty()) {
      result.content["text"] = std::move(text);
   }
   result.payload = std::move(payload);
   return result;
}

bool ToolResult::encode(Stream& stream, const nlohmann::json& id) const {
   if (payload.empty()) {
      return stream.sendText(
          makeResult(id,
                     nlohmann::json{{"content", content}, {"isError", isError}})
              .dump());
   }

   return stream.sendText(makeResult(id, content).dump()) &&
          stream.sendBinary(payload.data(), payload.size());
}

bool ToolResult::decode(const nlohmann::json& result,
                        std::vector<uint8_t> payload,
                        ToolResult& out,
                        std::string& error) {
   error.clear();
   if (!result.is_object()) {
      error = "tool result must be a JSON object";
      return false;
   }

   const auto content = result.find("content");
   if (content != result.end()) {
      if (!content->is_array()) {
         error = "tool result content must be an array";
         return false;
      }
      const auto isError = result.find("isError");
      if (isError != result.end() && !isError->is_boolean()) {
         error = "tool result isError must be a boolean";
         return false;
      }

      out = ToolResult{};
      out.content = *content;
      out.isError = isError != result.end() && isError->get<bool>();
      return true;
   }

   const auto format = result.find("format");
   if (format == result.end()) {
      error = "tool result is neither content nor a raw frame header";
      return false;
   }
   if (!format->is_string() || format->get<std::string>() != kFrameFormat) {
      error = "raw frame format is not rgba8888";
      return false;
   }

   const auto width = result.find("width");
   const auto height = result.find("height");
   std::uint64_t widthValue = 0;
   std::uint64_t heightValue = 0;
   if (width == result.end() || !readPositive(*width, widthValue) ||
       height == result.end() || !readPositive(*height, heightValue)) {
      error = "raw frame dimensions must be positive integers";
      return false;
   }

   const auto byteLength = result.find("byteLength");
   std::uint64_t lengthValue = 0;
   if (byteLength == result.end() ||
       !readNonNegative(*byteLength, lengthValue)) {
      error = "raw frame byteLength must be a non-negative integer";
      return false;
   }

   if (widthValue > std::numeric_limits<std::uint64_t>::max() / heightValue / 4) {
      error = "raw frame dimensions overflow";
      return false;
   }
   const std::uint64_t expected = widthValue * heightValue * 4;
   if (lengthValue != expected) {
      error = "raw frame byteLength " + std::to_string(lengthValue) +
              " does not match " + std::to_string(widthValue) + "x" +
              std::to_string(heightValue);
      return false;
   }
   if (lengthValue != payload.size()) {
      error = "raw frame payload is " + std::to_string(payload.size()) +
              " bytes, expected " + std::to_string(lengthValue);
      return false;
   }

   const auto frame = result.find("frame");
   if (frame != result.end()) {
      std::uint64_t frameValue = 0;
      if (!readNonNegative(*frame, frameValue)) {
         error = "raw frame index must be a non-negative integer";
         return false;
      }
   }

   out = ToolResult{};
   out.content = result;
   out.payload = std::move(payload);
   return true;
}

}  // namespace sdl3mcp
