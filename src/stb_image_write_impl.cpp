/*
  The one translation unit that compiles stb_image_write.

  STB_IMAGE_WRITE_STATIC makes every stb definition internal to this file, so a
  host application that also vendors stb_image_write links cleanly: there is no
  second definition of stbi_write_png_to_func or the tunables. The pragmas
  silence stb's warnings about the static entry points this wrapper does not
  call; they are unavoidable with STB_IMAGE_WRITE_STATIC and live only here.
*/

#include "png.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace sdl3mcp
{

namespace
{

void appendBytes(void *context, void *data, int size)
{
    auto *out = static_cast<std::vector<uint8_t> *>(context);
    const auto *bytes = static_cast<const uint8_t *>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

bool encodePngToVector(const uint8_t *pixels,
                       int width,
                       int height,
                       std::vector<uint8_t> &out)
{
    out.clear();
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    return stbi_write_png_to_func(appendBytes, &out, width, height, 4,
                                  pixels, width * 4) != 0;
}

} // namespace sdl3mcp
