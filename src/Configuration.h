#pragma once

#include <SDL3MCP/SDL3MCP1.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace sdl3mcp {

/*
  Parses HOST:PORT or [V6]:PORT. `allowZeroPort` exists for the Handler's
  listener, which may bind an ephemeral port; a dial target always needs
  1..65535. A scheme is never part of the grammar: the transport adds it.
*/
bool parseEndpoint(std::string_view value,
                   std::string_view& host,
                   std::uint16_t& port,
                   bool allowZeroPort);

/*
  The effective configuration plus the `--mcp` grammar that produced it.

  The v0 parser carried a transport enum and a frame timeout. Neither exists in
  1.0: the endpoint is the whole transport choice (`off` clears it), and the
  only library timeout is the dial. Every other tuning flag is kept.

  parseCommandLine() works on a copy and commits only after the whole command
  line validates, so a rejected argument leaves value() exactly as it was.
*/
class Configuration {
  public:
   enum class Status {
      Continue,
      ExitSuccess,
      Error
   };

   Configuration() noexcept;
   explicit Configuration(const SDL3MCP_Configuration& source) noexcept;

   /* Reads `--mcp*` arguments only; every other argument is left to the app. */
   Status parseCommandLine(int argc, char** argv);
   /* Validates the values already held, for a programmatic configuration. */
   Status validate();

   const SDL3MCP_Configuration& value() const noexcept { return value_; }
   const std::string& message() const noexcept { return message_; }

   /* An empty connect_host means no session; a set one means one dial. */
   bool hasEndpoint() const noexcept { return value_.connect_host[0] != '\0'; }

   static std::string usageText();
   static std::string versionText();

  private:
   Status applyArgument(SDL3MCP_Configuration& candidate,
                        std::string_view argument);
   Status applyEndpoint(std::string_view argument,
                        std::string_view value,
                        SDL3MCP_Configuration& candidate);
   Status applyMaxFrameBytes(std::string_view argument,
                             std::string_view value,
                             SDL3MCP_Configuration& candidate);
   Status applyLogLevel(std::string_view argument,
                        std::string_view value,
                        SDL3MCP_Configuration& candidate);
   Status validateValues(const SDL3MCP_Configuration& candidate);
   Status fail(std::string message);

   SDL3MCP_Configuration value_;
   std::string message_;
};

}  // namespace sdl3mcp
