/*
  The library's pure frame pipeline is validation and crop only: every rule is a
  value check here, with no window, server, or threads. Downscaling and
  image/base64 encoding are supervisor policy and are covered by the supervisor
  unit tests.
*/

#include "frame.hpp"

#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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
    return 0;
}
