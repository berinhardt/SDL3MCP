/*
  The 1.0 library end to end over its native socket: a test MCP server on a
  loopback port, a separate thread speaking JSON-RPC on it, and the C API on the
  main thread dialing in as an app would. The app uses SDL's dummy video driver,
  so a real window exists with no display.

  The server drives the conversation and checks every answer: initialize,
  tools/list, sdl_info, and sdl_terminate. The app thread only pumps through
  SDL3MCP_AppIterate, exactly as a game loop does, and then observes the one
  SDL_EVENT_QUIT the terminate request produces.

  One lifecycle per process, so this is the only AppInit in this executable.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <SDL3/SDL.h>
#include <SDL3MCP/SDL3MCP.h>
#include <arpa/inet.h>
#include <doctest/doctest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr int kWindowWidth = 64;
constexpr int kWindowHeight = 48;
constexpr std::uint32_t kDefaultMaxFrameBytes = 32u * 1024u * 1024u;

void writeAll(int fd, std::string_view data) {
   std::size_t written = 0;
   while (written < data.size()) {
      const ssize_t count =
          ::write(fd, data.data() + written, data.size() - written);
      if (count < 0 && errno == EINTR) {
         continue;
      }
      CHECK(count > 0);
      if (count <= 0) {
         return;
      }
      written += static_cast<std::size_t>(count);
   }
}

void writeLine(int fd, const nlohmann::json& message) {
   writeAll(fd, message.dump() + "\n");
}

/* Reads newline-delimited messages, buffering bytes past the last newline. */
class LineReader {
  public:
   explicit LineReader(int fd) : fd_(fd) {
   }

   std::string readLine(std::chrono::milliseconds timeout = 10000ms) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      for (;;) {
         const std::size_t newline = pending_.find('\n');
         if (newline != std::string::npos) {
            std::string line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            return line;
         }

         const auto remaining =
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 deadline - std::chrono::steady_clock::now());
         CHECK(remaining > 0ms);
         if (remaining <= 0ms) {
            return {};
         }

         pollfd descriptor{};
         descriptor.fd = fd_;
         descriptor.events = POLLIN;
         const int ready =
             ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
         if (ready < 0 && errno == EINTR) {
            continue;
         }
         CHECK(ready > 0);
         if (ready <= 0) {
            return {};
         }

         char chunk[1024];
         const ssize_t count = ::read(fd_, chunk, sizeof chunk);
         CHECK(count > 0);
         if (count <= 0) {
            return {};
         }
         pending_.append(chunk, static_cast<std::size_t>(count));
      }
   }

  private:
   int fd_ = -1;
   std::string pending_;
};

/*
  The MCP client half: it listens on loopback, accepts the app's one dial, and
  drives the session. `finished` is the app loop's stop condition; `failure`
  carries an exception that escaped the conversation.
*/
class TestServer {
  public:
   TestServer() = default;

   ~TestServer() {
      if (connection_ >= 0) {
         ::close(connection_);
      }
      if (listener_ >= 0) {
         ::close(listener_);
      }
   }

   bool listenOnLoopback() {
      listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
      CHECK(listener_ >= 0);
      if (listener_ < 0) {
         return false;
      }

      int reuse = 1;
      (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                         sizeof reuse);

      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = 0;
      CHECK(::bind(listener_, reinterpret_cast<const sockaddr*>(&address),
                   sizeof address) == 0);
      CHECK(::listen(listener_, 1) == 0);

      socklen_t length = sizeof address;
      CHECK(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                          &length) == 0);
      port = ntohs(address.sin_port);
      CHECK(port != 0);
      return listener_ >= 0 && port != 0;
   }

   void run() {
      try {
         converse();
      } catch (const std::exception& exception) {
         failure = exception.what();
      } catch (...) {
         failure = "unknown exception";
      }
      finished.store(true, std::memory_order_release);
   }

   std::uint16_t port = 0;
   std::atomic<bool> finished{false};
   std::string failure;

  private:
   void converse() {
      pollfd descriptor{};
      descriptor.fd = listener_;
      descriptor.events = POLLIN;
      const int ready = ::poll(&descriptor, 1, 10000);
      CHECK(ready > 0);
      if (ready <= 0) {
         return;
      }

      const int connection = ::accept(listener_, nullptr, nullptr);
      CHECK(connection >= 0);
      if (connection < 0) {
         return;
      }
      connection_ = connection;

      LineReader reader(connection);

      const nlohmann::json initialized = request(
          connection, reader,
          {{"jsonrpc", "2.0"},
           {"id", 1},
           {"method", "initialize"},
           {"params", {{"protocolVersion", "2025-11-25"}}}});
      CHECK(initialized["id"] == 1);
      CHECK(initialized["result"]["protocolVersion"] == "2025-11-25");
      CHECK(initialized["result"]["serverInfo"]["name"] == "sdl3mcp");
      CHECK(initialized["result"]["serverInfo"]["version"] == "1.0.0");

      writeLine(connection,
                {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

      const nlohmann::json listed = request(
          connection, reader, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
      CHECK(listed["id"] == 2);
      std::vector<std::string> names;
      for (const nlohmann::json& tool : listed["result"]["tools"]) {
         names.push_back(tool["name"].get<std::string>());
      }
      CHECK(names.size() == 4);
      CHECK(std::find(names.begin(), names.end(), "sdl_info") != names.end());
      CHECK(std::find(names.begin(), names.end(), "sdl_input") != names.end());
      CHECK(std::find(names.begin(), names.end(), "sdl_capture") != names.end());
      CHECK(std::find(names.begin(), names.end(), "sdl_terminate") !=
            names.end());

      const nlohmann::json info = request(
          connection, reader,
          {{"jsonrpc", "2.0"},
           {"id", 3},
           {"method", "tools/call"},
           {"params", {{"name", "sdl_info"}, {"arguments", nlohmann::json::object()}}}});
      checkInfo(info);

      const nlohmann::json terminated = request(
          connection, reader,
          {{"jsonrpc", "2.0"},
           {"id", 4},
           {"method", "tools/call"},
           {"params", {{"name", "sdl_terminate"}, {"arguments", nlohmann::json::object()}}}});
      CHECK(terminated["id"] == 4);
      CHECK(terminated["result"]["isError"] == false);
      CHECK(terminated["result"]["content"][0]["type"] == "text");
      CHECK(terminated["result"]["content"][0]["text"] ==
            "termination requested");

      ::close(connection_);
      connection_ = -1;
   }

   nlohmann::json request(int connection,
                          LineReader& reader,
                          const nlohmann::json& message) {
      writeLine(connection, message);
      return nlohmann::json::parse(reader.readLine());
   }

   void checkInfo(const nlohmann::json& response) {
      CHECK(response["id"] == 3);
      CHECK(response["result"]["isError"] == false);

      const nlohmann::json& content = response["result"]["content"];
      CHECK(content.is_array());
      CHECK(content.size() == 1);
      CHECK(content[0]["type"] == "text");

      const nlohmann::json document =
          nlohmann::json::parse(content[0]["text"].get<std::string>());
      CHECK(document["sdlVersion"].is_string());
      CHECK(!document["sdlVersion"].get<std::string>().empty());
      CHECK(document["videoDriver"] == "dummy");

      CHECK(document["window"]["title"] == "SDL3MCP socket test");
      CHECK(document["window"]["width"] == kWindowWidth);
      CHECK(document["window"]["height"] == kWindowHeight);
      CHECK(document["window"]["pixelWidth"] > 0);
      CHECK(document["window"]["pixelHeight"] > 0);

      CHECK(document["input"]["heldKeys"].is_array());
      CHECK(document["input"]["heldKeys"].empty());
      CHECK(document["input"]["heldButtons"].is_array());
      CHECK(document["input"]["heldButtons"].empty());

      /* The capture pipeline belongs to the next ticket: zero stats until then. */
      CHECK(document["capture"]["pending"] == false);
      CHECK(document["capture"]["count"] == 0);
      CHECK(document["capture"]["dropped"] == 0);
      CHECK(document["capture"]["lastWidth"] == 0);
      CHECK(document["capture"]["lastHeight"] == 0);
      CHECK(document["capture"]["lastBytes"] == 0);
      CHECK(document["capture"]["lastMs"] == 0.0);
      CHECK(document["capture"]["lastError"].is_null());

      CHECK(document["limits"]["maxFrameBytes"] == kDefaultMaxFrameBytes);
      /* There is no library frame timeout in 1.0. */
      CHECK(!document["limits"].contains("frameTimeoutMs"));

      CHECK(document["frame"] >= 1);
      CHECK(document["appliedFrame"] == 0);
   }

   int listener_ = -1;
   int connection_ = -1;
};

int quitEvents() {
   int count = 0;
   SDL_Event event;
   while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) {
         ++count;
      }
   }
   return count;
}

}  // namespace

TEST_CASE("an app dials a test server and serves the session") {
   (void)SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
   CHECK(SDL_Init(SDL_INIT_VIDEO));
   const char* videoDriver = SDL_GetCurrentVideoDriver();
   CHECK(videoDriver != nullptr);
   CHECK(std::string_view(videoDriver) == "dummy");

   SDL_Window* window =
       SDL_CreateWindow("SDL3MCP socket test", kWindowWidth, kWindowHeight, 0);
   CHECK(window != nullptr);

   TestServer server;
   CHECK(server.listenOnLoopback());
   std::thread serverThread([&server] { server.run(); });

   const std::string endpoint = "--mcp=127.0.0.1:" + std::to_string(server.port);
   char program[] = "library_socket_tests";
   char* argv[] = {program, const_cast<char*>(endpoint.c_str()), nullptr};
   CHECK(SDL3MCP_AppInit(nullptr, 2, argv) == SDL_APP_CONTINUE);
   CHECK(SDL3MCP_SetWindow(window) == SDL_APP_CONTINUE);

   /*
     The app loop is the pump: the server's requests are answered only by an
     iterate, so keep iterating until the conversation ends or the deadline
     proves it never will.
   */
   const auto deadline = std::chrono::steady_clock::now() + 10s;
   while (!server.finished.load(std::memory_order_acquire) &&
          std::chrono::steady_clock::now() < deadline) {
      CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
      std::this_thread::yield();
   }
   CHECK(server.finished.load(std::memory_order_acquire));

   /* The terminate answer may have been flushed just before the quit push. */
   CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
   CHECK(quitEvents() == 1);

   SDL3MCP_AppShutdown();
   SDL3MCP_AppShutdown();

   serverThread.join();
   CHECK(server.failure.empty());

   SDL_DestroyWindow(window);
   SDL_Quit();
}
