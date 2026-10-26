#include "configuration.hpp"

#include "test_util.hpp"

#include <SDL3MCP/SDL3MCP.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using sdl3mcp::Configuration;

constexpr Configuration::Status kContinue = Configuration::Status::Continue;
constexpr Configuration::Status kExitSuccess = Configuration::Status::ExitSuccess;
constexpr Configuration::Status kError = Configuration::Status::Error;

struct Outcome
{
    Configuration::Status status;
    SDL3MCP_Configuration value;
    std::string message;
};

SDL3MCP_Configuration defaults()
{
    return SDL3MCP_DefaultConfiguration();
}

bool sameConfiguration(const SDL3MCP_Configuration &left, const SDL3MCP_Configuration &right)
{
    return left.transport == right.transport && left.frame_timeout_ms == right.frame_timeout_ms
           && left.max_frame_bytes == right.max_frame_bytes
           && left.warp_cursor == right.warp_cursor && left.log_level == right.log_level;
}

Outcome parse(const SDL3MCP_Configuration &base, const std::vector<std::string> &arguments)
{
    Configuration configuration(base);
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    argv.push_back(const_cast<char *>("sdl3mcp-test"));
    for (const std::string &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }

    const Configuration::Status status =
        configuration.parseCommandLine(static_cast<int>(argv.size()), argv.data());
    return Outcome{status, configuration.value(), configuration.message()};
}

Outcome parse(const std::vector<std::string> &arguments)
{
    return parse(defaults(), arguments);
}

/* Every rejection leaves value() untouched and names the offending flag. */
void expectRejected(const SDL3MCP_Configuration &base,
                    const std::vector<std::string> &arguments,
                    std::string_view named)
{
    const Outcome outcome = parse(base, arguments);
    CHECK(outcome.status == kError);
    CHECK(!outcome.message.empty());
    CHECK(outcome.message.find(named) != std::string::npos);
    CHECK(sameConfiguration(outcome.value, base));
}

void expectDefaultRejected(std::string_view argument, std::string_view named)
{
    expectRejected(defaults(), {std::string(argument)}, named);
}

void testDefaultConstruction()
{
    Configuration configuration;
    CHECK(sameConfiguration(configuration.value(), SDL3MCP_DefaultConfiguration()));
    CHECK(configuration.message().empty());
    CHECK(configuration.validate() == kContinue);
    CHECK(configuration.message().empty());

    SDL3MCP_Configuration base = defaults();
    base.transport = SDL3MCP_TRANSPORT_STDIO;
    base.frame_timeout_ms = 3000;
    base.warp_cursor = false;
    const Configuration copied(base);
    CHECK(sameConfiguration(copied.value(), base));
}

void testNoArguments()
{
    const Outcome noArguments = parse({});
    CHECK(noArguments.status == kContinue);
    CHECK(noArguments.message.empty());
    CHECK(sameConfiguration(noArguments.value, defaults()));

    Configuration zeroArgc;
    CHECK(zeroArgc.parseCommandLine(0, nullptr) == kContinue);
    CHECK(zeroArgc.message().empty());
    CHECK(sameConfiguration(zeroArgc.value(), defaults()));

    Configuration nullArgv;
    CHECK(nullArgv.parseCommandLine(3, nullptr) == kContinue);
    CHECK(nullArgv.message().empty());
    CHECK(sameConfiguration(nullArgv.value(), defaults()));
}

void testTransport()
{
    Outcome outcome = parse({"--mcp"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_STDIO);

    outcome = parse({"--mcp=stdio"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_STDIO);

    outcome = parse({"--mcp=STDIO"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_STDIO);

    outcome = parse({"--mcp=StDiO"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_STDIO);

    SDL3MCP_Configuration stdioBase = defaults();
    stdioBase.transport = SDL3MCP_TRANSPORT_STDIO;
    outcome = parse(stdioBase, {"--mcp=off"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_NONE);

    outcome = parse({"--mcp=off"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_NONE);

    expectDefaultRejected("--mcp=socket", "--mcp=socket");
    expectDefaultRejected("--mcp=", "--mcp");
}

void testBasePreservation()
{
    SDL3MCP_Configuration base = defaults();
    base.frame_timeout_ms = 3000;
    base.warp_cursor = false;
    base.log_level = 4;

    Outcome outcome = parse(base, {});
    CHECK(outcome.status == kContinue);
    CHECK(sameConfiguration(outcome.value, base));

    base.transport = SDL3MCP_TRANSPORT_STDIO;
    outcome = parse(base, {"--verbose", "positional"});
    CHECK(outcome.status == kContinue);
    CHECK(sameConfiguration(outcome.value, base));
}

void testFrameTimeout()
{
    Outcome outcome = parse({"--mcp-timeout-ms=1"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.frame_timeout_ms == 1u);

    outcome = parse({"--mcp-timeout-ms=60000"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.frame_timeout_ms == 60000u);

    expectDefaultRejected("--mcp-timeout-ms=0", "--mcp-timeout-ms");
    expectDefaultRejected("--mcp-timeout-ms=60001", "--mcp-timeout-ms");
    expectDefaultRejected("--mcp-timeout-ms=", "--mcp-timeout-ms");
    expectDefaultRejected("--mcp-timeout-ms=12x", "--mcp-timeout-ms");
    expectDefaultRejected("--mcp-timeout-ms=-1", "--mcp-timeout-ms");
    expectDefaultRejected("--mcp-timeout-ms", "--mcp-timeout-ms");

    /* A command-line override repairs an invalid programmatic value. */
    SDL3MCP_Configuration base = defaults();
    base.frame_timeout_ms = 0;
    outcome = parse(base, {"--mcp-timeout-ms=100"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.frame_timeout_ms == 100u);
}

void testMaxFrameBytes()
{
    Outcome outcome = parse({"--mcp-max-frame-bytes=1048576"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.max_frame_bytes == 1048576u);

    outcome = parse({"--mcp-max-frame-bytes=1073741824"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.max_frame_bytes == 1073741824u);

    expectDefaultRejected("--mcp-max-frame-bytes=1048575", "--mcp-max-frame-bytes");
    expectDefaultRejected("--mcp-max-frame-bytes=1073741825", "--mcp-max-frame-bytes");
    expectDefaultRejected("--mcp-max-frame-bytes=", "--mcp-max-frame-bytes");
    expectDefaultRejected("--mcp-max-frame-bytes=many", "--mcp-max-frame-bytes");
    expectDefaultRejected("--mcp-max-frame-bytes=1048576x", "--mcp-max-frame-bytes");
    expectDefaultRejected("--mcp-max-frame-bytes", "--mcp-max-frame-bytes");
}

void testLogLevel()
{
    const std::vector<std::pair<std::string, int>> levels = {
        {"off", 0}, {"error", 1}, {"warn", 2}, {"info", 3}, {"debug", 4},
    };
    for (const auto &[name, expected] : levels) {
        const Outcome outcome = parse({"--mcp-log-level=" + name});
        CHECK(outcome.status == kContinue);
        CHECK(outcome.value.log_level == expected);
    }

    Outcome outcome = parse({"--mcp-log-level=WaRn"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.log_level == 2);

    outcome = parse({"--mcp-log-level=DEBUG"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.log_level == 4);

    expectDefaultRejected("--mcp-log-level=verbose", "--mcp-log-level");
    expectDefaultRejected("--mcp-log-level=", "--mcp-log-level");
    expectDefaultRejected("--mcp-log-level", "--mcp-log-level");
}

void testWarpCursor()
{
    const Outcome outcome = parse({"--mcp-no-warp-cursor"});
    CHECK(outcome.status == kContinue);
    CHECK(!outcome.value.warp_cursor);
}

void testUnknownArguments()
{
    expectDefaultRejected("--mcp-bogus", "--mcp-bogus");
    expectDefaultRejected("--mcpfoo", "--mcpfoo");
    expectDefaultRejected("--mcp-no-warp-cursor=1", "--mcp-no-warp-cursor=1");
    expectDefaultRejected("--mcpfoo=stdio", "--mcpfoo=stdio");
    expectRejected(defaults(), {"--mcp-timeout-ms=5000", "--mcp-bogus"}, "--mcp-bogus");
}

void testIgnoredArguments()
{
    const Outcome outcome = parse({"--verbose", "-v", "--mc", "positional", "--help"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.message.empty());
    CHECK(sameConfiguration(outcome.value, defaults()));
}

void testHelpAndVersion()
{
    const Outcome help = parse({"--mcp-help"});
    CHECK(help.status == kExitSuccess);
    CHECK(sameConfiguration(help.value, defaults()));
    CHECK(help.message.find("--mcp") != std::string::npos);
    CHECK(help.message.find("--mcp-timeout-ms") != std::string::npos);
    CHECK(help.message.find("--mcp-max-frame-bytes") != std::string::npos);
    CHECK(help.message.find("--mcp-no-warp-cursor") != std::string::npos);
    CHECK(help.message.find("--mcp-log-level") != std::string::npos);
    CHECK(help.message.find("--mcp-version") != std::string::npos);
    CHECK(help.message.find("off") != std::string::npos);

    const Outcome version = parse({"--mcp-version"});
    CHECK(version.status == kExitSuccess);
    CHECK(sameConfiguration(version.value, defaults()));
    CHECK(version.message == std::string("SDL3MCP ") + SDL3MCP_VERSION_STRING);
    CHECK(version.message.find(SDL3MCP_VERSION_STRING) != std::string::npos);
}

void testHelpAndVersionAreTerminal()
{
    Outcome outcome = parse({"--mcp-help", "--mcp-bogus"});
    CHECK(outcome.status == kExitSuccess);

    outcome = parse({"--mcp-version", "--mcp-bogus"});
    CHECK(outcome.status == kExitSuccess);

    outcome = parse({"--mcp-bogus", "--mcp-help"});
    CHECK(outcome.status == kError);
    CHECK(outcome.message.find("--mcp-bogus") != std::string::npos);

    /* Flags parsed before help are not committed. */
    outcome = parse({"--mcp-timeout-ms=1234", "--mcp-help"});
    CHECK(outcome.status == kExitSuccess);
    CHECK(outcome.value.frame_timeout_ms == 2000u);
}

void testDuplicateFlagsLastWins()
{
    Outcome outcome = parse({"--mcp-timeout-ms=100", "--mcp-timeout-ms=200"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.frame_timeout_ms == 200u);

    outcome = parse({"--mcp", "--mcp=off"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_NONE);

    outcome = parse({"--mcp=off", "--mcp"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_STDIO);

    outcome = parse({"--mcp-log-level=info", "--mcp-log-level=debug"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.log_level == 4);
}

void testInvalidArgumentRollsBack()
{
    const SDL3MCP_Configuration base = defaults();
    const std::vector<std::string> arguments = {"--mcp-timeout-ms=3000", "--mcp-bogus"};
    const Outcome outcome = parse(base, arguments);
    CHECK(outcome.status == kError);
    CHECK(sameConfiguration(outcome.value, base));
}

void testProgrammaticValidation()
{
    using MakeInvalid = void (*)(SDL3MCP_Configuration &);
    const MakeInvalid cases[] = {
        [](SDL3MCP_Configuration &base) { base.frame_timeout_ms = 0; },
        [](SDL3MCP_Configuration &base) { base.frame_timeout_ms = 60001; },
        [](SDL3MCP_Configuration &base) { base.max_frame_bytes = 0; },
        [](SDL3MCP_Configuration &base) { base.max_frame_bytes = 1073741825u; },
        [](SDL3MCP_Configuration &base) { base.log_level = 5; },
        [](SDL3MCP_Configuration &base) { base.log_level = -2; },
    };

    for (const MakeInvalid makeInvalid : cases) {
        SDL3MCP_Configuration base = defaults();
        makeInvalid(base);

        Configuration configuration(base);
        CHECK(configuration.parseCommandLine(0, nullptr) == kError);
        CHECK(!configuration.message().empty());
        CHECK(sameConfiguration(configuration.value(), base));

        Configuration direct(base);
        CHECK(direct.validate() == kError);
        CHECK(!direct.message().empty());
    }

    /* validate() clears a stale message once the values are acceptable. */
    Configuration configuration;
    CHECK(configuration.validate() == kContinue);
    CHECK(configuration.message().empty());
}

void testArgvIsNotModified()
{
    std::vector<std::string> arguments = {"--mcp", "--mcp-timeout-ms=2500",
                                          "--mcp-no-warp-cursor", "--verbose"};
    const std::vector<std::string> original = arguments;

    Configuration configuration;
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>("sdl3mcp-test"));
    for (std::string &argument : arguments) {
        argv.push_back(argument.data());
    }

    CHECK(configuration.parseCommandLine(static_cast<int>(argv.size()), argv.data())
          == kContinue);
    CHECK(arguments == original);
    CHECK(configuration.value().transport == SDL3MCP_TRANSPORT_STDIO);
    CHECK(configuration.value().frame_timeout_ms == 2500u);
    CHECK(!configuration.value().warp_cursor);
}

} // namespace

int main()
{
    testDefaultConstruction();
    testNoArguments();
    testTransport();
    testBasePreservation();
    testFrameTimeout();
    testMaxFrameBytes();
    testLogLevel();
    testWarpCursor();
    testUnknownArguments();
    testIgnoredArguments();
    testHelpAndVersion();
    testHelpAndVersionAreTerminal();
    testDuplicateFlagsLastWins();
    testInvalidArgumentRollsBack();
    testProgrammaticValidation();
    testArgvIsNotModified();

    return 0;
}
