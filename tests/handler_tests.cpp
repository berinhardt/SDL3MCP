/*
  The Handler end to end: the real executable over pipes and sockets, driven by
  a test MCP client and a fake application that speaks the library's wire
  protocol. No SDL and no video driver are involved, so a failure is a protocol,
  socket, or Handler bug rather than a driver one.

  Covered: the command line, --listen and --connect, wait_session, the private
  attach handshake, the tool merge, the universal no-app error, one application
  session at a time with a second connection closed, the forwarded call path,
  the raw frame framing turned into a PNG image block, the Handler-owned
  timeout_ms stripped before forwarding, a peer that stops answering, a peer
  that announces a new tool list, and a Relay whose upstream close ends the run.
*/

#define DOCTEST_CONFIG_IMPLEMENT
#include <arpa/inet.h>
#include <dirent.h>
#include <doctest/doctest.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Cli.h"
#include "Png.h"
#include "Targets.h"

namespace {

using namespace std::chrono_literals;

/*
  A test that writes to a peer which has already closed is reporting a protocol
  fact, not a crash: SIGPIPE must not terminate the harness.
*/
struct IgnoreSigpipe {
   IgnoreSigpipe() { ::signal(SIGPIPE, SIG_IGN); }
};
const IgnoreSigpipe g_ignoreSigpipe;

/* The CTest registration supplies the Handler binary as argv[1]. */
std::string g_handler;
/* And the fake application a --targets entry spawns as argv[2]. */
std::string g_fakeApp;

/* A private file the test owns, removed on destruction. */
struct TempPath {
   std::string path;

   TempPath() {
      char buffer[] = "/tmp/sdl3mcp-handler-XXXXXX";
      const int fd = ::mkstemp(buffer);
      CHECK(fd >= 0);
      if (fd >= 0) {
         ::close(fd);
      }
      path = buffer;
   }

   ~TempPath() {
      if (!path.empty()) {
         ::unlink(path.c_str());
      }
   }

   TempPath(const TempPath&) = delete;
   TempPath& operator=(const TempPath&) = delete;
};

void writeFile(const std::string& path, const std::string& contents) {
   std::FILE* file = std::fopen(path.c_str(), "wb");
   REQUIRE(file != nullptr);
   CHECK(std::fwrite(contents.data(), 1, contents.size(), file) == contents.size());
   CHECK(std::fclose(file) == 0);
}

std::string readFile(const std::string& path) {
   std::FILE* file = std::fopen(path.c_str(), "rb");
   REQUIRE(file != nullptr);
   std::string contents;
   char chunk[256];
   std::size_t count = 0;
   while ((count = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
      contents.append(chunk, count);
   }
   std::fclose(file);
   return contents;
}

/* A private directory emptied and removed on destruction. */
struct TempDir {
   std::string path;

   TempDir() {
      char buffer[] = "/tmp/sdl3mcp-targets-XXXXXX";
      const char* created = ::mkdtemp(buffer);
      REQUIRE(created != nullptr);
      path = created;
   }

   ~TempDir() {
      if (path.empty()) {
         return;
      }
      if (DIR* directory = ::opendir(path.c_str())) {
         while (const dirent* entry = ::readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") {
               ::unlink((path + "/" + name).c_str());
            }
         }
         ::closedir(directory);
      }
      ::rmdir(path.c_str());
   }

   TempDir(const TempDir&) = delete;
   TempDir& operator=(const TempDir&) = delete;

   std::string file(const std::string& name) const { return path + "/" + name; }
};

constexpr int kLineTimeoutMs = 5000;

std::string endpointText(const std::string& host, std::uint16_t port) {
   return host + ":" + std::to_string(port);
}

/* One fd pair with a line buffer, enough for both a pipe and a socket. */
struct FdChannel {
   int readFd = -1;
   int writeFd = -1;
   std::string buffer;

   bool readLine(std::string& line, int timeoutMs = kLineTimeoutMs) {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
      for (;;) {
         const std::size_t newline = buffer.find('\n');
         if (newline != std::string::npos) {
            line.assign(buffer, 0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
               line.pop_back();
            }
            return true;
         }

         const auto remaining =
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 deadline - std::chrono::steady_clock::now())
                 .count();
         if (remaining <= 0) {
            return false;
         }

         pollfd descriptor{};
         descriptor.fd = readFd;
         descriptor.events = POLLIN;
         const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
         if (ready < 0) {
            if (errno == EINTR) {
               continue;
            }
            return false;
         }
         if (ready == 0) {
            continue;
         }

         char chunk[4096];
         const ssize_t count = ::read(readFd, chunk, sizeof chunk);
         if (count <= 0) {
            return false;
         }
         buffer.append(chunk, static_cast<std::size_t>(count));
      }
   }

   bool readBytes(std::size_t count,
                  std::vector<std::uint8_t>& out,
                  int timeoutMs = kLineTimeoutMs) {
      out.clear();
      out.reserve(count);
      const std::size_t buffered = std::min(count, buffer.size());
      out.insert(out.end(), buffer.begin(),
                 buffer.begin() + static_cast<std::ptrdiff_t>(buffered));
      buffer.erase(0, buffered);

      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
      while (out.size() < count) {
         const auto remaining =
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 deadline - std::chrono::steady_clock::now())
                 .count();
         if (remaining <= 0) {
            return false;
         }

         pollfd descriptor{};
         descriptor.fd = readFd;
         descriptor.events = POLLIN;
         const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
         if (ready < 0) {
            if (errno == EINTR) {
               continue;
            }
            return false;
         }
         if (ready == 0) {
            continue;
         }

         std::uint8_t chunk[4096];
         const std::size_t wanted = std::min(count - out.size(), sizeof chunk);
         const ssize_t received = ::read(readFd, chunk, wanted);
         if (received <= 0) {
            return false;
         }
         out.insert(out.end(), chunk, chunk + received);
      }
      return true;
   }

   bool writeAll(const void* data, std::size_t size) {
      const char* bytes = static_cast<const char*>(data);
      std::size_t offset = 0;
      while (offset < size) {
         const ssize_t written = ::write(writeFd, bytes + offset, size - offset);
         if (written < 0) {
            if (errno == EINTR) {
               continue;
            }
            return false;
         }
         if (written == 0) {
            return false;
         }
         offset += static_cast<std::size_t>(written);
      }
      return true;
   }

   bool writeLine(std::string_view line) {
      return writeAll(line.data(), line.size()) &&
             writeAll("\n", 1);
   }
};

/* A forked Handler with stdin/stdout/stderr pipes. */
struct TestProcess {
   pid_t pid = -1;
   int toChild = -1;
   int fromChild = -1;
   int stderrChild = -1;
   int exitCode = -1;

   TestProcess() = default;
   TestProcess(const TestProcess&) = delete;
   TestProcess& operator=(const TestProcess&) = delete;

   ~TestProcess() {
      if (toChild >= 0) {
         ::close(toChild);
      }
      if (fromChild >= 0) {
         ::close(fromChild);
      }
      if (stderrChild >= 0) {
         ::close(stderrChild);
      }
      if (pid > 0) {
         ::kill(pid, SIGKILL);
         int status = 0;
         while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
         }
      }
   }

   void start(const std::string& executable,
              const std::vector<std::string>& arguments) {
      int inputPipe[2] = {-1, -1};
      int outputPipe[2] = {-1, -1};
      int errorPipe[2] = {-1, -1};
      CHECK(::pipe(inputPipe) == 0);
      CHECK(::pipe(outputPipe) == 0);
      CHECK(::pipe(errorPipe) == 0);

      const pid_t child = ::fork();
      CHECK(child >= 0);
      if (child == 0) {
         ::dup2(inputPipe[0], STDIN_FILENO);
         ::dup2(outputPipe[1], STDOUT_FILENO);
         ::dup2(errorPipe[1], STDERR_FILENO);
         ::close(inputPipe[0]);
         ::close(inputPipe[1]);
         ::close(outputPipe[0]);
         ::close(outputPipe[1]);
         ::close(errorPipe[0]);
         ::close(errorPipe[1]);

         std::vector<std::string> storage;
         storage.push_back(executable);
         for (const std::string& argument : arguments) {
            storage.push_back(argument);
         }
         std::vector<char*> argv;
         for (std::string& argument : storage) {
            argv.push_back(const_cast<char*>(argument.c_str()));
         }
         argv.push_back(nullptr);
         ::execv(executable.c_str(), argv.data());
         ::_exit(127);
      }

      pid = child;
      ::close(inputPipe[0]);
      ::close(outputPipe[1]);
      ::close(errorPipe[1]);
      toChild = inputPipe[1];
      fromChild = outputPipe[0];
      stderrChild = errorPipe[0];
   }

   std::string readStderrLine(int timeoutMs) {
      std::string line;
      FdChannel channel;
      channel.readFd = stderrChild;
      if (!channel.readLine(line, timeoutMs)) {
         return {};
      }
      return line;
   }

   /* Reads the Handler's `listening on` line and returns the bound port. */
   std::uint16_t readListeningPort(int timeoutMs = kLineTimeoutMs) {
      const std::string line = readStderrLine(timeoutMs);
      CHECK(line.rfind("sdl3mcp: listening on ", 0) == 0);
      const std::size_t separator = line.rfind(':');
      CHECK(separator != std::string::npos);
      const long parsed = std::strtol(line.c_str() + separator + 1, nullptr, 10);
      CHECK((parsed > 0 && parsed <= 65535));
      return static_cast<std::uint16_t>(parsed);
   }

   void closeStdin() {
      if (toChild >= 0) {
         ::close(toChild);
         toChild = -1;
      }
   }

   bool waitForExit(int timeoutMs) {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
      for (;;) {
         int status = 0;
         const pid_t result = ::waitpid(pid, &status, WNOHANG);
         if (result == pid || (result < 0 && errno == ECHILD)) {
            pid = -1;
            if (WIFEXITED(status)) {
               exitCode = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
               exitCode = 128 + WTERMSIG(status);
            }
            return true;
         }
         if (std::chrono::steady_clock::now() >= deadline) {
            return false;
         }
         std::this_thread::sleep_for(5ms);
      }
   }
};

/* Reads a descriptor to EOF, bounded by timeoutMs, and returns every byte. */
std::string readAllFd(int fd, int timeoutMs) {
   std::string out;
   const auto deadline =
       std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
   for (;;) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now())
                                 .count();
      if (remaining <= 0) {
         return out;
      }

      pollfd descriptor{};
      descriptor.fd = fd;
      descriptor.events = POLLIN;
      const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
      if (ready < 0) {
         if (errno == EINTR) {
            continue;
         }
         return out;
      }
      if (ready == 0) {
         return out;
      }

      char chunk[4096];
      const ssize_t count = ::read(fd, chunk, sizeof chunk);
      if (count <= 0) {
         return out;
      }
      out.append(chunk, static_cast<std::size_t>(count));
   }
}

bool connectTcp(const std::string& host, std::uint16_t port, int& fd) {
   addrinfo hints{};
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   addrinfo* addresses = nullptr;
   const std::string service = std::to_string(port);
   if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
      return false;
   }

   for (const addrinfo* address = addresses; address != nullptr;
        address = address->ai_next) {
      const int candidate =
          ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (candidate < 0) {
         continue;
      }
      if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
         fd = candidate;
         ::freeaddrinfo(addresses);
         return true;
      }
      ::close(candidate);
   }

   ::freeaddrinfo(addresses);
   return false;
}

int bindLoopback(std::uint16_t& port) {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   CHECK(fd >= 0);

   int reuse = 1;
   (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

   sockaddr_in address{};
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = 0;
   CHECK(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) ==
         0);

   socklen_t length = sizeof address;
   CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
   port = ntohs(address.sin_port);
   CHECK(port != 0);
   return fd;
}

nlohmann::json toolDefinition(const std::string& name,
                              const std::string& description) {
   return nlohmann::json{
       {"name", name},
       {"description", description},
       {"inputSchema", nlohmann::json{{"type", "object"}}},
       {"annotations", nlohmann::json{{"readOnlyHint", true}}},
   };
}

/* The application's view: the four library tools plus one extra name. */
nlohmann::json fakeAppTools(bool withTerminate = true) {
   nlohmann::json tools = nlohmann::json::array({
       toolDefinition("sdl_info", "fake app info"),
       toolDefinition("sdl_input", "fake app input"),
       toolDefinition("sdl_capture", "fake app capture"),
       toolDefinition("app_info", "fake extra"),
   });
   if (withTerminate) {
      tools.push_back(toolDefinition("sdl_terminate", "fake app terminate"));
   }
   return tools;
}

struct McpClient {
   FdChannel channel;
   int nextId = 1;
   std::vector<nlohmann::json> notifications;

   nlohmann::json readMessage(int timeoutMs = kLineTimeoutMs) {
      std::string line;
      REQUIRE(channel.readLine(line, timeoutMs));
      const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
      CHECK(!message.is_discarded());
      return message;
   }

   int send(const std::string& method,
            nlohmann::json params = nlohmann::json::object()) {
      const int id = nextId++;
      CHECK(channel.writeLine(nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", id},
          {"method", method},
          {"params", std::move(params)},
      }
                                  .dump()));
      return id;
   }

   nlohmann::json readResponse(int id, int timeoutMs = kLineTimeoutMs) {
      for (;;) {
         nlohmann::json message = readMessage(timeoutMs);
         if (message.contains("method")) {
            notifications.push_back(std::move(message));
            continue;
         }
         CHECK(message.contains("id"));
         CHECK(message["id"] == id);
         return message;
      }
   }

   nlohmann::json request(const std::string& method,
                          nlohmann::json params = nlohmann::json::object(),
                          int timeoutMs = kLineTimeoutMs) {
      return readResponse(send(method, std::move(params)), timeoutMs);
   }

   nlohmann::json initialize() {
      return request("initialize", nlohmann::json{
                                       {"protocolVersion", "2025-11-25"},
                                       {"capabilities", nlohmann::json::object()},
                                       {"clientInfo", nlohmann::json{
                                                          {"name", "handler-tests"},
                                                          {"version", "0.0.0"},
                                                      }},
                                   });
   }

   nlohmann::json toolsList() { return request("tools/list"); }

   nlohmann::json callTool(const std::string& name,
                           nlohmann::json arguments = nlohmann::json::object(),
                           int timeoutMs = kLineTimeoutMs) {
      return request("tools/call", nlohmann::json{
                                       {"name", name},
                                       {"arguments", std::move(arguments)},
                                   },
                     timeoutMs);
   }

   nlohmann::json readNotification(const std::string& method,
                                   int timeoutMs = kLineTimeoutMs) {
      nlohmann::json message = readMessage(timeoutMs);
      CHECK(message.contains("method"));
      CHECK(message["method"] == method);
      notifications.push_back(message);
      return message;
   }
};

std::string resultText(const nlohmann::json& response) {
   CHECK(response.contains("result"));
   CHECK(response["result"]["content"].is_array());
   CHECK(!response["result"]["content"].empty());
   return response["result"]["content"][0].value("text", std::string());
}

std::vector<std::string> toolNames(const nlohmann::json& listResponse) {
   CHECK(listResponse.contains("result"));
   const nlohmann::json& tools = listResponse["result"]["tools"];
   CHECK(tools.is_array());
   std::vector<std::string> names;
   for (const nlohmann::json& tool : tools) {
      names.push_back(tool.value("name", std::string()));
   }
   return names;
}

const nlohmann::json* toolByName(const nlohmann::json& listResponse,
                                 const std::string& name) {
   for (const nlohmann::json& tool : listResponse["result"]["tools"]) {
      if (tool.value("name", std::string()) == name) {
         return &tool;
      }
   }
   return nullptr;
}

int base64Value(char character) {
   if (character >= 'A' && character <= 'Z') {
      return character - 'A';
   }
   if (character >= 'a' && character <= 'z') {
      return character - 'a' + 26;
   }
   if (character >= '0' && character <= '9') {
      return character - '0' + 52;
   }
   if (character == '+') {
      return 62;
   }
   if (character == '/') {
      return 63;
   }
   return -1;
}

std::vector<std::uint8_t> base64Decode(const std::string& text) {
   std::vector<std::uint8_t> out;
   int accumulator = 0;
   int bits = 0;
   for (const char character : text) {
      if (character == '=') {
         break;
      }
      const int value = base64Value(character);
      if (value < 0) {
         continue;
      }
      accumulator = (accumulator << 6) | value;
      bits += 6;
      if (bits >= 8) {
         bits -= 8;
         out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
      }
   }
   return out;
}

std::uint32_t readBigEndian32(const std::vector<std::uint8_t>& bytes,
                              std::size_t offset) {
   return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
          (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
          (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
          static_cast<std::uint32_t>(bytes[offset + 3]);
}

/* The IHDR width and height, which is the dimension check the contract names. */
bool pngDimensions(const std::vector<std::uint8_t>& png,
                   std::uint32_t& width,
                   std::uint32_t& height) {
   if (png.size() < 24 || png[12] != 'I' || png[13] != 'H' || png[14] != 'D' ||
       png[15] != 'R') {
      return false;
   }
   width = readBigEndian32(png, 16);
   height = readBigEndian32(png, 20);
   return true;
}

bool secondConnectionIsClosed(std::uint16_t port) {
   int fd = -1;
   if (!connectTcp("127.0.0.1", port, fd)) {
      return false;
   }

   pollfd descriptor{};
   descriptor.fd = fd;
   descriptor.events = POLLIN;
   const int ready = ::poll(&descriptor, 1, 3000);
   if (ready <= 0) {
      ::close(fd);
      return false;
   }

   char byte = 0;
   const ssize_t count = ::recv(fd, &byte, 1, 0);
   const bool closed = count == 0 || (count < 0 && errno == ECONNRESET);
   ::close(fd);
   return closed;
}

/* The application side: dials the Handler and answers its handshake. */
struct FakeApp {
   FdChannel channel;
   int fd = -1;

   ~FakeApp() { close(); }

   void close() {
      if (fd >= 0) {
         ::close(fd);
         fd = -1;
      }
   }

   bool connect(std::uint16_t port) {
      if (!connectTcp("127.0.0.1", port, fd)) {
         std::fprintf(stderr, "fake app connect failed\n");
         return false;
      }
      channel.readFd = fd;
      channel.writeFd = fd;
      return true;
   }

   nlohmann::json readMessage(int timeoutMs = kLineTimeoutMs) {
      std::string line;
      REQUIRE(channel.readLine(line, timeoutMs));
      const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
      CHECK(!message.is_discarded());
      return message;
   }

   void write(nlohmann::json message) {
      CHECK(channel.writeLine(message.dump()));
   }

   void writeResponse(nlohmann::json id, nlohmann::json result) {
      write(nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", std::move(id)},
          {"result", std::move(result)},
      });
   }

   void handshake(const nlohmann::json& tools) {
      const nlohmann::json initialize = readMessage();
      CHECK(initialize["method"] == "initialize");
      CHECK(initialize["params"]["protocolVersion"] == "2025-11-25");
      writeResponse(initialize["id"],
                    nlohmann::json{
                        {"protocolVersion", "2025-11-25"},
                        {"capabilities",
                         nlohmann::json{{"tools", nlohmann::json::object()}}},
                        {"serverInfo",
                         nlohmann::json{{"name", "fake-app"},
                                        {"version", "0.0.0"}}},
                    });

      const nlohmann::json ack = readMessage();
      CHECK(ack["method"] == "notifications/initialized");

      const nlohmann::json list = readMessage();
      CHECK(list["method"] == "tools/list");
      writeResponse(list["id"], nlohmann::json{{"tools", tools}});
   }

   nlohmann::json readCall() {
      const nlohmann::json call = readMessage();
      CHECK(call["method"] == "tools/call");
      return call;
   }

   void serveContent(const std::string& text) {
      const nlohmann::json call = readCall();
      writeResponse(call["id"],
                    nlohmann::json{
                        {"content", nlohmann::json::array({
                                        nlohmann::json{{"type", "text"},
                                                       {"text", text}},
                                    })},
                        {"isError", false},
                    });
   }

   void serveRawFrame(int width,
                      int height,
                      std::uint64_t frame,
                      std::string text = {}) {
      const nlohmann::json call = readCall();
      const std::size_t byteLength =
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
      nlohmann::json header{
          {"format", "rgba8888"},
          {"width", width},
          {"height", height},
          {"byteLength", byteLength},
          {"frame", frame},
      };
      if (!text.empty()) {
         header["text"] = std::move(text);
      }
      writeResponse(call["id"], header);

      std::vector<std::uint8_t> pixels(byteLength, 0x80);
      CHECK(channel.writeAll(pixels.data(), pixels.size()));
   }

   void announceTools(const nlohmann::json& tools) {
      write(nlohmann::json{{"jsonrpc", "2.0"},
                           {"method", "notifications/tools/list_changed"}});
      const nlohmann::json list = readMessage();
      CHECK(list["method"] == "tools/list");
      writeResponse(list["id"], nlohmann::json{{"tools", tools}});
   }
};

void startServer(TestProcess& process,
                 std::uint16_t& port,
                 McpClient& client,
                 int timeoutMs = 2000) {
   process.start(g_handler,
                 {"--listen=127.0.0.1:0", "--timeout-ms=" + std::to_string(timeoutMs)});
   port = process.readListeningPort();
   client.channel.readFd = process.fromChild;
   client.channel.writeFd = process.toChild;
}

void attachFakeApp(FakeApp& app,
                   std::uint16_t port,
                   const nlohmann::json& tools = fakeAppTools()) {
   CHECK(app.connect(port));
   app.handshake(tools);
}

/* A non-checking read, for polling a file a spawned process may not have written. */
bool tryReadFile(const std::string& path, std::string& out) {
   std::FILE* file = std::fopen(path.c_str(), "rb");
   if (file == nullptr) {
      return false;
   }
   out.clear();
   char chunk[256];
   std::size_t count = 0;
   while ((count = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
      out.append(chunk, count);
   }
   std::fclose(file);
   return true;
}

/* Polls a pid file until the spawned app writes it, or the wait expires. */
pid_t waitForPidFile(const std::string& path, int timeoutMs = kLineTimeoutMs) {
   const auto deadline =
       std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
   for (;;) {
      std::string text;
      if (tryReadFile(path, text)) {
         const long parsed = std::strtol(text.c_str(), nullptr, 10);
         if (parsed > 0) {
            return static_cast<pid_t>(parsed);
         }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         return -1;
      }
      std::this_thread::sleep_for(10ms);
   }
}

bool processExists(pid_t pid) {
   return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

bool processIsGone(pid_t pid, int timeoutMs = kLineTimeoutMs) {
   const auto deadline =
       std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
   for (;;) {
      if (!processExists(pid)) {
         return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         return false;
      }
      std::this_thread::sleep_for(10ms);
   }
}

/* The test owns a spawned process the Handler deliberately never reaps. */
void killApp(pid_t pid) {
   if (!processExists(pid)) {
      return;
   }
   ::kill(pid, SIGKILL);
   (void)processIsGone(pid);
}

/*
  An app leaving is observed through behaviour: sdl_info is refused with the
  universal no-app error until the Handler has dropped the session.
*/
bool awaitNoApp(McpClient& client, int timeoutMs = kLineTimeoutMs) {
   const auto deadline =
       std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
   for (;;) {
      const nlohmann::json info = client.callTool("sdl_info");
      if (!info.contains("error") && info["result"]["isError"] == true &&
          resultText(info).find("no application session is attached") !=
              std::string::npos) {
         return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         std::fprintf(stderr, "the handler never returned to idle; last sdl_info: %s\n",
                      info.dump().c_str());
         return false;
      }
      std::this_thread::sleep_for(20ms);
   }
}

/* A server whose command line also carries the given extra arguments. */
void startServerWith(TestProcess& process,
                     std::uint16_t& port,
                     McpClient& client,
                     const std::vector<std::string>& extra,
                     int timeoutMs = 2000) {
   std::vector<std::string> arguments{
       "--listen=127.0.0.1:0",
       "--timeout-ms=" + std::to_string(timeoutMs),
   };
   arguments.insert(arguments.end(), extra.begin(), extra.end());
   process.start(g_handler, arguments);
   port = process.readListeningPort();
   client.channel.readFd = process.fromChild;
   client.channel.writeFd = process.toChild;
}

}  // namespace

TEST_CASE("targets files validate and accumulate") {
   using sdl3mcp::loadSpawnTargets;
   using sdl3mcp::SpawnTargets;

   const auto failsWith = [](const std::string& contents,
                             const std::string& needle) {
      TempPath file;
      writeFile(file.path, contents);
      SpawnTargets targets;
      std::string error;
      CHECK(!loadSpawnTargets(file.path, targets, error));
      if (error.find(needle) == std::string::npos) {
         std::fprintf(stderr, "error for %s did not name %s: %s\n",
                      contents.c_str(), needle.c_str(), error.c_str());
      }
      CHECK(error.find(needle) != std::string::npos);
      CHECK(targets.empty());
   };

   failsWith("{", "not valid JSON");
   failsWith("[]", "JSON object");
   failsWith("{}", "defines no aliases");
   failsWith(R"({"Bad":["app"]})", "Bad");
   failsWith(R"({"bad-key":["app"]})", "bad-key");
   failsWith(R"({"app_spawn_x":["app"]})", "app_spawn_x");
   failsWith(R"({"x":null})", "x");
   failsWith(R"({"x":[]})", "x");
   failsWith(R"({"x":["app",5]})", "x");
   failsWith(R"({"x":["app","--mcp=stdio"]})", "x");

   SpawnTargets targets;
   std::string error;
   CHECK(!loadSpawnTargets("/nonexistent/sdl3mcp-targets.json", targets,
                           error));
   CHECK(error.find("cannot open") != std::string::npos);

   TempPath valid;
   writeFile(valid.path,
             R"({"alpha":["./alpha","--flag"],"beta":["/bin/beta"]})");
   CHECK(loadSpawnTargets(valid.path, targets, error));
   CHECK(targets.size() == 2);
   CHECK(targets[0].alias == "alpha");
   CHECK((targets[0].argv == std::vector<std::string>{"./alpha", "--flag"}));
   CHECK(targets[1].alias == "beta");
   CHECK((targets[1].argv == std::vector<std::string>{"/bin/beta"}));

   /* A directory contributes every top-level *.json in sorted order. */
   TempDir directory;
   writeFile(directory.file("10-alpha.json"), R"({"alpha":["./alpha"]})");
   writeFile(directory.file("20-beta.json"), R"({"beta":["/bin/beta"]})");
   writeFile(directory.file("notes.txt"), "ignored");

   CHECK(loadSpawnTargets(directory.path, targets, error));
   CHECK(targets.size() == 2);
   CHECK(targets[0].alias == "alpha");
   CHECK(targets[1].alias == "beta");

   /* Repeating --targets unions a directory and a hand-written file. */
   TempDir extra;
   writeFile(extra.file("extra.json"), R"({"gamma":["./gamma"]})");
   CHECK(loadSpawnTargets({directory.path, extra.file("extra.json")}, targets,
                          error));
   CHECK(targets.size() == 3);
   CHECK(targets[2].alias == "gamma");

   /* A duplicate alias across two files names both and replaces neither. */
   TempDir duplicates;
   writeFile(duplicates.file("a.json"), R"({"alpha":["/a"]})");
   writeFile(duplicates.file("b.json"), R"({"alpha":["/b"]})");
   CHECK(!loadSpawnTargets(duplicates.path, targets, error));
   CHECK(targets.empty());
   CHECK(error.find(duplicates.file("a.json")) != std::string::npos);
   CHECK(error.find(duplicates.file("b.json")) != std::string::npos);

   /* A malformed file inside a directory names that file. */
   TempDir broken;
   writeFile(broken.file("good.json"), R"({"good":["/good"]})");
   writeFile(broken.file("zz-broken.json"), "{");
   CHECK(!loadSpawnTargets(broken.path, targets, error));
   CHECK(error.find(broken.file("zz-broken.json")) != std::string::npos);

   /* A directory with no *.json files names the directory. */
   TempDir empty;
   CHECK(!loadSpawnTargets(empty.path, targets, error));
   CHECK(error.find(empty.path) != std::string::npos);
   CHECK(error.find("*.json") != std::string::npos);
}

TEST_CASE("a target dry run prints the argv and starts nothing") {
   TempDir directory;
   writeFile(directory.file("targets.json"),
             R"({"alpha":["/bin/echo","--flag"],"beta":["/bin/true"]})");
   const std::string targetsPath = directory.file("targets.json");

   /* An ephemeral listener prints the literal placeholder it would append. */
   {
      TestProcess process;
      process.start(g_handler, {"--targets=" + targetsPath, "--dry-run=alpha"});
      const std::string out = readAllFd(process.fromChild, kLineTimeoutMs);
      CHECK(process.waitForExit(kLineTimeoutMs));
      CHECK(process.exitCode == 0);
      CHECK(out.find("/bin/echo") != std::string::npos);
      CHECK(out.find("--flag") != std::string::npos);
      CHECK(out.find("--mcp=HOST:PORT") != std::string::npos);
      /* A run that had bound a port would have said so on stderr. */
      CHECK(readAllFd(process.stderrChild, 100).find("listening on") ==
            std::string::npos);
   }

   /* An explicit listener prints the endpoint it would actually append. */
   {
      TestProcess process;
      process.start(g_handler,
                    {"--targets=" + targetsPath, "--listen=127.0.0.1:9999",
                     "--dry-run=beta"});
      const std::string out = readAllFd(process.fromChild, kLineTimeoutMs);
      CHECK(process.waitForExit(kLineTimeoutMs));
      CHECK(process.exitCode == 0);
      CHECK(out.find("/bin/true") != std::string::npos);
      CHECK(out.find("--mcp=127.0.0.1:9999") != std::string::npos);
   }

   /* An unknown alias is an error that names it. */
   {
      TestProcess process;
      process.start(g_handler, {"--targets=" + targetsPath, "--dry-run=gamma"});
      const std::string err = readAllFd(process.stderrChild, kLineTimeoutMs);
      CHECK(process.waitForExit(kLineTimeoutMs));
      CHECK(process.exitCode != 0);
      CHECK(err.find("gamma") != std::string::npos);
   }
}

TEST_CASE("a targets server advertises spawn tools and spawns one app at a time") {
   TempDir directory;
   const std::string alphaPid = directory.file("alpha.pid");
   const std::string alphaArgv = directory.file("alpha.argv");
   const std::string betaPid = directory.file("beta.pid");
   writeFile(directory.file("targets.json"),
             nlohmann::json{
                 {"alpha", nlohmann::json::array({g_fakeApp, "--pid-file", alphaPid,
                                                  "--argv-file", alphaArgv})},
                 {"beta", nlohmann::json::array({g_fakeApp, "--pid-file", betaPid})},
             }
                 .dump());

   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServerWith(process, port, client,
                   {"--targets=" + directory.file("targets.json")});
   CHECK(port != 0);
   CHECK(!client.initialize().contains("error"));

   const nlohmann::json idle = client.toolsList();
   const std::vector<std::string> names = toolNames(idle);
   CHECK((names == std::vector<std::string>{"app_spawn_alpha", "app_spawn_beta",
                                            "sdl_capture", "sdl_info", "sdl_input",
                                            "sdl_terminate", "wait_session"}));

   /* The spawn returns only after the app has dialed and handshaken. */
   const nlohmann::json spawned = client.callTool("app_spawn_alpha");
   CHECK(spawned["result"]["isError"] == false);
   const std::string spawnedText = resultText(spawned);
   CHECK(spawnedText.rfind("spawned alpha (pid ", 0) == 0);
   const pid_t pid = waitForPidFile(alphaPid);
   CHECK(pid > 0);
   CHECK(spawnedText.find(std::to_string(pid)) != std::string::npos);
   CHECK(processExists(pid));

   /*
     The configured argv is preserved and the listener endpoint is appended, not
     substituted: the app was told the port the Handler actually bound.
   */
   const std::string argvText = readFile(alphaArgv);
   CHECK(argvText.rfind(g_fakeApp + "\n", 0) == 0);
   CHECK(argvText.find("--pid-file\n" + alphaPid + "\n") != std::string::npos);
   CHECK(argvText.find("--mcp=127.0.0.1:" + std::to_string(port) + "\n") !=
         std::string::npos);

   /* A spawn, attach, or detach changes no advertised name. */
   CHECK(client.toolsList()["result"].dump() == idle["result"].dump());
   CHECK(client.notifications.empty());

   /* The attached app's tools forward through the Handler. */
   const nlohmann::json info = client.callTool("sdl_info");
   CHECK(info["result"]["isError"] == false);
   CHECK(resultText(info) == "fake target app info");

   /* One application at a time; the second spawn starts nothing. */
   const nlohmann::json second = client.callTool("app_spawn_beta");
   CHECK(second["result"]["isError"] == true);
   CHECK(resultText(second) ==
         "an application is already attached; one spawn at a time");
   CHECK(processExists(pid));
   std::string betaPidText;
   CHECK(!tryReadFile(betaPid, betaPidText));

   /* sdl_terminate is an ordinary forward; the exit returns the Handler to idle. */
   const nlohmann::json terminated = client.callTool("sdl_terminate");
   CHECK(terminated["result"]["isError"] == false);
   CHECK(resultText(terminated) == "termination requested");
   CHECK(awaitNoApp(client));
   CHECK(processIsGone(pid));
   CHECK(client.toolsList()["result"].dump() == idle["result"].dump());

   /* Idle again, so the other alias spawns. */
   const nlohmann::json again = client.callTool("app_spawn_beta");
   CHECK(again["result"]["isError"] == false);
   CHECK(resultText(again).rfind("spawned beta (pid ", 0) == 0);
   const pid_t beta = waitForPidFile(betaPid);
   CHECK(beta > 0);
   CHECK(client.callTool("sdl_terminate")["result"]["isError"] == false);
   CHECK(awaitNoApp(client));
   killApp(beta);

   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("a spawn that never connects times out and leaves the app running") {
   TempDir directory;
   const std::string slowPid = directory.file("slow.pid");
   writeFile(directory.file("targets.json"),
             nlohmann::json{
                 {"slow", nlohmann::json::array({g_fakeApp, "--pid-file", slowPid,
                                                 "--dial-delay-ms", "5000"})},
             }
                 .dump());

   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServerWith(process, port, client,
                   {"--targets=" + directory.file("targets.json")});
   CHECK(!client.initialize().contains("error"));

   const auto started = std::chrono::steady_clock::now();
   const nlohmann::json answer =
       client.callTool("app_spawn_slow", nlohmann::json{{"timeout_ms", 300}});
   const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
   CHECK(answer["result"]["isError"] == true);
   const std::string text = resultText(answer);
   CHECK(text.find("300") != std::string::npos);
   CHECK(text.find("pid") != std::string::npos);
   CHECK(text.find("may still be running") != std::string::npos);
   CHECK(elapsed >= 250);
   CHECK(elapsed < 3000);

   /* Spawn-and-forget: a timed-out spawn is reported, not killed. */
   const pid_t pid = waitForPidFile(slowPid);
   CHECK(pid > 0);
   CHECK(processExists(pid));

   /* The Handler is idle even though the child still exists. */
   CHECK(awaitNoApp(client));

   killApp(pid);
   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("a spawn returns only after the application handshakes") {
   TempDir directory;
   const std::string pidPath = directory.file("app.pid");
   writeFile(directory.file("targets.json"),
             nlohmann::json{
                 {"slowinit",
                  nlohmann::json::array({g_fakeApp, "--pid-file", pidPath,
                                         "--initialize-delay-ms", "400"})},
             }
                 .dump());

   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServerWith(process, port, client,
                   {"--targets=" + directory.file("targets.json")});
   CHECK(!client.initialize().contains("error"));

   const auto started = std::chrono::steady_clock::now();
   const nlohmann::json spawned = client.callTool("app_spawn_slowinit");
   const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
   CHECK(spawned["result"]["isError"] == false);
   CHECK(resultText(spawned).rfind("spawned slowinit (pid ", 0) == 0);
   /* The answer is the handshake's, not the socket's: it may not arrive early. */
   CHECK(elapsed >= 350);

   CHECK(client.callTool("sdl_terminate")["result"]["isError"] == false);
   CHECK(awaitNoApp(client));
   killApp(waitForPidFile(pidPath));

   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("a spawn that cannot exec names the reason and the pid") {
   TempDir directory;
   writeFile(directory.file("targets.json"),
             nlohmann::json{
                 {"broken",
                  nlohmann::json::array({"/nonexistent/sdl3mcp-handler-app"})},
             }
                 .dump());

   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServerWith(process, port, client,
                   {"--targets=" + directory.file("targets.json")});
   CHECK(!client.initialize().contains("error"));

   const nlohmann::json answer = client.callTool("app_spawn_broken");
   CHECK(answer["result"]["isError"] == true);
   const std::string text = resultText(answer);
   CHECK(text.find("cannot execute") != std::string::npos);
   CHECK(text.find("pid") != std::string::npos);
   CHECK(awaitNoApp(client));

   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("handler shutdown leaves a spawned app running") {
   TempDir directory;
   const std::string pidPath = directory.file("app.pid");
   writeFile(directory.file("targets.json"),
             nlohmann::json{
                 {"alpha",
                  nlohmann::json::array(
                      {g_fakeApp, "--pid-file", pidPath, "--stay-after-close"})},
             }
                 .dump());

   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServerWith(process, port, client,
                   {"--targets=" + directory.file("targets.json")});
   CHECK(!client.initialize().contains("error"));
   CHECK(client.callTool("app_spawn_alpha")["result"]["isError"] == false);
   const pid_t pid = waitForPidFile(pidPath);
   CHECK(pid > 0);

   /* stdin EOF ends the Handler; the app is not its to kill. */
   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
   CHECK(processExists(pid));
   killApp(pid);
}

TEST_CASE("an invalid targets source is fatal before the listener binds") {
   TempPath file;
   writeFile(file.path, "{}");

   TestProcess process;
   process.start(g_handler, {"--targets=" + file.path});
   const std::string err = readAllFd(process.stderrChild, kLineTimeoutMs);
   CHECK(process.waitForExit(kLineTimeoutMs));
   CHECK(process.exitCode == 2);
   CHECK(err.find("defines no aliases") != std::string::npos);
   CHECK(err.find("listening on") == std::string::npos);
}

TEST_CASE("the handler command line") {
   using sdl3mcp::parseHandlerCommandLine;

   const auto server = parseHandlerCommandLine({"--listen=127.0.0.1:0"});
   CHECK(server.ok);
   CHECK(!server.exit);
   CHECK(server.options.hasListen);
   CHECK(server.options.listenHost == "127.0.0.1");
   CHECK(server.options.listenPort == 0);
   CHECK(!server.options.relay());
   CHECK(server.options.timeoutMs == 10000);

   const auto relay = parseHandlerCommandLine(
       {"--listen=127.0.0.1:7778", "--connect=127.0.0.1:7777",
        "--timeout-ms=2500"});
   CHECK(relay.ok);
   CHECK(relay.options.relay());
   CHECK(relay.options.connectHost == "127.0.0.1");
   CHECK(relay.options.connectPort == 7777);
   CHECK(relay.options.listenPort == 7778);
   CHECK(relay.options.timeoutMs == 2500);

   CHECK(!parseHandlerCommandLine({}).ok);
   CHECK(!parseHandlerCommandLine({"--listen"}).ok);
   CHECK(!parseHandlerCommandLine({"--listen=127.0.0.1"}).ok);
   CHECK(!parseHandlerCommandLine({"--listen=:7000"}).ok);
   CHECK(!parseHandlerCommandLine({"--listen=127.0.0.1:70000"}).ok);
   CHECK(!parseHandlerCommandLine({"--listen=127.0.0.1:0", "--timeout-ms=0"}).ok);
   CHECK(!parseHandlerCommandLine({"--listen=127.0.0.1:0", "--timeout-ms=60001"}).ok);
   CHECK(!parseHandlerCommandLine({"--listen=127.0.0.1:0", "--connect=x"}).ok);
   CHECK(!parseHandlerCommandLine({"--listen=127.0.0.1:0", "--bogus"}).ok);

   /* An app source makes --listen optional and defaults it to loopback:0. */
   const auto targetsOnly =
       parseHandlerCommandLine({"--targets=/tmp/targets.json"});
   CHECK(targetsOnly.ok);
   CHECK(targetsOnly.options.hasListen);
   CHECK(targetsOnly.options.listenHost == "127.0.0.1");
   CHECK(targetsOnly.options.listenPort == 0);
   CHECK(!targetsOnly.options.relay());
   CHECK((targetsOnly.options.targetsPaths ==
          std::vector<std::string>{"/tmp/targets.json"}));

   /* --targets is repeatable and keeps the sources in command-line order. */
   const auto repeated = parseHandlerCommandLine(
       {"--targets=a.json", "--targets=b.json"});
   CHECK(repeated.ok);
   CHECK((repeated.options.targetsPaths ==
          std::vector<std::string>{"a.json", "b.json"}));

   /* An explicit --listen still wins over the default. */
   const auto explicitListen = parseHandlerCommandLine(
       {"--targets=a.json", "--listen=127.0.0.1:9999"});
   CHECK(explicitListen.ok);
   CHECK(explicitListen.options.listenPort == 9999);

   CHECK(!parseHandlerCommandLine({"--targets"}).ok);
   CHECK(!parseHandlerCommandLine({"--targets="}).ok);
   CHECK(!parseHandlerCommandLine({"--targets=a.json", "--targets"}).ok);

   /* --dry-run names one alias and needs no listener of its own. */
   const auto dryRun =
       parseHandlerCommandLine({"--targets=a.json", "--dry-run=alpha"});
   CHECK(dryRun.ok);
   CHECK(dryRun.options.hasDryRun);
   CHECK(dryRun.options.dryRunAlias == "alpha");
   CHECK(!parseHandlerCommandLine({"--targets=a.json", "--dry-run"}).ok);
   CHECK(!parseHandlerCommandLine({"--targets=a.json", "--dry-run="}).ok);
   CHECK(!parseHandlerCommandLine({"--dry-run=alpha"}).ok);

   const auto help = parseHandlerCommandLine({"--help"});
   CHECK(help.ok);
   CHECK(help.exit);
   CHECK(help.exitCode == 0);
   CHECK(help.message.find("--listen") != std::string::npos);
   CHECK(help.message.find("--targets") != std::string::npos);
   CHECK(help.message.find("--dry-run") != std::string::npos);
}

TEST_CASE("png and base64 round-trip") {
   std::vector<std::uint8_t> pixels(2 * 2 * 4, 0x7F);
   pixels[0] = 0xFF;
   pixels[1] = 0x00;
   pixels[2] = 0x10;
   pixels[3] = 0xFF;

   std::vector<std::uint8_t> png;
   REQUIRE(sdl3mcp::encodePngToVector(pixels.data(), 2, 2, png));
   CHECK(png.size() > 8);
   CHECK(png[0] == 0x89);
   CHECK(png[1] == 'P');
   CHECK(png[2] == 'N');
   CHECK(png[3] == 'G');

   std::uint32_t width = 0;
   std::uint32_t height = 0;
   REQUIRE(pngDimensions(png, width, height));
   CHECK(width == 2);
   CHECK(height == 2);

   CHECK(sdl3mcp::base64Encode(std::vector<std::uint8_t>{'M', 'a', 'n'}) == "TWFu");
   CHECK(sdl3mcp::base64Encode(std::vector<std::uint8_t>{'M'}) == "TQ==");
   CHECK(sdl3mcp::base64Encode(std::vector<std::uint8_t>{'M', 'a'}) == "TWE=");
}

TEST_CASE("a server refuses app calls before attach and waits") {
   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServer(process, port, client);
   CHECK(port != 0);

   CHECK(!client.initialize().contains("error"));

   /* Sorted map order: the catalog is a map, not an insertion list. */
   const std::vector<std::string> names = toolNames(client.toolsList());
   CHECK((names == std::vector<std::string>{"sdl_capture", "sdl_info", "sdl_input",
                                            "sdl_terminate", "wait_session"}));

   const nlohmann::json missing = client.callTool("sdl_info");
   CHECK(missing["result"]["isError"] == true);
   CHECK(resultText(missing) ==
         "no application session is attached; call wait_session first");

   const auto started = std::chrono::steady_clock::now();
   const nlohmann::json timeout =
       client.callTool("wait_session", nlohmann::json{{"timeout_ms", 200}});
   const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
   CHECK(timeout["result"]["isError"] == true);
   CHECK(resultText(timeout).find("200 ms") != std::string::npos);
   CHECK(elapsed >= 150);
   CHECK(elapsed < 3000);

   const nlohmann::json invalid =
       client.callTool("wait_session", nlohmann::json{{"timeout_ms", 0}});
   CHECK(invalid["result"]["isError"] == true);

   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("a server attaches one app, merges tools, and forwards calls") {
   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServer(process, port, client);
   CHECK(!client.initialize().contains("error"));

   /* Park a wait_session; the attach wakes it without changing the list. */
   const int waitId = client.send("tools/call", nlohmann::json{
                                                    {"name", "wait_session"},
                                                    {"arguments", nlohmann::json{
                                                                      {"timeout_ms", 5000}}},
                                                });

   FakeApp app;
   attachFakeApp(app, port, fakeAppTools(false));

   const nlohmann::json waited = client.readResponse(waitId);
   CHECK(waited["result"]["isError"] == false);
   CHECK(resultText(waited) == "an application session attached");
   CHECK(client.notifications.empty());

   const nlohmann::json list = client.toolsList();
   const std::vector<std::string> names = toolNames(list);
   CHECK((names == std::vector<std::string>{"app_info", "sdl_capture", "sdl_info",
                                            "sdl_input", "sdl_terminate",
                                            "wait_session"}));

   /* The peer wins sdl_info; the Handler keeps capture and input presentation. */
   const nlohmann::json* info = toolByName(list, "sdl_info");
   REQUIRE(info != nullptr);
   CHECK((*info)["description"] == "fake app info");

   const nlohmann::json* capture = toolByName(list, "sdl_capture");
   REQUIRE(capture != nullptr);
   CHECK((*capture)["inputSchema"]["properties"].contains("timeout_ms"));
   CHECK((*capture)["inputSchema"]["properties"].contains("region"));
   CHECK((*capture)["description"].get<std::string>().find("PNG") !=
         std::string::npos);

   const nlohmann::json* input = toolByName(list, "sdl_input");
   REQUIRE(input != nullptr);
   CHECK((*input)["inputSchema"]["properties"].contains("timeout_ms"));
   CHECK((*input)["inputSchema"]["properties"].contains("actions"));

   /* A content result from the app crosses unchanged. */
   const int infoId = client.send("tools/call", nlohmann::json{
                                                    {"name", "sdl_info"},
                                                    {"arguments", nlohmann::json::object()},
                                                });
   app.serveContent("fake app info ok");
   const nlohmann::json infoResult = client.readResponse(infoId);
   CHECK(infoResult["result"]["isError"] == false);
   CHECK(resultText(infoResult) == "fake app info ok");

   /* The raw capture framing becomes a text summary plus one PNG block. */
   const nlohmann::json captureArguments{
       {"timeout_ms", 3000},
       {"region", nlohmann::json{{"x", 0}, {"y", 0}, {"width", 2}, {"height", 2}}},
   };
   const int captureId = client.send("tools/call", nlohmann::json{
                                                       {"name", "sdl_capture"},
                                                       {"arguments", captureArguments},
                                                   });
   app.serveRawFrame(2, 2, 12);
   const nlohmann::json image = client.readResponse(captureId);
   CHECK(image["result"]["isError"] == false);
   const nlohmann::json& content = image["result"]["content"];
   REQUIRE(content.size() == 2);
   CHECK(content[0]["type"] == "text");
   CHECK(content[0]["text"].get<std::string>().rfind("2x2 PNG at 0,0,", 0) == 0);
   CHECK(content[0]["text"].get<std::string>().find("frame 12 attached") !=
         std::string::npos);
   CHECK(content[1]["type"] == "image");
   CHECK(content[1]["mimeType"] == "image/png");
   CHECK(content[1]["data"].get<std::string>().size() > 20);

   /* The PNG is at exactly the dimensions the application sent. */
   const std::vector<std::uint8_t> png =
       base64Decode(content[1]["data"].get<std::string>());
   std::uint32_t pngWidth = 0;
   std::uint32_t pngHeight = 0;
   REQUIRE(pngDimensions(png, pngWidth, pngHeight));
   CHECK(pngWidth == 2);
   CHECK(pngHeight == 2);

   /*
     The Handler's timeout_ms is its forward budget, not the library's: it is
     stripped before the call reaches the app, which would reject it otherwise.
   */
   const int inputId = client.send("tools/call", nlohmann::json{
                                                     {"name", "sdl_input"},
                                                     {"arguments", nlohmann::json{
                                                                       {"actions", nlohmann::json::array({
                                                                                       nlohmann::json{{"type", "key"},
                                                                                                      {"key", "r"}},
                                                                                   })},
                                                                       {"timeout_ms", 3000}}},
                                                 });
   const nlohmann::json inputCall = app.readCall();
   CHECK(inputCall["params"]["name"] == "sdl_input");
   CHECK(!inputCall["params"]["arguments"].contains("timeout_ms"));
   CHECK(inputCall["params"]["arguments"].contains("actions"));
   app.writeResponse(inputCall["id"],
                     nlohmann::json{
                         {"format", "rgba8888"},
                         {"width", 2},
                         {"height", 2},
                         {"byteLength", 16},
                         {"frame", 14},
                         {"text",
                          "1 actions applied at frame 12; frame 14 "
                          "attached, rendered after the actions were "
                          "delivered"},
                     });
   {
      std::vector<std::uint8_t> pixels(16, 0x40);
      CHECK(app.channel.writeAll(pixels.data(), pixels.size()));
   }
   const nlohmann::json inputResult = client.readResponse(inputId);
   CHECK(inputResult["result"]["isError"] == false);
   CHECK(resultText(inputResult) ==
         "1 actions applied at frame 12; frame 14 attached, rendered after the "
         "actions were delivered");
   CHECK(inputResult["result"]["content"].size() == 2);
   CHECK(inputResult["result"]["content"][1]["type"] == "image");

   /* A tool the base advertises but this app does not provide is refused. */
   const nlohmann::json absent = client.callTool("sdl_terminate");
   CHECK(absent["result"]["isError"] == true);
   CHECK(resultText(absent) == "the application does not provide sdl_terminate");

   /* Exactly one app: a second connection is closed, not queued. */
   CHECK(secondConnectionIsClosed(port));

   /* Detach drops the app's entries; the base list is still there. */
   app.close();
   for (int attempt = 0; attempt < 100; ++attempt) {
      const nlohmann::json after = client.callTool("sdl_info");
      if (after["result"]["isError"] == true &&
          resultText(after).find("no application session is attached") !=
              std::string::npos) {
         break;
      }
      std::this_thread::sleep_for(20ms);
      REQUIRE(attempt < 99);
   }
   const std::vector<std::string> afterNames = toolNames(client.toolsList());
   CHECK((afterNames == std::vector<std::string>{"sdl_capture", "sdl_info",
                                                 "sdl_input", "sdl_terminate",
                                                 "wait_session"}));

   /* The client endpoint is still open and wait_session is usable again. */
   const nlohmann::json again =
       client.callTool("wait_session", nlohmann::json{{"timeout_ms", 200}});
   CHECK(again["result"]["isError"] == true);
   CHECK(resultText(again).find("200 ms") != std::string::npos);

   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("a peer that stops answering is detached and named") {
   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServer(process, port, client);
   CHECK(!client.initialize().contains("error"));

   FakeApp app;
   attachFakeApp(app, port);
   CHECK(client.callTool("wait_session", nlohmann::json{{"timeout_ms", 3000}})
             ["result"]["isError"] == false);

   const int callId = client.send("tools/call", nlohmann::json{
                                                    {"name", "sdl_info"},
                                                    {"arguments", nlohmann::json{
                                                                      {"timeout_ms", 200}}},
                                                });
   /* The app reads the call and never answers. */
   const nlohmann::json call = app.readCall();
   CHECK(call["params"]["name"] == "sdl_info");

   const nlohmann::json answer = client.readResponse(callId, 4000);
   CHECK(answer["result"]["isError"] == true);
   CHECK(resultText(answer).find("sdl_info") != std::string::npos);
   CHECK(resultText(answer).find("proxy timeout") != std::string::npos);

   const nlohmann::json after = client.callTool("sdl_info");
   CHECK(after["result"]["isError"] == true);
   CHECK(resultText(after).find("no application session is attached") !=
         std::string::npos);

   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("an oversized frame is refused before it is read") {
   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServer(process, port, client);
   CHECK(!client.initialize().contains("error"));

   FakeApp app;
   attachFakeApp(app, port);
   CHECK(client.callTool("wait_session", nlohmann::json{{"timeout_ms", 3000}})
             ["result"]["isError"] == false);

   const int callId = client.send("tools/call", nlohmann::json{
                                                    {"name", "sdl_capture"},
                                                    {"arguments", nlohmann::json::object()},
                                                });
   const nlohmann::json call = app.readCall();
   /*
     5000x5000 RGBA is 100,000,000 bytes, above the 64 MiB cap. The Handler must
     reject the header and answer without waiting for a payload that must never
     be allocated.
   */
   app.writeResponse(call["id"], nlohmann::json{
                                     {"format", "rgba8888"},
                                     {"width", 5000},
                                     {"height", 5000},
                                     {"byteLength", 100000000},
                                     {"frame", 1},
                                 });

   const nlohmann::json answer = client.readResponse(callId, 3000);
   CHECK(answer["result"]["isError"] == true);
   CHECK(resultText(answer).find("Handler cap") != std::string::npos);

   const nlohmann::json after = client.callTool("sdl_info");
   CHECK(after["result"]["isError"] == true);
   CHECK(resultText(after).find("no application session is attached") !=
         std::string::npos);

   app.close();
   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("a malformed frame header is a tool error") {
   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServer(process, port, client);
   CHECK(!client.initialize().contains("error"));

   FakeApp app;
   attachFakeApp(app, port);
   CHECK(client.callTool("wait_session", nlohmann::json{{"timeout_ms", 3000}})
             ["result"]["isError"] == false);

   const int callId = client.send("tools/call", nlohmann::json{
                                                    {"name", "sdl_capture"},
                                                    {"arguments", nlohmann::json::object()},
                                                });
   const nlohmann::json call = app.readCall();
   app.writeResponse(call["id"], nlohmann::json{
                                     {"format", "rgba8888"},
                                     {"width", 2},
                                     {"height", 2},
                                     {"byteLength", 4},
                                     {"frame", 1},
                                 });

   const nlohmann::json answer = client.readResponse(callId, 3000);
   CHECK(answer["result"]["isError"] == true);
   CHECK(resultText(answer).find("invalid frame result") != std::string::npos);

   const nlohmann::json after = client.callTool("sdl_info");
   CHECK(after["result"]["isError"] == true);
   CHECK(resultText(after).find("no application session is attached") !=
         std::string::npos);

   app.close();
   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("a peer announcement is re-listed and republished") {
   TestProcess process;
   std::uint16_t port = 0;
   McpClient client;
   startServer(process, port, client);
   CHECK(!client.initialize().contains("error"));

   FakeApp app;
   attachFakeApp(app, port);
   CHECK(client.callTool("wait_session", nlohmann::json{{"timeout_ms", 3000}})
             ["result"]["isError"] == false);

   nlohmann::json grown = fakeAppTools();
   grown.push_back(toolDefinition("app_extra", "added after attach"));
   app.announceTools(grown);

   client.readNotification("notifications/tools/list_changed");
   const std::vector<std::string> names = toolNames(client.toolsList());
   CHECK((names == std::vector<std::string>{"app_extra", "app_info", "sdl_capture",
                                            "sdl_info", "sdl_input", "sdl_terminate",
                                            "wait_session"}));

   process.closeStdin();
   CHECK(process.waitForExit(kLineTimeoutMs));
}

TEST_CASE("a relay dials its upstream and serves a dial-in app") {
   std::uint16_t upstreamPort = 0;
   const int upstreamListener = bindLoopback(upstreamPort);
   CHECK(::listen(upstreamListener, 1) == 0);

   TestProcess process;
   process.start(g_handler,
                 {"--connect=127.0.0.1:" + std::to_string(upstreamPort),
                  "--listen=127.0.0.1:0", "--timeout-ms=2000"});

   /* The Handler dials the upstream before it binds its own listener. */
   int upstreamFd = -1;
   for (int attempt = 0; attempt < 200 && upstreamFd < 0; ++attempt) {
      pollfd descriptor{};
      descriptor.fd = upstreamListener;
      descriptor.events = POLLIN;
      if (::poll(&descriptor, 1, 20) > 0) {
         upstreamFd = ::accept(upstreamListener, nullptr, nullptr);
      }
   }
   CHECK(upstreamFd >= 0);

   const std::uint16_t port = process.readListeningPort();

   McpClient client;
   client.channel.readFd = upstreamFd;
   client.channel.writeFd = upstreamFd;
   CHECK(!client.initialize().contains("error"));
   CHECK((toolNames(client.toolsList()) ==
          std::vector<std::string>{"sdl_capture", "sdl_info", "sdl_input",
                                   "sdl_terminate", "wait_session"}));

   FakeApp app;
   attachFakeApp(app, port);
   CHECK(client.callTool("wait_session", nlohmann::json{{"timeout_ms", 3000}})
             ["result"]["isError"] == false);

   const int callId = client.send("tools/call", nlohmann::json{
                                                    {"name", "sdl_capture"},
                                                    {"arguments", nlohmann::json::object()},
                                                });
   app.serveRawFrame(2, 2, 7);
   const nlohmann::json image = client.readResponse(callId);
   CHECK(image["result"]["isError"] == false);
   CHECK(image["result"]["content"].size() == 2);
   CHECK(image["result"]["content"][1]["mimeType"] == "image/png");

   /* The upstream closing ends the Handler; the app is left alone. */
   ::close(upstreamFd);
   CHECK(process.waitForExit(kLineTimeoutMs));

   ::close(upstreamListener);
}

TEST_CASE("a relay with targets spawns an app through the default listener") {
   TempDir directory;
   const std::string pidPath = directory.file("app.pid");
   writeFile(directory.file("targets.json"),
             nlohmann::json{
                 {"alpha", nlohmann::json::array({g_fakeApp, "--pid-file", pidPath,
                                                  "--capture-width", "3",
                                                  "--capture-height", "2"})},
             }
                 .dump());

   std::uint16_t upstreamPort = 0;
   const int upstreamListener = bindLoopback(upstreamPort);
   CHECK(::listen(upstreamListener, 1) == 0);

   /*
     No --listen: an app source supplies the default 127.0.0.1:0, and the app is
     told the port the kernel chose, not the one in the command line.
   */
   TestProcess process;
   process.start(g_handler,
                 {"--connect=127.0.0.1:" + std::to_string(upstreamPort),
                  "--targets=" + directory.file("targets.json"),
                  "--timeout-ms=3000"});

   int upstreamFd = -1;
   for (int attempt = 0; attempt < 200 && upstreamFd < 0; ++attempt) {
      pollfd descriptor{};
      descriptor.fd = upstreamListener;
      descriptor.events = POLLIN;
      if (::poll(&descriptor, 1, 20) > 0) {
         upstreamFd = ::accept(upstreamListener, nullptr, nullptr);
      }
   }
   CHECK(upstreamFd >= 0);
   const std::uint16_t port = process.readListeningPort();

   McpClient client;
   client.channel.readFd = upstreamFd;
   client.channel.writeFd = upstreamFd;
   CHECK(!client.initialize().contains("error"));

   const nlohmann::json idle = client.toolsList();
   const std::vector<std::string> names = toolNames(idle);
   CHECK((names == std::vector<std::string>{"app_spawn_alpha", "sdl_capture",
                                            "sdl_info", "sdl_input", "sdl_terminate",
                                            "wait_session"}));

   const nlohmann::json spawned = client.callTool("app_spawn_alpha");
   CHECK(spawned["result"]["isError"] == false);
   CHECK(resultText(spawned).rfind("spawned alpha (pid ", 0) == 0);
   const pid_t pid = waitForPidFile(pidPath);
   CHECK(pid > 0);
   CHECK(processExists(pid));
   CHECK(client.toolsList()["result"].dump() == idle["result"].dump());

   /* The capture crosses the app -> relay -> client chain as a PNG. */
   const nlohmann::json capture = client.callTool("sdl_capture");
   CHECK(capture["result"]["isError"] == false);
   const nlohmann::json& content = capture["result"]["content"];
   REQUIRE(content.size() == 2);
   CHECK(content[0]["text"].get<std::string>().rfind("3x2 PNG", 0) == 0);
   CHECK(content[1]["type"] == "image");
   CHECK(content[1]["mimeType"] == "image/png");
   CHECK(content[1]["data"].get<std::string>().rfind("iVBORw0KGgo", 0) == 0);

   /* The exit restores idle without moving the list. */
   CHECK(client.callTool("sdl_terminate")["result"]["isError"] == false);
   CHECK(awaitNoApp(client));
   CHECK(processIsGone(pid));
   CHECK(client.toolsList()["result"].dump() == idle["result"].dump());
   CHECK(client.notifications.empty());

   /* The upstream closing ends the relay; the app was already gone. */
   ::close(upstreamFd);
   CHECK(process.waitForExit(kLineTimeoutMs));
   ::close(upstreamListener);
}

int main(int argc, char** argv) {
   /* doctest owns argv once it runs, so it sees only the binary name. */
   doctest::Context context(1, argv);
   context.setAsDefaultForAssertsOutOfTestCases();
   CHECK(argc >= 3);
   if (argc >= 2) {
      g_handler = argv[1];
   }
   if (argc >= 3) {
      g_fakeApp = argv[2];
   }
   return context.run();
}
