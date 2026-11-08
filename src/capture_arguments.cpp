#include "capture_arguments.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace sdl3mcp
{
namespace
{

constexpr std::array<std::string_view, 4> kRegionProperties{"x", "y", "width", "height"};

template <std::size_t N>
std::optional<std::string> rejectUnknownProperties(const nlohmann::json &value,
                                                   const std::array<std::string_view, N> &allowed)
{
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
std::optional<int64_t> readInteger(const nlohmann::json &object,
                                   const char *name,
                                   std::string &error)
{
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
        const uint64_t value = entry->get<uint64_t>();
        if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            error = std::string(name) + " is out of range";
            return std::nullopt;
        }
        return static_cast<int64_t>(value);
    }
    return entry->get<int64_t>();
}

} // namespace

bool parseRegionArgument(const nlohmann::json &arguments,
                         std::optional<CaptureRegion> &region,
                         std::string &error)
{
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

    const std::optional<int64_t> x = readInteger(*regionEntry, "x", error);
    if (!x.has_value()) {
        return false;
    }
    const std::optional<int64_t> y = readInteger(*regionEntry, "y", error);
    if (!y.has_value()) {
        return false;
    }
    const std::optional<int64_t> width = readInteger(*regionEntry, "width", error);
    if (!width.has_value()) {
        return false;
    }
    const std::optional<int64_t> height = readInteger(*regionEntry, "height", error);
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
    if (*x > std::numeric_limits<int>::max() || *y > std::numeric_limits<int>::max() ||
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

bool parseTimeoutArgument(const nlohmann::json &arguments,
                          std::chrono::milliseconds defaultTimeout,
                          std::chrono::milliseconds &timeout,
                          std::string &error)
{
    timeout = defaultTimeout;
    if (!arguments.contains("timeout_ms")) {
        return true;
    }

    const std::optional<int64_t> value = readInteger(arguments, "timeout_ms", error);
    if (!value.has_value()) {
        return false;
    }
    if (*value < kCaptureMinTimeoutMs || *value > kCaptureMaxTimeoutMs) {
        error = "timeout_ms " + std::to_string(*value) + " must be between " +
                std::to_string(kCaptureMinTimeoutMs) + " and " +
                std::to_string(kCaptureMaxTimeoutMs);
        return false;
    }
    timeout = std::chrono::milliseconds(*value);
    return true;
}

} // namespace sdl3mcp
