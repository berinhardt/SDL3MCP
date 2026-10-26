/*
  The supervisor executable: parse our own options, then hand stdin/stdout to
  the Supervisor. Everything after --target belongs to the app, and the app's
  stdio is a private pipe pair, so the client keeps talking to this process.
*/

#include "cli.hpp"
#include "supervisor.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include <unistd.h>

int main(int argc, char **argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const sdl3mcp::CliParseResult parsed = sdl3mcp::parseCommandLine(arguments);
    if (!parsed.ok) {
        std::fprintf(stderr, "sdl3mcp-supervisor: %s\n%s\n", parsed.error.c_str(),
                     sdl3mcp::usageLine().c_str());
        return 2;
    }

    sdl3mcp::Supervisor supervisor(STDIN_FILENO, STDOUT_FILENO, parsed.options);
    return supervisor.run();
}
