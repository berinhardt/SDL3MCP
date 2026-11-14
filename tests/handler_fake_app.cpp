/*
  A fake application for the Handler's --targets tests.

  It is a process a targets entry can name: it dials the endpoint the Handler
  appends as --mcp=HOST:PORT, completes the private handshake, and answers the
  four application tools without touching SDL, a window, or a renderer.

  sdl_info returns fixed text. sdl_capture returns the library's raw framing at
  --capture-width by --capture-height (default 2x2). sdl_input returns the same
  framing with the F+1 text. sdl_terminate answers "termination requested" and
  exits, like the real library.

  The switches exist only so a test can exercise the spawn outcomes:
  --pid-file proves which process was started and whether it survived;
  --dial-delay-ms postpones the dial (a spawn timeout); --exit-before-connect
  exits without dialing (an app that dies first); --initialize-delay-ms proves
  the spawn waits for the handshake instead of returning when the socket opens.
  --browser-url reads the bridge endpoint from a page URL, so the same process
  stands in for the browser a web spawn starts.
*/

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <charconv>
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

#include "ToolDefinitions.h"

namespace {

constexpr int kPollMs = 50;
constexpr std::string_view kProtocolVersion = "2025-11-25";

/* The dialed socket is the MCP channel; stdout is diagnostics and goes to stderr. */
int g_outputFd = -1;

void writeAll(int fd, const void* data, std::size_t size) {
   const char* bytes = static_cast<const char*>(data);
   std::size_t offset = 0;
   while (offset < size) {
      const ssize_t written = ::write(fd, bytes + offset, size - offset);
      if (written < 0) {
         if (errno == EINTR) {
            continue;
         }
         return;
      }
      if (written == 0) {
         return;
      }
      offset += static_cast<std::size_t>(written);
   }
}

struct Options {
   std::string mcpAddress;
   std::string browserUrl;
   std::string pidFile;
   std::string argvFile;
   int dialDelayMs = 0;
   int initializeDelayMs = 0;
   int exitCode = 0;
   int captureWidth = 2;
   int captureHeight = 2;
   bool exitBeforeConnect = false;
   /* A real app may outlive the Handler's socket; this proves the Handler does
      not signal or reap it. */
   bool stayAfterClose = false;
};

bool parseInteger(std::string_view text, int& value) {
   const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
   return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

/* The page query the browser was opened with: the mcp parameter is the bridge. */
bool mcpQueryEndpoint(const std::string& url, std::string& endpoint) {
   const std::size_t question = url.find('?');
   if (question == std::string::npos) {
      return false;
   }

   std::size_t position = question + 1;
   while (position < url.size()) {
      const std::size_t equals = url.find('=', position);
      const std::size_t ampersand = url.find('&', position);
      if (equals == std::string::npos ||
          (ampersand != std::string::npos && equals > ampersand)) {
         return false;
      }
      const std::string name = url.substr(position, equals - position);
      const std::size_t valueEnd =
          ampersand == std::string::npos ? url.size() : ampersand;
      if (name == "mcp") {
         endpoint = url.substr(equals + 1, valueEnd - (equals + 1));
         return !endpoint.empty();
      }
      if (ampersand == std::string::npos) {
         return false;
      }
      position = ampersand + 1;
   }
   return false;
}

bool parseOptions(int argc, char** argv, Options& options) {
   for (int index = 1; index < argc; ++index) {
      const std::string_view argument = argv[index];
      const bool hasValue = index + 1 < argc;

      if (argument.rfind("--mcp=", 0) == 0) {
         options.mcpAddress = std::string(argument.substr(6));
      } else if (argument.rfind("--browser-url=", 0) == 0) {
         options.browserUrl = std::string(argument.substr(14));
      } else if (argument == "--browser-url" && hasValue) {
         options.browserUrl = argv[++index];
      } else if (argument == "--pid-file" && hasValue) {
         options.pidFile = argv[++index];
      } else if (argument == "--argv-file" && hasValue) {
         options.argvFile = argv[++index];
      } else if (argument == "--dial-delay-ms" && hasValue) {
         if (!parseInteger(argv[++index], options.dialDelayMs) ||
             options.dialDelayMs < 0) {
            return false;
         }
      } else if (argument == "--initialize-delay-ms" && hasValue) {
         if (!parseInteger(argv[++index], options.initializeDelayMs) ||
             options.initializeDelayMs < 0) {
            return false;
         }
      } else if (argument == "--exit-code" && hasValue) {
         if (!parseInteger(argv[++index], options.exitCode)) {
            return false;
         }
      } else if (argument == "--capture-width" && hasValue) {
         if (!parseInteger(argv[++index], options.captureWidth) ||
             options.captureWidth <= 0) {
            return false;
         }
      } else if (argument == "--capture-height" && hasValue) {
         if (!parseInteger(argv[++index], options.captureHeight) ||
             options.captureHeight <= 0) {
            return false;
         }
      } else if (argument == "--exit-before-connect") {
         options.exitBeforeConnect = true;
      } else if (argument == "--stay-after-close") {
         options.stayAfterClose = true;
      } else {
         return false;
      }
   }
   if (options.mcpAddress.empty() && !options.browserUrl.empty()) {
      if (!mcpQueryEndpoint(options.browserUrl, options.mcpAddress)) {
         return false;
      }
   }
   return !options.mcpAddress.empty();
}

/* Splits HOST:PORT or [V6]:PORT; the Handler only ever appends those shapes. */
bool splitAddress(const std::string& text, std::string& host, std::string& port) {
   if (!text.empty() && text.front() == '[') {
      const std::size_t closing = text.find(']');
      if (closing == std::string::npos || closing + 2 >= text.size() ||
          text[closing + 1] != ':') {
         return false;
      }
      host = text.substr(1, closing - 1);
      port = text.substr(closing + 2);
      return true;
   }

   const std::size_t separator = text.rfind(':');
   if (separator == std::string::npos || separator == 0 ||
       separator + 1 >= text.size()) {
      return false;
   }
   host = text.substr(0, separator);
   port = text.substr(separator + 1);
   return true;
}

int dialAddress(const std::string& text) {
   std::string host;
   std::string port;
   if (!splitAddress(text, host, port)) {
      return -1;
   }

   addrinfo hints{};
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   addrinfo* addresses = nullptr;
   if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
      return -1;
   }

   int fd = -1;
   for (const addrinfo* address = addresses; address != nullptr;
        address = address->ai_next) {
      const int candidate =
          ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (candidate < 0) {
         continue;
      }
      if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
         fd = candidate;
         break;
      }
      ::close(candidate);
   }

   ::freeaddrinfo(addresses);
   return fd;
}

/* Retries for a moment: the Handler binds its listener before it forks. */
int dialWithRetry(const std::string& address, int timeoutMs) {
   const auto deadline =
       std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
   for (;;) {
      const int fd = dialAddress(address);
      if (fd >= 0) {
         return fd;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         return -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
   }
}

void writePidFile(const std::string& path) {
   if (path.empty()) {
      return;
   }
   std::FILE* file = std::fopen(path.c_str(), "w");
   if (file == nullptr) {
      return;
   }
   std::fprintf(file, "%d\n", static_cast<int>(::getpid()));
   std::fclose(file);
}

/* The argv the Handler exec'd, one argument per line, for the endpoint check. */
void writeArgvFile(const std::string& path, int argc, char** argv) {
   if (path.empty()) {
      return;
   }
   std::FILE* file = std::fopen(path.c_str(), "w");
   if (file == nullptr) {
      return;
   }
   for (int index = 0; index < argc; ++index) {
      std::fprintf(file, "%s\n", argv[index]);
   }
   std::fclose(file);
}

void writeMessage(const nlohmann::json& message) {
   std::string line = message.dump();
   line.push_back('\n');
   writeAll(g_outputFd, line.data(), line.size());
}

void writeResult(const nlohmann::json& id, nlohmann::json result) {
   writeMessage(
       nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void writeTextResult(const nlohmann::json& id, std::string_view text) {
   writeResult(id, nlohmann::json{
                       {"content", nlohmann::json::array({
                                       nlohmann::json{{"type", "text"},
                                                      {"text", std::string(text)}},
                                   })},
                       {"isError", false},
                   });
}

/* One raw-framing result: the header line, then exactly byteLength pixels. */
void writeFrameResult(const nlohmann::json& id,
                      const Options& options,
                      std::string_view text,
                      std::uint64_t frame) {
   const std::size_t byteLength =
       static_cast<std::size_t>(options.captureWidth) *
       static_cast<std::size_t>(options.captureHeight) * 4;

   nlohmann::json header{
       {"jsonrpc", "2.0"},
       {"id", id},
       {"result", nlohmann::json{
                      {"format", "rgba8888"},
                      {"width", options.captureWidth},
                      {"height", options.captureHeight},
                      {"byteLength", byteLength},
                      {"frame", frame},
                  }},
   };
   if (!text.empty()) {
      header["result"]["text"] = std::string(text);
   }
   std::string line = header.dump();
   line.push_back('\n');
   writeAll(g_outputFd, line.data(), line.size());

   std::vector<std::uint8_t> pixels(byteLength, 0x60);
   writeAll(g_outputFd, pixels.data(), pixels.size());
}

/* The library's own metadata, so a bare app is an identity on tools/list. */
nlohmann::json toolsResult() {
   nlohmann::json tools = nlohmann::json::array();
   for (const sdl3mcp::ToolDefinition& tool : sdl3mcp::applicationToolDefinitions()) {
      tools.push_back(nlohmann::json{
          {"name", tool.name},
          {"description", tool.description},
          {"inputSchema", tool.inputSchema},
          {"annotations", tool.annotations},
      });
   }
   return nlohmann::json{{"tools", std::move(tools)}};
}

/* Reads one newline-terminated line from `fd`, or false on close. */
bool readLine(int fd, std::string& buffer, std::string& line) {
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

      pollfd descriptor{};
      descriptor.fd = fd;
      descriptor.events = POLLIN;
      const int ready = ::poll(&descriptor, 1, kPollMs);
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
      const ssize_t count = ::read(fd, chunk, sizeof chunk);
      if (count <= 0) {
         return false;
      }
      buffer.append(chunk, static_cast<std::size_t>(count));
   }
}

int serve(int fd, const Options& options) {
   g_outputFd = fd;
   std::string buffer;
   std::string line;

   const auto readMessage = [&](nlohmann::json& message) {
      if (!readLine(fd, buffer, line)) {
         return false;
      }
      message = nlohmann::json::parse(line, nullptr, false);
      return !message.is_discarded();
   };

   nlohmann::json message;
   if (!readMessage(message) || message.value("method", std::string()) !=
                                    "initialize") {
      return 2;
   }
   if (options.initializeDelayMs > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(options.initializeDelayMs));
   }
   writeResult(message["id"], nlohmann::json{
                                  {"protocolVersion", std::string(kProtocolVersion)},
                                  {"capabilities",
                                   nlohmann::json{{"tools", nlohmann::json::object()}}},
                                  {"serverInfo", nlohmann::json{{"name", "fake-target"},
                                                                {"version", "0.0.0"}}},
                              });

   if (!readMessage(message) ||
       message.value("method", std::string()) != "notifications/initialized") {
      return 2;
   }
   if (!readMessage(message) ||
       message.value("method", std::string()) != "tools/list") {
      return 2;
   }
   writeResult(message["id"], toolsResult());

   for (;;) {
      if (!readMessage(message)) {
         if (options.stayAfterClose) {
            /* The Handler going away is not this app's exit. */
            for (;;) {
               ::pause();
            }
         }
         return 0;
      }
      if (message.value("method", std::string()) != "tools/call") {
         continue;
      }
      const nlohmann::json& params = message["params"];
      const std::string name = params.value("name", std::string());
      if (name == "sdl_info") {
         writeTextResult(message["id"], "fake target app info");
      } else if (name == "sdl_capture") {
         writeFrameResult(message["id"], options, {}, 5);
      } else if (name == "sdl_input") {
         writeFrameResult(message["id"], options,
                          "1 actions applied at frame 4; frame 5 attached, rendered "
                          "after the actions were delivered",
                          5);
      } else if (name == "sdl_terminate") {
         writeTextResult(message["id"], "termination requested");
         return 0;
      } else {
         writeMessage(nlohmann::json{
             {"jsonrpc", "2.0"},
             {"id", message["id"]},
             {"error", nlohmann::json{{"code", -32602},
                                      {"message", "unknown tool: " + name}}},
         });
      }
   }
}

}  // namespace

int main(int argc, char** argv) {
   Options options;
   if (!parseOptions(argc, argv, options)) {
      std::fprintf(stderr, "fake-target: invalid command line\n");
      return 2;
   }

   writePidFile(options.pidFile);
   writeArgvFile(options.argvFile, argc, argv);
   if (options.exitBeforeConnect) {
      return options.exitCode;
   }
   if (options.dialDelayMs > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(options.dialDelayMs));
   }

   const int fd = dialWithRetry(options.mcpAddress, 2000);
   if (fd < 0) {
      std::fprintf(stderr, "fake-target: cannot dial %s\n",
                   options.mcpAddress.c_str());
      return 1;
   }

   const int status = serve(fd, options);
   ::close(fd);
   return status;
}
