/*
  The supervisor's downscale is pure pixel math: a value in, value out check of
  the box filter that presentation used to apply inside the library.
*/

#include "scaling.hpp"

#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

} // namespace

int main()
{
    testDownscaleKeepsSolidColor();
    testDownscaleAveragesQuadrants();
    testDownscaleAveragesPixels();
    testDownscaleRoundsOddHeight();
    testDownscaleNeverUpscales();
    testDownscaleAveragesAlpha();
    testDownscaleHeightAtLeastOne();

    std::fprintf(stderr, "supervisor scaling tests passed\n");
    return 0;
}
