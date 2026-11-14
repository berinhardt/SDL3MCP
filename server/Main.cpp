#include <cstdio>
#include <string>
#include <vector>

#include "Cli.h"
#include "Handler.h"

int main(int argc, char** argv) {
   std::vector<std::string> arguments;
   for (int index = 1; index < argc; ++index) {
      if (argv[index] != nullptr) {
         arguments.emplace_back(argv[index]);
      }
   }

   const sdl3mcp::HandlerCliResult parsed =
       sdl3mcp::parseHandlerCommandLine(arguments);
   if (!parsed.ok) {
      std::fprintf(stderr, "sdl3mcp: %s\n%s\n", parsed.error.c_str(),
                   sdl3mcp::handlerUsageText().c_str());
      return 2;
   }
   if (parsed.exit) {
      std::fprintf(stderr, "%s\n", parsed.message.c_str());
      return parsed.exitCode;
   }

   sdl3mcp::Handler handler(parsed.options);
   return handler.run();
}
