/*
  Starts text input for the lifetime of one input batch.

  Synthetic text needs no keyboard focus, so the pipeline starts text input
  while a batch that carries text is applied and stops it again on the way out.
  It stops only what it started, so a window whose text input the application
  had already enabled keeps it; the previous state is restored on every exit
  path, including a failed batch.
*/

#pragma once

struct SDL_Window;

namespace sdl3mcp {

class TextInputScope {
  public:
   TextInputScope(SDL_Window* window, bool needed);
   ~TextInputScope();

   TextInputScope(const TextInputScope&) = delete;
   TextInputScope& operator=(const TextInputScope&) = delete;

  private:
   SDL_Window* window_ = nullptr;
   bool started_ = false;
};

}  // namespace sdl3mcp
