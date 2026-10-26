/*
  The supervisor's PNG and base64 helpers, moved out of the library when
  presentation became supervisor policy. These are value checks over a Frame:
  the PNG header must match the frame and base64 must round-trip.
*/

#include "png.hpp"

#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

using sdl3mcp::Frame;

uint8_t *pixelAt(Frame &frame, int x, int y)
{
    const size_t offset =
        (static_cast<size_t>(y) * static_cast<size_t>(frame.width) +
         static_cast<size_t>(x)) * 4;
    return frame.pixels.data() + offset;
}

Frame gradientFrame(int width, int height)
{
    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t *out = pixelAt(frame, x, y);
            out[0] = static_cast<uint8_t>(x * 10);
            out[1] = static_cast<uint8_t>(y * 10);
            out[2] = 0;
            out[3] = 255;
        }
    }
    return frame;
}

uint32_t readBigEndian(const std::vector<uint8_t> &bytes, size_t offset)
{
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

void testEncodePngSignatureAndHeader()
{
    const Frame frame = gradientFrame(4, 5);
    const std::vector<uint8_t> png = sdl3mcp::encodePng(frame);

    CHECK(!png.empty());
    CHECK(png.size() > 33);

    const std::vector<uint8_t> signature{137, 80, 78, 71, 13, 10, 26, 10};
    for (size_t index = 0; index < signature.size(); ++index) {
        CHECK(png[index] == signature[index]);
    }

    CHECK(png[12] == 'I');
    CHECK(png[13] == 'H');
    CHECK(png[14] == 'D');
    CHECK(png[15] == 'R');
    CHECK(readBigEndian(png, 16) == 4);
    CHECK(readBigEndian(png, 20) == 5);
    CHECK(png[24] == 8); /* bit depth */
    CHECK(png[25] == 6); /* colour type: RGBA */
}

void testEncodePngRejectsMalformedFrame()
{
    Frame frame;
    frame.width = 2;
    frame.height = 2;
    frame.pixels.resize(4);
    CHECK(sdl3mcp::encodePng(frame).empty());

    Frame empty;
    CHECK(sdl3mcp::encodePng(empty).empty());
}

int base64Value(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

/* Test-local standard-alphabet decoder, so round trips do not need a dependency. */
std::vector<uint8_t> decodeBase64(const std::string &text)
{
    std::vector<uint8_t> bytes;
    uint32_t buffer = 0;
    int bits = 0;

    for (char character : text) {
        if (character == '=') {
            break;
        }
        const int value = base64Value(character);
        CHECK(value >= 0);
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return bytes;
}

void testBase64KnownVectors()
{
    const std::vector<uint8_t> empty;
    CHECK(sdl3mcp::base64Encode(empty).empty());

    CHECK(sdl3mcp::base64Encode(std::vector<uint8_t>{'f'}) == "Zg==");
    CHECK(sdl3mcp::base64Encode(std::vector<uint8_t>{'f', 'o'}) == "Zm8=");
    CHECK(sdl3mcp::base64Encode(std::vector<uint8_t>{'f', 'o', 'o'}) == "Zm9v");
    CHECK(sdl3mcp::base64Encode(std::vector<uint8_t>{'f', 'o', 'o', 'b', 'a', 'r'}) ==
          "Zm9vYmFy");
    CHECK(sdl3mcp::base64Encode(std::vector<uint8_t>{0x00}) == "AA==");
    CHECK(sdl3mcp::base64Encode(std::vector<uint8_t>{0xFF, 0xFF}) == "//8=");
    CHECK(sdl3mcp::base64Encode(std::vector<uint8_t>{0xFB, 0xFF}) == "+/8=");
}

void testBase64RoundTrip()
{
    std::vector<uint8_t> allBytes;
    allBytes.reserve(256);
    for (int value = 0; value < 256; ++value) {
        allBytes.push_back(static_cast<uint8_t>(value));
    }

    const std::string encoded = sdl3mcp::base64Encode(allBytes);
    CHECK(encoded.size() == 344);
    CHECK(encoded.size() % 4 == 0);
    CHECK(encoded.find('\n') == std::string::npos);
    CHECK(encoded.find('\r') == std::string::npos);
    CHECK(decodeBase64(encoded) == allBytes);

    for (size_t length = 0; length <= 8; ++length) {
        const std::vector<uint8_t> bytes(
            allBytes.begin(),
            allBytes.begin() + static_cast<std::ptrdiff_t>(length));
        const std::string text = sdl3mcp::base64Encode(bytes);
        CHECK(text.size() == (length + 2) / 3 * 4);
        CHECK(decodeBase64(text) == bytes);
    }

    /* A null pointer never reads, and an empty range encodes to nothing. */
    CHECK(sdl3mcp::base64Encode(nullptr, 0).empty());
}

} // namespace

int main()
{
    testEncodePngSignatureAndHeader();
    testEncodePngRejectsMalformedFrame();
    testBase64KnownVectors();
    testBase64RoundTrip();

    std::fprintf(stderr, "supervisor png tests passed\n");
    return 0;
}
