#include "png.hpp"

#include <cstddef>

namespace sdl3mcp
{

namespace
{

constexpr size_t kChannels = 4;
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool hasConsistentPixels(const Frame &frame)
{
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }
    const uint64_t expected = static_cast<uint64_t>(frame.width) *
                              static_cast<uint64_t>(frame.height) * kChannels;
    return frame.pixels.size() == static_cast<size_t>(expected);
}

} // namespace

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
