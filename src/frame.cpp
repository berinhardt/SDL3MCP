#include "frame.hpp"

#include <cstring>

namespace sdl3mcp
{

namespace
{

constexpr size_t kChannels = 4;

/* Bytes a tightly packed RGBA8888 frame of these dimensions occupies. */
uint64_t expectedByteCount(int width, int height)
{
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * kChannels;
}

bool hasConsistentPixels(const Frame &frame)
{
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }
    return frame.pixels.size() ==
           static_cast<size_t>(expectedByteCount(frame.width, frame.height));
}

} // namespace

std::optional<std::string> validateFrame(const uint8_t *data,
                                         size_t size,
                                         int width,
                                         int height,
                                         uint32_t maxFrameBytes)
{
    if (data == nullptr) {
        return std::string("frame data is null");
    }
    if (width <= 0) {
        return "frame width " + std::to_string(width) + " must be positive";
    }
    if (height <= 0) {
        return "frame height " + std::to_string(height) + " must be positive";
    }

    const uint64_t expected = expectedByteCount(width, height);
    if (static_cast<uint64_t>(size) != expected) {
        return "size " + std::to_string(size) + " != " + std::to_string(width) +
               "*" + std::to_string(height) + "*4";
    }

    if (static_cast<uint64_t>(size) > maxFrameBytes) {
        return "frame size " + std::to_string(size) +
               " exceeds max_frame_bytes " + std::to_string(maxFrameBytes);
    }

    return std::nullopt;
}

std::optional<Frame> cropFrame(const Frame &frame,
                               const CaptureRegion &region,
                               std::string &error)
{
    error.clear();

    if (region.width < 1) {
        error = "crop region width " + std::to_string(region.width) +
                " must be at least 1";
        return std::nullopt;
    }
    if (region.height < 1) {
        error = "crop region height " + std::to_string(region.height) +
                " must be at least 1";
        return std::nullopt;
    }
    if (region.x < 0 || region.y < 0) {
        error = "crop region must not have a negative origin: (" +
                std::to_string(region.x) + ", " + std::to_string(region.y) + ")";
        return std::nullopt;
    }
    if (!hasConsistentPixels(frame)) {
        error = "frame " + std::to_string(frame.width) + "x" +
                std::to_string(frame.height) + " does not match its " +
                std::to_string(frame.pixels.size()) + " pixel bytes";
        return std::nullopt;
    }

    const int64_t right = static_cast<int64_t>(region.x) + region.width;
    const int64_t bottom = static_cast<int64_t>(region.y) + region.height;
    if (right > frame.width || bottom > frame.height) {
        error = "crop region " + std::to_string(region.width) + "x" +
                std::to_string(region.height) + " at (" +
                std::to_string(region.x) + ", " + std::to_string(region.y) +
                ") exceeds frame " + std::to_string(frame.width) + "x" +
                std::to_string(frame.height);
        return std::nullopt;
    }

    Frame cropped;
    cropped.width = region.width;
    cropped.height = region.height;
    cropped.pixels.resize(
        static_cast<size_t>(expectedByteCount(region.width, region.height)));

    const size_t sourceStride = static_cast<size_t>(frame.width) * kChannels;
    const size_t rowBytes = static_cast<size_t>(region.width) * kChannels;
    for (int row = 0; row < region.height; ++row) {
        const uint8_t *source = frame.pixels.data() +
                                static_cast<size_t>(region.y + row) * sourceStride +
                                static_cast<size_t>(region.x) * kChannels;
        uint8_t *destination =
            cropped.pixels.data() + static_cast<size_t>(row) * rowBytes;
        std::memcpy(destination, source, rowBytes);
    }

    return cropped;
}

} // namespace sdl3mcp
