#include "scaling.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

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

} // namespace sdl3mcp
