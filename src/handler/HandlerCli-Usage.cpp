#include "HandlerCli.h"

#include <string>

namespace sdl3mcp {

std::string handlerUsageText() {
   return "usage: sdl3mcp [--listen=HOST:PORT] [--connect=HOST:PORT] "
          "[--targets=PATH ...] [--web=PATH ...] [--timeout-ms=N] "
          "[--dry-run=ALIAS] [--dump-tools]\n"
          "  --listen=HOST:PORT   accept one application connection at HOST:PORT;\n"
          "                       optional, and with an app source it defaults to\n"
          "                       127.0.0.1:0, an ephemeral loopback port\n"
          "  --connect=HOST:PORT  dial an upstream MCP client (a Relay)\n"
          "  --targets=PATH       an alias-to-argv app source; a JSON file or a\n"
          "                       directory of *.json files (repeatable)\n"
          "  --web=PATH           an alias-to-web-root app source; a JSON file\n"
          "                       mapping each alias to the root it serves\n"
          "                       (repeatable)\n"
          "  --timeout-ms=N       forward/attach/spawn budget in ms (1..60000,\n"
          "                       default 10000)\n"
          "  --dry-run=ALIAS      print the argv app_spawn_<alias> would run, or\n"
          "                       the web commands/page/bridge/profile, and start\n"
          "                       nothing\n"
          "  --dump-tools         print the tools/list catalog as JSON and exit\n"
          "  --help               print this help and exit\n"
          "  --version            print the Handler version and exit\n"
          "An app source or an explicit --listen is required; --dump-tools needs "
          "neither.";
}

}  // namespace sdl3mcp
