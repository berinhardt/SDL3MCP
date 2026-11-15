#include "Input.h"

#include <string>
#include <string_view>

namespace sdl3mcp {
namespace {

char toAsciiLower(char character) noexcept {
   if (character >= 'A' && character <= 'Z') {
      return static_cast<char>(character - 'A' + 'a');
   }
   return character;
}

}  // namespace

std::optional<Modifier> parseModifier(std::string_view name) {
   std::string normalized;
   normalized.reserve(name.size());
   for (const char character : name) {
      normalized.push_back(toAsciiLower(character));
   }

   if (normalized == "ctrl") {
      return Modifier::Ctrl;
   }
   if (normalized == "shift") {
      return Modifier::Shift;
   }
   if (normalized == "alt") {
      return Modifier::Alt;
   }
   if (normalized == "gui") {
      return Modifier::Gui;
   }
   return std::nullopt;
}

SDL_Keymod modifierBit(Modifier modifier) {
   switch (modifier) {
      case Modifier::Ctrl:
         return SDL_KMOD_LCTRL;
      case Modifier::Shift:
         return SDL_KMOD_LSHIFT;
      case Modifier::Alt:
         return SDL_KMOD_LALT;
      case Modifier::Gui:
         return SDL_KMOD_LGUI;
   }
   return SDL_KMOD_NONE;
}

}  // namespace sdl3mcp
