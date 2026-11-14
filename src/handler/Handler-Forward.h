/*
  The forwarding unit's own declarations: the error a forward without an
  application answers, the grace a call's timeout gets, the poll bound
  while a web stack starts, and the frame text a capture is summarised
  with.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "Handler.h"
#include "ToolArguments.h"

namespace sdl3mcp {

constexpr std::string_view kNoAppError =
    "no application session is attached; call wait_session first";

/*
  A call's own timeout_ms is a budget for the application's answer, so the
  Handler gives it a little slack on top: the application is the authority on
  when it has nothing to send, and the Handler only bounds the wait.
*/
constexpr std::int64_t kForwardGraceMs = 500;

/*
  While a web run is waiting for its page server and bridge, the loop probes
  the two ports on every turn; this bounds how long poll may sleep first.
*/
constexpr int kWebProbePollMs = 25;

inline std::uint64_t frameIndexOf(const nlohmann::json& header) {
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
inline std::string captureSummary(const nlohmann::json& arguments,
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

}  // namespace sdl3mcp
