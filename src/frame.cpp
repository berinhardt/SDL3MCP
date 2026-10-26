#include "frame.hpp"

#include "png.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sdl3mcp
{

namespace
{

constexpr size_t kChannels = 4;
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

Frame downscaleFrame(const Frame &frame, int maxWidth)
{
    if (!hasConsistentPixels(frame) || maxWidth <= 0 || maxWidth >= frame.width) {
        return frame;
    }

    Frame scaled;
    scaled.width = maxWidth;
    scaled.height = std::max(
        1, static_cast<int>(std::lround(static_cast<double>(frame.height) *
                                        maxWidth / frame.width)));
    scaled.pixels.resize(
        static_cast<size_t>(expectedByteCount(scaled.width, scaled.height)));

    const size_t sourceStride = static_cast<size_t>(frame.width) * kChannels;
    const size_t destinationStride =
        static_cast<size_t>(scaled.width) * kChannels;

    for (int y = 0; y < scaled.height; ++y) {
        const int sourceY0 = static_cast<int>(static_cast<int64_t>(y) *
                                              frame.height / scaled.height);
        const int sourceY1 = std::max(
            sourceY0 + 1,
            static_cast<int>(static_cast<int64_t>(y + 1) * frame.height /
                             scaled.height));
        for (int x = 0; x < scaled.width; ++x) {
            const int sourceX0 = static_cast<int>(static_cast<int64_t>(x) *
                                                  frame.width / scaled.width);
            const int sourceX1 = std::max(
                sourceX0 + 1,
                static_cast<int>(static_cast<int64_t>(x + 1) * frame.width /
                                 scaled.width));

            uint32_t sums[kChannels] = {0, 0, 0, 0};
            for (int sourceY = sourceY0; sourceY < sourceY1; ++sourceY) {
                const uint8_t *row = frame.pixels.data() +
                                     static_cast<size_t>(sourceY) * sourceStride;
                for (int sourceX = sourceX0; sourceX < sourceX1; ++sourceX) {
                    const uint8_t *pixel =
                        row + static_cast<size_t>(sourceX) * kChannels;
                    for (size_t channel = 0; channel < kChannels; ++channel) {
                        sums[channel] += pixel[channel];
                    }
                }
            }

            const uint32_t count = static_cast<uint32_t>(
                (sourceX1 - sourceX0) * (sourceY1 - sourceY0));
            uint8_t *destination =
                scaled.pixels.data() +
                static_cast<size_t>(y) * destinationStride +
                static_cast<size_t>(x) * kChannels;
            for (size_t channel = 0; channel < kChannels; ++channel) {
                destination[channel] = static_cast<uint8_t>(
                    (sums[channel] + count / 2) / count);
            }
        }
    }

    return scaled;
}

std::vector<uint8_t> encodePng(const Frame &frame)
{
    if (!hasConsistentPixels(frame)) {
        return {};
    }

    std::vector<uint8_t> png;
    if (!encodePngToVector(frame.pixels.data(), frame.width, frame.height, png)) {
        return {};
    }
    return png;
}

std::string base64Encode(const uint8_t *data, size_t size)
{
    if (data == nullptr) {
        return {};
    }

    std::string encoded;
    encoded.reserve((size + 2) / 3 * 4);

    size_t index = 0;
    for (; index + 3 <= size; index += 3) {
        const uint32_t group = (static_cast<uint32_t>(data[index]) << 16) |
                               (static_cast<uint32_t>(data[index + 1]) << 8) |
                               static_cast<uint32_t>(data[index + 2]);
        encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(group >> 6) & 0x3F]);
        encoded.push_back(kBase64Alphabet[group & 0x3F]);
    }

    const size_t remaining = size - index;
    if (remaining == 1) {
        const uint32_t group = static_cast<uint32_t>(data[index]) << 16;
        encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
        encoded.append("==");
    } else if (remaining == 2) {
        const uint32_t group = (static_cast<uint32_t>(data[index]) << 16) |
                               (static_cast<uint32_t>(data[index + 1]) << 8);
        encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
        encoded.push_back(kBase64Alphabet[(group >> 6) & 0x3F]);
        encoded.push_back('=');
    }

    return encoded;
}

std::string base64Encode(const std::vector<uint8_t> &data)
{
    return base64Encode(data.data(), data.size());
}

} // namespace sdl3mcp
