#include "Capture.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace sdl3mcp {
namespace {

constexpr std::array<std::string_view, 4> kRegionProperties{"x", "y", "width",
                                                            "height"};
constexpr std::array<std::string_view, 1> kCaptureProperties{"region"};

template <std::size_t N>
Result<void, ProtocolError> rejectUnknownProperties(
    const nlohmann::json& value,
    const std::array<std::string_view, N>& allowed) {
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

/* Reads one required integer; unsigned JSON values too large for int64_t fail. */
Result<std::int64_t, ProtocolError> readInteger(const nlohmann::json& object,
                                                const char* name) {
   const auto entry = object.find(name);
   if (entry == object.end()) {
      return ProtocolError{std::string(name) + " is required"};
   }
   if (!entry->is_number_integer()) {
      return ProtocolError{std::string(name) + " must be an integer"};
   }
   if (entry->is_number_unsigned()) {
      const std::uint64_t value = entry->get<std::uint64_t>();
      if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
         return ProtocolError{std::string(name) + " is out of range"};
      }
      return static_cast<std::int64_t>(value);
   }
   return entry->get<std::int64_t>();
}

}  // namespace

Result<void, ProtocolError> parseRegionArgument(
    const nlohmann::json& arguments, std::optional<CaptureRegion>& region) {
   const auto regionEntry = arguments.find("region");
   if (regionEntry == arguments.end()) {
      return {};
   }
   if (!regionEntry->is_object()) {
      return ProtocolError{"region must be an object"};
   }
   const Result<void, ProtocolError> unknown =
       rejectUnknownProperties(*regionEntry, kRegionProperties);
   if (!unknown) {
      return unknown.error();
   }

   const Result<std::int64_t, ProtocolError> x = readInteger(*regionEntry, "x");
   if (!x) {
      return x.error();
   }
   const Result<std::int64_t, ProtocolError> y = readInteger(*regionEntry, "y");
   if (!y) {
      return y.error();
   }
   const Result<std::int64_t, ProtocolError> width =
       readInteger(*regionEntry, "width");
   if (!width) {
      return width.error();
   }
   const Result<std::int64_t, ProtocolError> height =
       readInteger(*regionEntry, "height");
   if (!height) {
      return height.error();
   }
   if (*x < 0 || *y < 0) {
      return ProtocolError{"region x and y must be at least 0"};
   }
   if (*width < 1 || *height < 1) {
      return ProtocolError{"region width and height must be at least 1"};
   }
   if (*x > std::numeric_limits<int>::max() ||
       *y > std::numeric_limits<int>::max() ||
       *width > std::numeric_limits<int>::max() ||
       *height > std::numeric_limits<int>::max()) {
      return ProtocolError{"region is out of range"};
   }

   CaptureRegion parsed;
   parsed.x = static_cast<int>(*x);
   parsed.y = static_cast<int>(*y);
   parsed.width = static_cast<int>(*width);
   parsed.height = static_cast<int>(*height);
   region = parsed;
   return {};
}

Result<CaptureArguments, ProtocolError> parseCaptureArguments(
    const nlohmann::json& arguments) {
   if (!arguments.is_object()) {
      return ProtocolError{"arguments must be a JSON object"};
   }
   const Result<void, ProtocolError> unknown =
       rejectUnknownProperties(arguments, kCaptureProperties);
   if (!unknown) {
      return unknown.error();
   }

   CaptureArguments parsed;
   const Result<void, ProtocolError> region =
       parseRegionArgument(arguments, parsed.region);
   if (!region) {
      return region.error();
   }
   return parsed;
}

}  // namespace sdl3mcp
