#include "Png.h"

#include <cstddef>

namespace sdl3mcp {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string base64Encode(const std::uint8_t* data, std::size_t size) {
   if (data == nullptr) {
      return {};
   }

   std::string encoded;
   encoded.reserve((size + 2) / 3 * 4);

   std::size_t index = 0;
   for (; index + 3 <= size; index += 3) {
      const std::uint32_t group = (static_cast<std::uint32_t>(data[index]) << 16) |
                                  (static_cast<std::uint32_t>(data[index + 1]) << 8) |
                                  static_cast<std::uint32_t>(data[index + 2]);
      encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
      encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
      encoded.push_back(kBase64Alphabet[(group >> 6) & 0x3F]);
      encoded.push_back(kBase64Alphabet[group & 0x3F]);
   }

   const std::size_t remaining = size - index;
   if (remaining == 1) {
      const std::uint32_t group = static_cast<std::uint32_t>(data[index]) << 16;
      encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
      encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
      encoded.append("==");
   } else if (remaining == 2) {
      const std::uint32_t group = (static_cast<std::uint32_t>(data[index]) << 16) |
                                  (static_cast<std::uint32_t>(data[index + 1]) << 8);
      encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
      encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
      encoded.push_back(kBase64Alphabet[(group >> 6) & 0x3F]);
      encoded.push_back('=');
   }

   return encoded;
}

std::string base64Encode(const std::vector<std::uint8_t>& data) {
   return base64Encode(data.data(), data.size());
}

}  // namespace sdl3mcp
