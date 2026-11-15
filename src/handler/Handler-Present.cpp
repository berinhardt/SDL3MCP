#include "Handler.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Png.h"
#include "ToolArguments.h"
#include "ToolResult.h"

namespace sdl3mcp {
namespace {

std::uint64_t frameIndexOf(const nlohmann::json& header) {
   const auto frame = header.find("frame");
   if (frame == header.end() || !frame->is_number_integer()) {
      return 0;
   }
   if (frame->is_number_unsigned()) {
      return frame->get<std::uint64_t>();
   }
   const std::int64_t signedValue = frame->get<std::int64_t>();
   return signedValue > 0 ? static_cast<std::uint64_t>(signedValue) : 0;
}

/*
  The image summary names the frame the app sent, its region when one was given,
  and the PNG size, so a caller can tell a fresh attachment from a stale one.
*/
std::string captureSummary(const nlohmann::json& arguments,
                           int width,
                           int height,
                           std::size_t pngBytes,
                           std::uint64_t frame) {
   std::string text = std::to_string(width) + "x" + std::to_string(height) + " PNG";

   const auto region = arguments.find("region");
   std::int64_t x = 0;
   std::int64_t y = 0;
   if (region != arguments.end() && region->is_object() &&
       readInteger(*region, "x", x) && readInteger(*region, "y", y)) {
      text += " at " + std::to_string(x) + "," + std::to_string(y);
   }

   text += ", " + std::to_string(pngBytes) + " bytes";
   if (frame > 0) {
      text += "; frame " + std::to_string(frame) + " attached";
   }
   return text;
}

}  // namespace

ToolResult Handler::presentFrame(const nlohmann::json& arguments,
                                 const nlohmann::json& header,
                                 std::vector<std::uint8_t> payload) const {
   const int width = header.value("width", 0);
   const int height = header.value("height", 0);

   std::vector<std::uint8_t> png;
   if (!encodePngToVector(payload.data(), width, height, png)) {
      return ToolResult::text("PNG encoding failed for " + std::to_string(width) +
                                  "x" + std::to_string(height),
                              true);
   }

   std::string text = header.value("text", std::string());
   if (text.empty()) {
      text = captureSummary(arguments, width, height, png.size(),
                            frameIndexOf(header));
   }

   ToolResult result;
   result.content = nlohmann::json::array({
       nlohmann::json{{"type", "text"}, {"text", std::move(text)}},
       nlohmann::json{{"type", "image"},
                      {"mimeType", "image/png"},
                      {"data", base64Encode(png)}},
   });
   return result;
}

}  // namespace sdl3mcp
