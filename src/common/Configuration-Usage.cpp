#include "Configuration.h"

#include <string>

namespace sdl3mcp {

/* Desktop only: a browser has no argv, so this text has no reader there. */
#ifndef __EMSCRIPTEN__
std::string Configuration::usageText() {
   return "SDL3MCP command line options:\n"
          "  --mcp=HOST:PORT           Connect to an MCP client at HOST:PORT.\n"
          "  --mcp=[V6]:PORT           Connect to an IPv6 MCP client.\n"
          "  --mcp=off                 Start no MCP session.\n"
          "  --mcp-max-frame-bytes=N   Maximum frame size in bytes (1048576..1073741824).\n"
          "  --mcp-no-warp-cursor      Do not warp the cursor for pointer actions.\n"
          "  --mcp-log-level=LEVEL     off, error, warn, info or debug.\n"
          "  --mcp-help                Print this help and exit successfully.\n"
          "  --mcp-version             Print the library version and exit successfully.\n"
          "The MCP session is off by default; launch with --mcp=HOST:PORT to enable it.";
}

std::string Configuration::versionText() {
   return "SDL3MCP " SDL3MCP_VERSION_STRING;
}
#endif

}  // namespace sdl3mcp
