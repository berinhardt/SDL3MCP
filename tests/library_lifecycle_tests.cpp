/*
  The 1.0 lifecycle, one scenario per process because the C API allows exactly
  one AppInit per process. Every scenario that must observe a failed or repeated
  init therefore runs in its own CTest.

  No SDL window is needed: the lifecycle itself is descriptor-free now that the
  library dials instead of owning stdio. The one socket case binds a loopback
  port without listening, so the kernel refuses the dial deterministically and
  the endpoint error is the one the app sees.
*/

#define DOCTEST_CONFIG_IMPLEMENT
#include <SDL3MCP/SDL3MCP1.h>
#include <arpa/inet.h>
#include <doctest/doctest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

/*
  Binds an IPv4 loopback socket to an ephemeral port and returns the fd. The
  caller decides whether it listens: a bound but silent socket refuses
  connections deterministically, without racing another process for the port.
*/
int bindLoopback(std::uint16_t& port) {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   CHECK(fd >= 0);

   int reuse = 1;
   (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

   sockaddr_in address{};
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = 0;
   CHECK(::bind(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof address) == 0);

   socklen_t length = sizeof address;
   CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
   port = ntohs(address.sin_port);
   CHECK(port != 0);
   return fd;
}

bool hasSdlError() {
   const char* error = SDL_GetError();
   return error != nullptr && error[0] != '\0';
}

/* Defaults plus the given arguments, without touching argv values. */
SDL_AppResult initWithArguments(SDL3MCP_Configuration* configuration,
                                std::string_view argument) {
   char program[] = "library_lifecycle_tests";
   std::string storage(argument);
   char* argv[] = {program, storage.data(), nullptr};
   return SDL3MCP_AppInit(configuration, 2, argv);
}

void testOff() {
   CHECK(initWithArguments(nullptr, "--mcp=off") == SDL_APP_CONTINUE);

   /* No session: iterate is a no-op and no frame can be requested. */
   CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
   CHECK(!SDL3MCP_FrameRequested());
   SDL3MCP_WriteFrame(nullptr, 0, 0, 0);
   CHECK(SDL3MCP_SetWindow(nullptr) == SDL_APP_CONTINUE);

   /* Shutdown is idempotent, and the lifecycle stays claimed. */
   SDL3MCP_AppShutdown();
   SDL3MCP_AppShutdown();
   CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
}

void testNoEndpoint() {
   /* The default configuration names no endpoint, so AppInit starts nothing. */
   CHECK(SDL3MCP_AppInit(nullptr, 0, nullptr) == SDL_APP_CONTINUE);
   CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
   CHECK(!SDL3MCP_FrameRequested());
   SDL3MCP_AppShutdown();
}

void testOffClearsConfiguredEndpoint() {
   /* If off did not clear the endpoint, this dial would be refused. */
   std::uint16_t port = 0;
   const int socketFd = bindLoopback(port);
   CHECK(socketFd >= 0);

   SDL3MCP_Configuration configuration = SDL3MCP_DefaultConfiguration();
   std::strcpy(configuration.connect_host, "127.0.0.1");
   configuration.connect_port = port;
   configuration.connect_timeout_ms = 1000;

   CHECK(initWithArguments(&configuration, "--mcp=off") == SDL_APP_CONTINUE);
   SDL3MCP_AppShutdown();

   ::close(socketFd);
}

void testRefusedEndpoint() {
   std::uint16_t port = 0;
   const int socketFd = bindLoopback(port);
   CHECK(socketFd >= 0);

   SDL_ClearError();
   const std::string argument = "--mcp=127.0.0.1:" + std::to_string(port);
   CHECK(initWithArguments(nullptr, argument) == SDL_APP_FAILURE);
   CHECK(hasSdlError());

   const std::string error = SDL_GetError();
   CHECK(error.find("127.0.0.1") != std::string::npos);
   CHECK(error.find(std::to_string(port)) != std::string::npos);

   /* A failed startup still leaves a safe shutdown, and claims the lifecycle. */
   SDL3MCP_AppShutdown();
   CHECK(SDL3MCP_AppInit(nullptr, 0, nullptr) == SDL_APP_FAILURE);

   ::close(socketFd);
}

void testSecondInit() {
   CHECK(initWithArguments(nullptr, "--mcp=off") == SDL_APP_CONTINUE);
   SDL3MCP_AppShutdown();

   SDL_ClearError();
   CHECK(SDL3MCP_AppInit(nullptr, 0, nullptr) == SDL_APP_FAILURE);
   CHECK(hasSdlError());

   /* Even after shutdown there is no restart path. */
   SDL3MCP_AppShutdown();
   CHECK(SDL3MCP_AppInit(nullptr, 0, nullptr) == SDL_APP_FAILURE);
}

void testUninitialized() {
   SDL_ClearError();
   CHECK(SDL3MCP_SetWindow(nullptr) == SDL_APP_FAILURE);
   CHECK(hasSdlError());

   CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
   CHECK(!SDL3MCP_FrameRequested());
   SDL3MCP_WriteFrame(nullptr, 0, 0, 0);

   SDL3MCP_AppShutdown();
   SDL3MCP_AppShutdown();
}

void testHelp() {
   CHECK(initWithArguments(nullptr, "--mcp-help") == SDL_APP_SUCCESS);
   SDL3MCP_AppShutdown();
}

void testVersion() {
   CHECK(initWithArguments(nullptr, "--mcp-version") == SDL_APP_SUCCESS);
   SDL3MCP_AppShutdown();
}

void testBadArgument() {
   SDL_ClearError();
   CHECK(initWithArguments(nullptr, "--mcp=stdio") == SDL_APP_FAILURE);
   CHECK(hasSdlError());
   CHECK(std::string_view(SDL_GetError()).find("stdio") !=
         std::string_view::npos);
   SDL3MCP_AppShutdown();
}

}  // namespace

/* One scenario per process, selected by the CTest argument, never by state. */
TEST_CASE("off") {
   testOff();
}

TEST_CASE("no_endpoint") {
   testNoEndpoint();
}

TEST_CASE("off_clears_endpoint") {
   testOffClearsConfiguredEndpoint();
}

TEST_CASE("refused_endpoint") {
   testRefusedEndpoint();
}

TEST_CASE("second_init") {
   testSecondInit();
}

TEST_CASE("uninitialized") {
   testUninitialized();
}

TEST_CASE("help") {
   testHelp();
}

TEST_CASE("version") {
   testVersion();
}

TEST_CASE("bad_argument") {
   testBadArgument();
}

int main(int argc, char** argv) {
   /* Doctest owns argv once it runs, so the scenario becomes its filter. */
   doctest::Context context(1, argv);
   context.setAsDefaultForAssertsOutOfTestCases();
   CHECK(argc == 2);

   context.setOption("test-case", argv[1]);
   return context.run();
}
