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
std::optional<std::string> rejectUnknownProperties(
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
         return "unknown property \"" + entry.key() + "\"";
      }
   }
   return std::nullopt;
}

/* Reads one required integer; unsigned JSON values too large for int64_t fail. */
std::optional<std::int64_t> readInteger(const nlohmann::json& object,
                                        const char* name,
                                        std::string& error) {
   const auto entry = object.find(name);
   if (entry == object.end()) {
      error = std::string(name) + " is required";
      return std::nullopt;
   }
   if (!entry->is_number_integer()) {
      error = std::string(name) + " must be an integer";
      return std::nullopt;
   }
   if (entry->is_number_unsigned()) {
      const std::uint64_t value = entry->get<std::uint64_t>();
      if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
         error = std::string(name) + " is out of range";
         return std::nullopt;
      }
      return static_cast<std::int64_t>(value);
   }
   return entry->get<std::int64_t>();
}

}  // namespace

bool parseRegionArgument(const nlohmann::json& arguments,
                         std::optional<CaptureRegion>& region,
                         std::string& error) {
   const auto regionEntry = arguments.find("region");
   if (regionEntry == arguments.end()) {
      return true;
   }
   if (!regionEntry->is_object()) {
      error = "region must be an object";
      return false;
   }
   if (const std::optional<std::string> unknown =
           rejectUnknownProperties(*regionEntry, kRegionProperties);
       unknown.has_value()) {
      error = *unknown;
      return false;
   }

   const std::optional<std::int64_t> x = readInteger(*regionEntry, "x", error);
   if (!x.has_value()) {
      return false;
   }
   const std::optional<std::int64_t> y = readInteger(*regionEntry, "y", error);
   if (!y.has_value()) {
      return false;
   }
   const std::optional<std::int64_t> width =
       readInteger(*regionEntry, "width", error);
   if (!width.has_value()) {
      return false;
   }
   const std::optional<std::int64_t> height =
       readInteger(*regionEntry, "height", error);
   if (!height.has_value()) {
      return false;
   }
   if (*x < 0 || *y < 0) {
      error = "region x and y must be at least 0";
      return false;
   }
   if (*width < 1 || *height < 1) {
      error = "region width and height must be at least 1";
      return false;
   }
   if (*x > std::numeric_limits<int>::max() ||
       *y > std::numeric_limits<int>::max() ||
       *width > std::numeric_limits<int>::max() ||
       *height > std::numeric_limits<int>::max()) {
      error = "region is out of range";
      return false;
   }

   CaptureRegion parsed;
   parsed.x = static_cast<int>(*x);
   parsed.y = static_cast<int>(*y);
   parsed.width = static_cast<int>(*width);
   parsed.height = static_cast<int>(*height);
   region = parsed;
   return true;
}

std::optional<CaptureArguments> parseCaptureArguments(
    const nlohmann::json& arguments, std::string& error) {
   error.clear();

   if (!arguments.is_object()) {
      error = "arguments must be a JSON object";
      return std::nullopt;
   }
   if (const std::optional<std::string> unknown =
           rejectUnknownProperties(arguments, kCaptureProperties);
       unknown.has_value()) {
      error = *unknown;
      return std::nullopt;
   }

   CaptureArguments parsed;
   if (!parseRegionArgument(arguments, parsed.region, error)) {
      return std::nullopt;
   }
   return parsed;
}

}  // namespace sdl3mcp
