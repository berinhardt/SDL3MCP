#include "Configuration.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "Configuration-Limits.h"

namespace sdl3mcp {
namespace {

constexpr std::size_t kConnectHostCapacity = 64;

/* -1 for anything that is not a hexadecimal digit. */
int hexDigitValue(char digit) {
   if (digit >= '0' && digit <= '9') {
      return digit - '0';
   }
   if (digit >= 'a' && digit <= 'f') {
      return digit - 'a' + 10;
   }
   if (digit >= 'A' && digit <= 'F') {
      return digit - 'A' + 10;
   }
   return -1;
}

}  // namespace

bool parseDecimal(std::string_view text, std::uint64_t& out) {
   constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();

   if (text.empty()) {
      return false;
   }

   std::uint64_t result = 0;
   for (const char character : text) {
      if (character < '0' || character > '9') {
         return false;
      }
      const auto digit = static_cast<std::uint64_t>(character - '0');
      if (result > (max - digit) / 10u) {
         return false;
      }
      result = result * 10u + digit;
   }

   out = result;
   return true;
}

std::string endpointText(std::string_view host, std::uint16_t port) {
   std::string text;
   if (host.find(':') != std::string_view::npos) {
      text = "[" + std::string(host) + "]";
   } else {
      text = std::string(host);
   }
   text += ":" + std::to_string(port);
   return text;
}

bool parseEndpoint(std::string_view value,
                   std::string_view& host,
                   std::uint16_t& port,
                   bool allowZeroPort) {
   std::string_view portText;
   if (!value.empty() && value.front() == '[') {
      const std::size_t closing = value.find(']');
      if (closing == std::string_view::npos || closing == 1) {
         return false;
      }
      if (closing + 1 >= value.size() || value[closing + 1] != ':') {
         return false;
      }
      host = value.substr(1, closing - 1);
      portText = value.substr(closing + 2);
   } else {
      const std::size_t separator = value.find(':');
      if (separator == std::string_view::npos || separator == 0) {
         return false;
      }
      if (value.find(':', separator + 1) != std::string_view::npos) {
         return false;
      }
      host = value.substr(0, separator);
      portText = value.substr(separator + 1);
   }

   if (host.empty() || host.size() >= kConnectHostCapacity) {
      return false;
   }

   std::uint64_t parsed = 0;
   const std::uint64_t minimum = allowZeroPort ? 0u : 1u;
   if (!parseDecimal(portText, parsed) || parsed < minimum || parsed > 65535) {
      return false;
   }
   port = static_cast<std::uint16_t>(parsed);
   return true;
}

std::string percentDecode(std::string_view encoded) {
   std::string decoded;
   decoded.reserve(encoded.size());
   for (std::size_t index = 0; index < encoded.size(); ++index) {
      if (encoded[index] == '%' && index + 2 < encoded.size()) {
         const int high = hexDigitValue(encoded[index + 1]);
         const int low = hexDigitValue(encoded[index + 2]);
         if (high >= 0 && low >= 0) {
            decoded.push_back(static_cast<char>(high * 16 + low));
            index += 2;
            continue;
         }
      }
      decoded.push_back(encoded[index]);
   }
   return decoded;
}

std::optional<std::string> mcpQueryValue(std::string_view query) {
   if (!query.empty() && query.front() == '?') {
      query.remove_prefix(1);
   }

   while (!query.empty()) {
      const std::size_t separator = query.find('&');
      const std::string_view parameter = query.substr(0, separator);
      query = separator == std::string_view::npos
                  ? std::string_view()
                  : query.substr(separator + 1);

      const std::size_t equals = parameter.find('=');
      if (parameter.substr(0, equals) != "mcp") {
         continue;
      }
      return equals == std::string_view::npos
                 ? std::string()
                 : percentDecode(parameter.substr(equals + 1));
   }
   return std::nullopt;
}

}  // namespace sdl3mcp
