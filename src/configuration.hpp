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
