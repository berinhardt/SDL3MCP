#include "TextInputScope.h"

#include <SDL3/SDL.h>

namespace sdl3mcp {

TextInputScope::TextInputScope(SDL_Window* window, bool needed)
    : window_(window) {
   if (needed && window_ != nullptr && !SDL_TextInputActive(window_)) {
      started_ = SDL_StartTextInput(window_);
   }
}

TextInputScope::~TextInputScope() {
   if (started_) {
      SDL_StopTextInput(window_);
   }
}

}  // namespace sdl3mcp
