#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "configuration.hpp"

#include <SDL3MCP/SDL3MCP.h>

#include <cstring>
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
           && left.warp_cursor == right.warp_cursor && left.log_level == right.log_level
           && std::strcmp(left.connect_host, right.connect_host) == 0
           && left.connect_port == right.connect_port;
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

TEST_CASE("default construction")
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

TEST_CASE("no arguments")
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

TEST_CASE("transport")
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

TEST_CASE("connect transport")
{
    struct Endpoint
    {
        const char *argument;
        const char *host;
        std::uint16_t port;
    };
    const Endpoint accepted[] = {
        {"--mcp=127.0.0.1:7777", "127.0.0.1", 7777},
        {"--mcp=localhost:7777", "localhost", 7777},
        {"--mcp=[::1]:7777", "::1", 7777},
        {"--mcp=example.test:1", "example.test", 1},
        {"--mcp=example.test:65535", "example.test", 65535},
    };

    for (const Endpoint &endpoint : accepted) {
        const Outcome outcome = parse({endpoint.argument});
        CHECK(outcome.status == kContinue);
        CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_CONNECT);
        CHECK(std::string(outcome.value.connect_host) == endpoint.host);
        CHECK(outcome.value.connect_port == endpoint.port);
    }

    /* A CONNECT base is replaced, not merged, by a later transport flag. */
    Outcome outcome = parse({"--mcp=127.0.0.1:7777", "--mcp=stdio"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.value.transport == SDL3MCP_TRANSPORT_STDIO);

    const char *rejected[] = {
        "--mcp=localhost",
        "--mcp=localhost:",
        "--mcp=localhost:abc",
        "--mcp=localhost:0",
        "--mcp=localhost:70000",
        "--mcp=:7777",
        "--mcp=[]:7777",
        "--mcp=[::1]7777",
        "--mcp=[::1]",
        "--mcp=::1:7777",
        "--mcp=host:1:2",
    };
    for (const char *argument : rejected) {
        expectDefaultRejected(argument, "--mcp");
    }

    /* A host that does not fit the fixed field is rejected at parse time. */
    const std::string longHost = std::string(64, 'a') + ":7777";
    expectDefaultRejected("--mcp=" + longHost, "--mcp");

    /* The defaults leave the connect fields empty. */
    const SDL3MCP_Configuration defaultsValue = defaults();
    CHECK(defaultsValue.connect_host[0] == '\0');
    CHECK(defaultsValue.connect_port == 0);
}

TEST_CASE("base preservation")
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

TEST_CASE("frame timeout")
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

TEST_CASE("max frame bytes")
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

TEST_CASE("log level")
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

TEST_CASE("warp cursor")
{
    const Outcome outcome = parse({"--mcp-no-warp-cursor"});
    CHECK(outcome.status == kContinue);
    CHECK(!outcome.value.warp_cursor);
}

TEST_CASE("unknown arguments")
{
    expectDefaultRejected("--mcp-bogus", "--mcp-bogus");
    expectDefaultRejected("--mcpfoo", "--mcpfoo");
    expectDefaultRejected("--mcp-no-warp-cursor=1", "--mcp-no-warp-cursor=1");
    expectDefaultRejected("--mcpfoo=stdio", "--mcpfoo=stdio");
    expectRejected(defaults(), {"--mcp-timeout-ms=5000", "--mcp-bogus"}, "--mcp-bogus");
}

TEST_CASE("ignored arguments")
{
    const Outcome outcome = parse({"--verbose", "-v", "--mc", "positional", "--help"});
    CHECK(outcome.status == kContinue);
    CHECK(outcome.message.empty());
    CHECK(sameConfiguration(outcome.value, defaults()));
}

TEST_CASE("help and version")
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

TEST_CASE("help and version are terminal")
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

TEST_CASE("duplicate flags last wins")
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

TEST_CASE("invalid argument rolls back")
{
    const SDL3MCP_Configuration base = defaults();
    const std::vector<std::string> arguments = {"--mcp-timeout-ms=3000", "--mcp-bogus"};
    const Outcome outcome = parse(base, arguments);
    CHECK(outcome.status == kError);
    CHECK(sameConfiguration(outcome.value, base));
}

TEST_CASE("programmatic validation")
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

TEST_CASE("argv is not modified")
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
