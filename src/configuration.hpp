/*
  Internal command-line configuration layer. Not installed and not part of the
  public C API: SDL3MCP_AppInit owns the effectful part (printing messages,
  calling SDL_SetError, starting the server), while this class only derives and
  validates the effective configuration from a base value and argv.
*/

#ifndef SDL3MCP_CONFIGURATION_HPP
#define SDL3MCP_CONFIGURATION_HPP

#include <SDL3MCP/SDL3MCP.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace sdl3mcp
{

/*
  Parses "HOST:PORT" or "[V6]:PORT". An unbracketed host may not contain a
  colon, so an IPv6 literal must be written in brackets; an empty host and a
  port outside 1..65535 are rejected. The host is returned as a view into
  `value` so the caller can copy it into its fixed-size field.

  `allowZeroPort` exists for a listener that asks the kernel for an ephemeral
  port; the application's connect transport keeps the stricter rule.
*/
bool parseEndpoint(std::string_view value,
                   std::string_view &host,
                   std::uint16_t &port,
                   bool allowZeroPort = false);

class Configuration
{
public:
    enum class Status
    {
        Continue,
        ExitSuccess,
        Error
    };

    Configuration() noexcept;
    explicit Configuration(const SDL3MCP_Configuration &source) noexcept;

    /*
      Scans the arguments that begin with "--mcp" and commits the result only
      when the whole command line is accepted. Error and ExitSuccess leave
      value() untouched; message() is reset on every call.
    */
    Status parseCommandLine(int argc, char **argv);
    Status validate();

    const SDL3MCP_Configuration &value() const noexcept { return value_; }
    const std::string &message() const noexcept { return message_; }

    static std::string usageText();
    static std::string versionText();

private:
    Status applyArgument(SDL3MCP_Configuration &candidate, std::string_view argument);
    Status applyTransport(std::string_view argument,
                          std::string_view value,
                          SDL3MCP_Configuration &candidate);
    Status applyFrameTimeout(std::string_view argument,
                             std::string_view value,
                             SDL3MCP_Configuration &candidate);
    Status applyMaxFrameBytes(std::string_view argument,
                              std::string_view value,
                              SDL3MCP_Configuration &candidate);
    Status applyLogLevel(std::string_view argument,
                         std::string_view value,
                         SDL3MCP_Configuration &candidate);
    Status validateValues(const SDL3MCP_Configuration &candidate);
    Status fail(std::string message);

    SDL3MCP_Configuration value_;
    std::string message_;
};

} // namespace sdl3mcp

#endif /* SDL3MCP_CONFIGURATION_HPP */
