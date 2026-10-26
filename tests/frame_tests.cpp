/*
  The frame pipeline is pure, so every rule from validation to base64 is a value
  check here: no window, no server, no threads.
*/

#include "frame.hpp"

#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace
{

using sdl3mcp::CaptureRegion;
using sdl3mcp::Frame;

uint8_t *pixelAt(Frame &frame, int x, int y)
{
    const size_t offset =
        (static_cast<size_t>(y) * static_cast<size_t>(frame.width) +
         static_cast<size_t>(x)) * 4;
    return frame.pixels.data() + offset;
}

std::array<uint8_t, 4> pixel(const Frame &frame, int x, int y)
{
    const size_t offset =
        (static_cast<size_t>(y) * static_cast<size_t>(frame.width) +
         static_cast<size_t>(x)) * 4;
    return {frame.pixels[offset], frame.pixels[offset + 1],
            frame.pixels[offset + 2], frame.pixels[offset + 3]};
}

bool isPixel(const Frame &frame,
             int x,
             int y,
             uint8_t red,
             uint8_t green,
             uint8_t blue,
             uint8_t alpha)
{
    return pixel(frame, x, y) == std::array<uint8_t, 4>{red, green, blue, alpha};
}

/* R encodes the pixel's linear index so a wrong row or column is obvious. */
Frame patternedFrame(int width, int height)
{
    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t *out = pixelAt(frame, x, y);
            out[0] = static_cast<uint8_t>(1 + x + y * width);
            out[1] = static_cast<uint8_t>(x * 3);
            out[2] = static_cast<uint8_t>(y * 5);
            out[3] = 255;
        }
    }
    return frame;
}

/* R = x * 10, G = y * 10, so a box filter's average is predictable. */
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

bool hasValue(const std::optional<std::string> &error)
{
    return error.has_value();
}

void testValidateAcceptsExactBytes()
{
    const std::vector<uint8_t> pixels(4 * 5 * 4, 0);
    CHECK(!hasValue(sdl3mcp::validateFrame(pixels.data(), pixels.size(), 4, 5,
                                          33554432u)));
    CHECK(!hasValue(sdl3mcp::validateFrame(pixels.data(), pixels.size(), 4, 5,
                                          80u)));
}

void testValidateRejectsNullAndBadDimensions()
{
    CHECK(hasValue(sdl3mcp::validateFrame(nullptr, 0, 4, 5, 33554432u)));
    CHECK(hasValue(sdl3mcp::validateFrame(nullptr, 80, 4, 5, 33554432u)));

    const std::vector<uint8_t> pixels(80, 0);
    CHECK(hasValue(sdl3mcp::validateFrame(pixels.data(), pixels.size(), 0, 5,
                                          33554432u)));
    CHECK(hasValue(sdl3mcp::validateFrame(pixels.data(), pixels.size(), 4, 0,
                                          33554432u)));
    CHECK(hasValue(sdl3mcp::validateFrame(pixels.data(), pixels.size(), -4, 5,
                                          33554432u)));
}

void testValidateRejectsSizeMismatch()
{
    const std::vector<uint8_t> pixels(81, 0);

    const auto tooSmall =
        sdl3mcp::validateFrame(pixels.data(), 79, 4, 5, 33554432u);
    CHECK(hasValue(tooSmall));
    CHECK(tooSmall->find("size 79 != 4*5*4") != std::string::npos);

    const auto tooLarge =
        sdl3mcp::validateFrame(pixels.data(), 81, 4, 5, 33554432u);
    CHECK(hasValue(tooLarge));
    CHECK(tooLarge->find("size 81 != 4*5*4") != std::string::npos);
}

void testValidateRejectsOversizedFrame()
{
    const std::vector<uint8_t> pixels(4 * 5 * 4, 0);
    const auto error =
        sdl3mcp::validateFrame(pixels.data(), pixels.size(), 4, 5, 79u);
    CHECK(hasValue(error));
    CHECK(error->find("max_frame_bytes") != std::string::npos);
}

void testValidateUsesSixtyFourBitByteCount()
{
    /*
      A 32-bit product of 100000 * 100000 * 4 truncates to 1345294336, which is
      exactly the size offered here. The check must still reject it.
    */
    const std::vector<uint8_t> pixels(1, 0);
    const auto error = sdl3mcp::validateFrame(
        pixels.data(), 1345294336u, 100000, 100000,
        std::numeric_limits<uint32_t>::max());
    CHECK(hasValue(error));
    CHECK(error->find("100000*100000*4") != std::string::npos);

    /* 65536 * 65536 * 4 truncates to zero in 32 bits. */
    CHECK(hasValue(sdl3mcp::validateFrame(pixels.data(), 0, 65536, 65536,
                                          std::numeric_limits<uint32_t>::max())));
}

void testCropCopiesExactPixels()
{
    const Frame frame = patternedFrame(4, 4);
    std::string error;

    const auto cropped =
        sdl3mcp::cropFrame(frame, CaptureRegion{1, 1, 2, 2}, error);
    CHECK(cropped.has_value());
    CHECK(error.empty());
    CHECK(cropped->width == 2);
    CHECK(cropped->height == 2);
    CHECK(cropped->pixels.size() == 2 * 2 * 4);

    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            CHECK(pixel(*cropped, x, y) == pixel(frame, 1 + x, 1 + y));
        }
    }
}

void testCropAcceptsFarEdge()
{
    const Frame frame = patternedFrame(4, 4);
    std::string error;

    const auto corner =
        sdl3mcp::cropFrame(frame, CaptureRegion{2, 2, 2, 2}, error);
    CHECK(corner.has_value());
    CHECK(corner->width == 2);
    CHECK(corner->height == 2);
    CHECK(pixel(*corner, 1, 1) == pixel(frame, 3, 3));

    const auto single =
        sdl3mcp::cropFrame(frame, CaptureRegion{3, 3, 1, 1}, error);
    CHECK(single.has_value());
    CHECK(pixel(*single, 0, 0) == pixel(frame, 3, 3));

    const auto full =
        sdl3mcp::cropFrame(frame, CaptureRegion{0, 0, 4, 4}, error);
    CHECK(full.has_value());
    CHECK(full->pixels == frame.pixels);
}

void expectCropRejected(const Frame &frame, const CaptureRegion &region)
{
    std::string error;
    const auto cropped = sdl3mcp::cropFrame(frame, region, error);
    CHECK(!cropped.has_value());
    CHECK(!error.empty());
}

void testCropRejectsInvalidRegions()
{
    const Frame frame = patternedFrame(4, 4);

    expectCropRejected(frame, CaptureRegion{0, 0, 0, 2});
    expectCropRejected(frame, CaptureRegion{0, 0, 2, 0});
    expectCropRejected(frame, CaptureRegion{0, 0, -1, 2});
    expectCropRejected(frame, CaptureRegion{0, 0, 2, -1});
    expectCropRejected(frame, CaptureRegion{-1, 0, 2, 2});
    expectCropRejected(frame, CaptureRegion{0, -1, 2, 2});
    expectCropRejected(frame, CaptureRegion{3, 0, 2, 2});
    expectCropRejected(frame, CaptureRegion{0, 3, 2, 2});
    expectCropRejected(frame, CaptureRegion{std::numeric_limits<int>::max(), 0, 1,
                                            1});

    /* A zero-sized region is rejected before the frame's own invariant matters. */
    Frame empty;
    expectCropRejected(empty, CaptureRegion{0, 0, 1, 1});
}

void testDownscaleKeepsSolidColor()
{
    Frame solid;
    solid.width = 4;
    solid.height = 4;
    solid.pixels.resize(4 * 4 * 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t *out = pixelAt(solid, x, y);
            out[0] = 10;
            out[1] = 20;
            out[2] = 30;
            out[3] = 40;
        }
    }

    const Frame scaled = sdl3mcp::downscaleFrame(solid, 2);
    CHECK(scaled.width == 2);
    CHECK(scaled.height == 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            CHECK(isPixel(scaled, x, y, 10, 20, 30, 40));
        }
    }
}

void testDownscaleAveragesQuadrants()
{
    Frame quadrants;
    quadrants.width = 4;
    quadrants.height = 4;
    quadrants.pixels.resize(4 * 4 * 4);

    const std::array<std::array<uint8_t, 4>, 4> colors{{
        {10, 0, 0, 255},
        {20, 0, 0, 255},
        {30, 0, 0, 255},
        {40, 0, 0, 255},
    }};
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int quadrant = (y >= 2 ? 2 : 0) + (x >= 2 ? 1 : 0);
            uint8_t *out = pixelAt(quadrants, x, y);
            for (size_t channel = 0; channel < 4; ++channel) {
                out[channel] = colors[static_cast<size_t>(quadrant)][channel];
            }
        }
    }

    const Frame scaled = sdl3mcp::downscaleFrame(quadrants, 2);
    CHECK(scaled.width == 2);
    CHECK(scaled.height == 2);
    CHECK(pixel(scaled, 0, 0) == colors[0]);
    CHECK(pixel(scaled, 1, 0) == colors[1]);
    CHECK(pixel(scaled, 0, 1) == colors[2]);
    CHECK(pixel(scaled, 1, 1) == colors[3]);
}

void testDownscaleAveragesPixels()
{
    const Frame frame = gradientFrame(4, 4);
    const Frame scaled = sdl3mcp::downscaleFrame(frame, 2);
    CHECK(scaled.width == 2);
    CHECK(scaled.height == 2);

    /* Each output pixel covers a 2x2 block, so R and G average 5 apart. */
    CHECK(isPixel(scaled, 0, 0, 5, 5, 0, 255));
    CHECK(isPixel(scaled, 1, 0, 25, 5, 0, 255));
    CHECK(isPixel(scaled, 0, 1, 5, 25, 0, 255));
    CHECK(isPixel(scaled, 1, 1, 25, 25, 0, 255));
}

void testDownscaleRoundsOddHeight()
{
    const Frame frame = gradientFrame(3, 3);
    const Frame scaled = sdl3mcp::downscaleFrame(frame, 2);
    CHECK(scaled.width == 2);
    CHECK(scaled.height == 2);
    CHECK(scaled.pixels.size() == 2 * 2 * 4);
}

void testDownscaleNeverUpscales()
{
    const Frame frame = gradientFrame(4, 4);

    const Frame wider = sdl3mcp::downscaleFrame(frame, 10);
    CHECK(wider.width == frame.width);
    CHECK(wider.height == frame.height);
    CHECK(wider.pixels == frame.pixels);

    const Frame same = sdl3mcp::downscaleFrame(frame, 4);
    CHECK(same.width == frame.width);
    CHECK(same.pixels == frame.pixels);

    const Frame nonPositive = sdl3mcp::downscaleFrame(frame, 0);
    CHECK(nonPositive.width == frame.width);
    CHECK(nonPositive.pixels == frame.pixels);

    /* A malformed frame is passed through untouched rather than read out of bounds. */
    Frame malformed;
    malformed.width = 4;
    malformed.height = 4;
    malformed.pixels.resize(3);
    const Frame untouched = sdl3mcp::downscaleFrame(malformed, 2);
    CHECK(untouched.width == 4);
    CHECK(untouched.pixels.size() == 3);
}

void testDownscaleAveragesAlpha()
{
    Frame frame;
    frame.width = 2;
    frame.height = 1;
    frame.pixels = {10, 20, 30, 0, 40, 50, 60, 200};

    const Frame scaled = sdl3mcp::downscaleFrame(frame, 1);
    CHECK(scaled.width == 1);
    CHECK(scaled.height == 1);
    CHECK(isPixel(scaled, 0, 0, 25, 35, 45, 100));
}

void testDownscaleHeightAtLeastOne()
{
    const Frame frame = gradientFrame(100, 1);
    const Frame scaled = sdl3mcp::downscaleFrame(frame, 10);
    CHECK(scaled.width == 10);
    CHECK(scaled.height == 1);
}

void testCropThenDownscale()
{
    Frame frame;
    frame.width = 16;
    frame.height = 16;
    frame.pixels.resize(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint8_t *out = pixelAt(frame, x, y);
            out[0] = static_cast<uint8_t>(x);
            out[1] = static_cast<uint8_t>(y);
            out[2] = 0;
            out[3] = 255;
        }
    }

    std::string error;
    const auto cropped =
        sdl3mcp::cropFrame(frame, CaptureRegion{0, 0, 8, 8}, error);
    CHECK(cropped.has_value());
    CHECK(cropped->width == 8);
    CHECK(cropped->height == 8);

    const Frame scaled = sdl3mcp::downscaleFrame(*cropped, 4);
    CHECK(scaled.width == 4);
    CHECK(scaled.height == 4);

    /* Each output pixel covers source columns 2x..2x+1 and rows 2y..2y+1. */
    CHECK(isPixel(scaled, 0, 0, 1, 1, 0, 255));
    CHECK(isPixel(scaled, 3, 3, 7, 7, 0, 255));
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
    testValidateAcceptsExactBytes();
    testValidateRejectsNullAndBadDimensions();
    testValidateRejectsSizeMismatch();
    testValidateRejectsOversizedFrame();
    testValidateUsesSixtyFourBitByteCount();

    testCropCopiesExactPixels();
    testCropAcceptsFarEdge();
    testCropRejectsInvalidRegions();

    testDownscaleKeepsSolidColor();
    testDownscaleAveragesQuadrants();
    testDownscaleAveragesPixels();
    testDownscaleRoundsOddHeight();
    testDownscaleNeverUpscales();
    testDownscaleAveragesAlpha();
    testDownscaleHeightAtLeastOne();
    testCropThenDownscale();

    testEncodePngSignatureAndHeader();
    testEncodePngRejectsMalformedFrame();

    testBase64KnownVectors();
    testBase64RoundTrip();
    return 0;
}
