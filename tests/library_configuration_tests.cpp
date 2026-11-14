/*
  The 1.0 configuration grammar: an optional Session instead of a transport
  enum, and no library frame timeout. The only endpoint forms are `off` and
  `HOST:PORT`; everything else the v0 parser accepted for a transport is gone.

  These cases are pure: the parser is a value in, a value and a message out, so
  no SDL window or socket is involved.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <SDL3MCP/SDL3MCP1.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "Configuration.h"

namespace {

using sdl3mcp::Configuration;

constexpr Configuration::Status kContinue = Configuration::Status::Continue;
constexpr Configuration::Status kExitSuccess =
    Configuration::Status::ExitSuccess;
constexpr Configuration::Status kError = Configuration::Status::Error;

struct Outcome {
   Configuration::Status status;
   SDL3MCP_Configuration value;
   std::string message;
};

SDL3MCP_Configuration defaults() {
   return SDL3MCP_DefaultConfiguration();
}

bool sameConfiguration(const SDL3MCP_Configuration& left,
                       const SDL3MCP_Configuration& right) {
   return left.connect_port == right.connect_port &&
          left.connect_timeout_ms == right.connect_timeout_ms &&
          left.max_frame_bytes == right.max_frame_bytes &&
          left.warp_cursor == right.warp_cursor &&
          left.log_level == right.log_level &&
          std::strcmp(left.connect_host, right.connect_host) == 0;
}

Outcome parse(const SDL3MCP_Configuration& base,
              const std::vector<std::string>& arguments) {
   Configuration configuration(base);
   std::vector<char*> argv;
   argv.reserve(arguments.size() + 1);
   argv.push_back(const_cast<char*>("sdl3mcp-test"));
   for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
   }

   const Configuration::Status status = configuration.parseCommandLine(
       static_cast<int>(argv.size()), argv.data());
   return Outcome{status, configuration.value(), configuration.message()};
}

Outcome parse(const std::vector<std::string>& arguments) {
   return parse(defaults(), arguments);
}

/* Every rejection leaves value() untouched and names the offending flag. */
void expectRejected(const SDL3MCP_Configuration& base,
                    const std::vector<std::string>& arguments,
                    std::string_view named) {
   const Outcome outcome = parse(base, arguments);
   CHECK(outcome.status == kError);
   CHECK(!outcome.message.empty());
   CHECK(outcome.message.find(named) != std::string::npos);
   CHECK(sameConfiguration(outcome.value, base));
}

void expectDefaultRejected(std::string_view argument, std::string_view named) {
   expectRejected(defaults(), {std::string(argument)}, named);
}

TEST_CASE("default construction") {
   Configuration configuration;
   CHECK(sameConfiguration(configuration.value(),
                           SDL3MCP_DefaultConfiguration()));
   CHECK(configuration.message().empty());
   CHECK(configuration.validate() == kContinue);
   CHECK(!configuration.hasEndpoint());

   const SDL3MCP_Configuration value = defaults();
   CHECK(value.connect_host[0] == '\0');
   CHECK(value.connect_port == 0);
   CHECK(value.connect_timeout_ms == 5000u);
   CHECK(value.max_frame_bytes == 32u * 1024u * 1024u);
   CHECK(value.warp_cursor);
   CHECK(value.log_level == -1);

   SDL3MCP_Configuration base = value;
   std::strcpy(base.connect_host, "127.0.0.1");
   base.connect_port = 7777;
   base.connect_timeout_ms = 250;
   base.warp_cursor = false;
   const Configuration copied(base);
   CHECK(sameConfiguration(copied.value(), base));
   CHECK(copied.hasEndpoint());
}

TEST_CASE("no arguments") {
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

TEST_CASE("endpoint") {
   struct Endpoint {
      const char* argument;
      const char* host;
      std::uint16_t port;
   };
   const Endpoint accepted[] = {
       {"--mcp=127.0.0.1:7777", "127.0.0.1", 7777},
       {"--mcp=localhost:7777", "localhost", 7777},
       {"--mcp=[::1]:7777", "::1", 7777},
       {"--mcp=example.test:1", "example.test", 1},
       {"--mcp=example.test:65535", "example.test", 65535},
   };

   for (const Endpoint& endpoint : accepted) {
      const Outcome outcome = parse({endpoint.argument});
      CHECK(outcome.status == kContinue);
      CHECK(outcome.message.empty());
      CHECK(std::string(outcome.value.connect_host) == endpoint.host);
      CHECK(outcome.value.connect_port == endpoint.port);
      /* The endpoint never carries a scheme; the dial adds it. */
      CHECK(std::string(outcome.value.connect_host).find("://") ==
            std::string::npos);
   }

   /* off clears an endpoint the caller configured. */
   SDL3MCP_Configuration base = defaults();
   std::strcpy(base.connect_host, "127.0.0.1");
   base.connect_port = 7777;
   const Outcome off = parse(base, {"--mcp=off"});
   CHECK(off.status == kContinue);
   CHECK(off.value.connect_host[0] == '\0');
   CHECK(off.value.connect_port == 0);

   /* A later endpoint replaces an earlier one. */
   const Outcome replaced =
       parse({"--mcp=127.0.0.1:7777", "--mcp=localhost:9000"});
   CHECK(replaced.status == kContinue);
   CHECK(std::string(replaced.value.connect_host) == "localhost");
   CHECK(replaced.value.connect_port == 9000);

   const char* rejected[] = {
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
       "--mcp=ws://localhost:7777",
       "--mcp=wss://localhost:7777",
       "--mcp=",
   };
   for (const char* argument : rejected) {
      expectDefaultRejected(argument, "--mcp");
   }

   /* A host that does not fit the fixed field is rejected at parse time. */
   const std::string longHost = std::string(64, 'a') + ":7777";
   expectDefaultRejected("--mcp=" + longHost, "--mcp");
}

TEST_CASE("the transport values are gone") {
   /* No stdio server, no bare --mcp: only off and an endpoint are accepted. */
   expectDefaultRejected("--mcp", "--mcp");
   expectDefaultRejected("--mcp=stdio", "--mcp=stdio");
   expectDefaultRejected("--mcp=STDIO", "--mcp=STDIO");
   expectDefaultRejected("--mcp=none", "--mcp=none");
   expectDefaultRejected("--mcp=socket", "--mcp=socket");
}

TEST_CASE("the frame timeout flag is gone") {
   expectDefaultRejected("--mcp-timeout-ms=100", "--mcp-timeout-ms");
   expectDefaultRejected("--mcp-timeout-ms", "--mcp-timeout-ms");
}

TEST_CASE("retained tuning flags") {
   Outcome outcome = parse({"--mcp-max-frame-bytes=1048576"});
   CHECK(outcome.status == kContinue);
   CHECK(outcome.value.max_frame_bytes == 1048576u);

   outcome = parse({"--mcp-max-frame-bytes=1073741824"});
   CHECK(outcome.status == kContinue);
   CHECK(outcome.value.max_frame_bytes == 1073741824u);

   outcome = parse({"--mcp-no-warp-cursor"});
   CHECK(outcome.status == kContinue);
   CHECK(!outcome.value.warp_cursor);

   outcome = parse({"--mcp-log-level=debug"});
   CHECK(outcome.status == kContinue);
   CHECK(outcome.value.log_level == 4);

   expectDefaultRejected("--mcp-max-frame-bytes=1048575",
                         "--mcp-max-frame-bytes");
   expectDefaultRejected("--mcp-max-frame-bytes=1073741825",
                         "--mcp-max-frame-bytes");
   expectDefaultRejected("--mcp-log-level=verbose", "--mcp-log-level");
   expectDefaultRejected("--mcp-no-warp-cursor=1", "--mcp-no-warp-cursor=1");
}

TEST_CASE("help and version") {
   const Outcome help = parse({"--mcp-help"});
   CHECK(help.status == kExitSuccess);
   CHECK(sameConfiguration(help.value, defaults()));
   CHECK(help.message.find("--mcp=HOST:PORT") != std::string::npos);
   CHECK(help.message.find("--mcp=off") != std::string::npos);
   CHECK(help.message.find("--mcp-version") != std::string::npos);

   const Outcome version = parse({"--mcp-version"});
   CHECK(version.status == kExitSuccess);
   CHECK(version.message == std::string("SDL3MCP ") + SDL3MCP_VERSION_STRING);
}

TEST_CASE("invalid argument rolls back") {
   const SDL3MCP_Configuration base = defaults();
   const Outcome outcome =
       parse(base, {"--mcp=127.0.0.1:7777", "--mcp-bogus"});
   CHECK(outcome.status == kError);
   CHECK(sameConfiguration(outcome.value, base));
}

TEST_CASE("programmatic validation") {
   using MakeInvalid = void (*)(SDL3MCP_Configuration&);
   const MakeInvalid cases[] = {
       [](SDL3MCP_Configuration& base) { base.connect_timeout_ms = 0; },
       [](SDL3MCP_Configuration& base) { base.connect_timeout_ms = 60001; },
       [](SDL3MCP_Configuration& base) { base.max_frame_bytes = 0; },
       [](SDL3MCP_Configuration& base) { base.max_frame_bytes = 1073741825u; },
       [](SDL3MCP_Configuration& base) { base.log_level = 5; },
       [](SDL3MCP_Configuration& base) { base.log_level = -2; },
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
}

TEST_CASE("an endpoint needs a host and a port") {
   SDL3MCP_Configuration hostOnly = defaults();
   std::strcpy(hostOnly.connect_host, "127.0.0.1");
   Configuration hostConfiguration(hostOnly);
   CHECK(hostConfiguration.validate() == kError);
   CHECK(hostConfiguration.message().find("connect_port") != std::string::npos);

   SDL3MCP_Configuration portOnly = defaults();
   portOnly.connect_port = 7777;
   Configuration portConfiguration(portOnly);
   CHECK(portConfiguration.validate() == kError);
   CHECK(portConfiguration.message().find("connect_host") != std::string::npos);

   /* An unterminated host field is not a string at all. */
   SDL3MCP_Configuration unterminated = defaults();
   std::memset(unterminated.connect_host, 'a', sizeof unterminated.connect_host);
   unterminated.connect_port = 7777;
   Configuration unterminatedConfiguration(unterminated);
   CHECK(unterminatedConfiguration.validate() == kError);
}

TEST_CASE("argv is not modified") {
   std::vector<std::string> arguments = {"--mcp=127.0.0.1:7777",
                                         "--mcp-no-warp-cursor", "--verbose"};
   const std::vector<std::string> original = arguments;

   Configuration configuration;
   std::vector<char*> argv;
   argv.push_back(const_cast<char*>("sdl3mcp-test"));
   for (std::string& argument : arguments) {
      argv.push_back(argument.data());
   }

   CHECK(configuration.parseCommandLine(static_cast<int>(argv.size()),
                                        argv.data()) == kContinue);
   CHECK(arguments == original);
   CHECK(std::string(configuration.value().connect_host) == "127.0.0.1");
   CHECK(configuration.value().connect_port == 7777);
   CHECK(!configuration.value().warp_cursor);
}

TEST_CASE("endpoint parsing") {
   std::string_view host;
   std::uint16_t port = 0;
   CHECK(sdl3mcp::parseEndpoint("localhost:7777", host, port, false));
   CHECK(host == "localhost");
   CHECK(port == 7777);

   CHECK(sdl3mcp::parseEndpoint("[::1]:7777", host, port, false));
   CHECK(host == "::1");
   CHECK(port == 7777);

   /* The Handler's listener needs port 0, a dial target never does. */
   CHECK(sdl3mcp::parseEndpoint("127.0.0.1:0", host, port, true));
   CHECK(port == 0);
   CHECK(!sdl3mcp::parseEndpoint("127.0.0.1:0", host, port, false));
}

}  // namespace
