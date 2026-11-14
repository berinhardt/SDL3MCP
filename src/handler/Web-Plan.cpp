#include "Web-Plan.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sdl3mcp {
namespace {

constexpr const char* kPythonExecutable = "python3";
constexpr const char* kWebSocatExecutable = "websocat";
constexpr const char* kBrowserWidth = "1280";
constexpr const char* kBrowserHeight = "800";

std::string baseName(const std::string& path) {
   const std::size_t separator = path.find_last_of('/');
   return separator == std::string::npos ? path : path.substr(separator + 1);
}

bool isExecutable(const std::string& path) {
   return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

/* realpath() turns a PATH hit into the absolute path the plan prints. */
bool absolutePath(const std::string& path, std::string& absolute) {
   char* resolved = ::realpath(path.c_str(), nullptr);
   if (resolved == nullptr) {
      return false;
   }
   absolute.assign(resolved);
   std::free(resolved);
   return true;
}

bool findOnPath(const char* name, std::string& found) {
   const char* path = std::getenv("PATH");
   if (path == nullptr) {
      return false;
   }

   /* An empty entry would mean the current directory; skip it. */
   std::string_view remaining(path);
   while (!remaining.empty()) {
      const std::size_t separator = remaining.find(':');
      const std::string_view entry = remaining.substr(0, separator);
      if (!entry.empty()) {
         std::string candidate(entry);
         candidate += '/';
         candidate += name;
         if (isExecutable(candidate) && absolutePath(candidate, found)) {
            return true;
         }
      }
      if (separator == std::string_view::npos) {
         break;
      }
      remaining.remove_prefix(separator + 1);
   }
   return false;
}

}  // namespace

Result<void, TransportError> preflightWebLaunch(WebLaunchOptions& options) {
   std::string websocat;
   if (!findOnPath(kWebSocatExecutable, websocat)) {
      return TransportError{
          "websocat not found: install websocat (cargo install websocat)"};
   }

   std::string python;
   if (!findOnPath(kPythonExecutable, python)) {
      return TransportError{"python3 not found: install python3"};
   }

   struct stat status{};
   if (::stat(options.pageRoot.c_str(), &status) != 0 ||
       !S_ISDIR(status.st_mode)) {
      return TransportError{"page root " + options.pageRoot +
                            " is not a directory"};
   }
   const std::string entryPath = options.pageRoot + "/" + options.entry;
   if (::stat(entryPath.c_str(), &status) != 0) {
      return TransportError{"page root " + options.pageRoot +
                            " does not contain " + options.entry};
   }

   const Result<std::string, TransportError> browser = resolveBrowser();
   if (!browser) {
      return browser.error();
   }

   options.websocat = std::move(websocat);
   options.python = std::move(python);
   options.browser = *browser;
   return {};
}

Result<std::uint16_t, TransportError> pickLoopbackPort() {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      return TransportError{std::string("cannot open a loopback socket: ") +
                            std::strerror(errno)};
   }

   int reuse = 1;
   (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

   sockaddr_in address{};
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = 0;
   if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) !=
       0) {
      const int bindError = errno;
      ::close(fd);
      return TransportError{std::string("cannot reserve a loopback port: ") +
                            std::strerror(bindError)};
   }

   socklen_t length = sizeof address;
   if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      const int nameError = errno;
      ::close(fd);
      return TransportError{
          std::string("cannot read the reserved loopback port: ") +
          std::strerror(nameError)};
   }

   const std::uint16_t port = ntohs(address.sin_port);
   ::close(fd);
   return port;
}

Result<void, TransportError> pickWebPorts(WebLaunchOptions& options) {
   const Result<std::uint16_t, TransportError> page = pickLoopbackPort();
   if (!page) {
      return page.error();
   }

   /*
     A released port can be chosen again, so the bridge is retried until it
     differs; the page server binds first, which makes a collision a real
     startup failure rather than a plan-time guess.
   */
   for (int attempt = 0; attempt < 16; ++attempt) {
      const Result<std::uint16_t, TransportError> bridge = pickLoopbackPort();
      if (!bridge) {
         return bridge.error();
      }
      if (*bridge != *page) {
         options.pagePort = *page;
         options.bridgePort = *bridge;
         return {};
      }
   }

   return TransportError{"cannot reserve two distinct loopback ports"};
}

Result<std::vector<std::vector<std::string>>, TransportError>
makeWebLaunchPlan(const WebLaunchOptions& options) {
   std::vector<std::vector<std::string>> plan;

   if (options.pageRoot.empty()) {
      return TransportError{"pageRoot must not be empty"};
   }
   if (options.entry.empty()) {
      return TransportError{"entry must not be empty"};
   }
   if (!isExecutable(options.python)) {
      return TransportError{"python must name an executable"};
   }
   if (!isExecutable(options.websocat)) {
      return TransportError{"websocat must name an executable"};
   }
   if (options.browser.empty()) {
      return TransportError{"browser must not be empty"};
   }
   if (options.handlerEndpoint.empty()) {
      return TransportError{"handlerEndpoint must not be empty"};
   }
   if (!options.placeholderPorts) {
      if (options.pagePort == 0) {
         return TransportError{"pagePort must not be zero"};
      }
      if (options.bridgePort == 0) {
         return TransportError{"bridgePort must not be zero"};
      }
      if (options.pagePort == options.bridgePort) {
         return TransportError{"pagePort and bridgePort must differ"};
      }
   }

   const std::string pagePort = options.placeholderPorts
                                    ? "PAGE_PORT"
                                    : std::to_string(options.pagePort);
   const std::string bridgePort = options.placeholderPorts
                                      ? "BRIDGE_PORT"
                                      : std::to_string(options.bridgePort);
   const std::string bridgeEndpoint =
       std::string(kWebLoopbackHost) + ":" + bridgePort;

   std::vector<std::string> pageServer{
       options.python,
       "-m",
       "http.server",
       pagePort,
       "--bind",
       kWebLoopbackHost,
       "--directory",
       options.pageRoot,
   };

   std::vector<std::string> bridge{
       options.websocat,
       "--binary",
       "ws-l:" + bridgeEndpoint,
       "tcp:" + options.handlerEndpoint,
   };

   std::vector<std::string> browser{
       options.browser,
       "--no-remote",
       "--new-instance",
       "--profile",
       options.profileDir,
       "--width",
       kBrowserWidth,
       "--height",
       kBrowserHeight,
       "http://" + std::string(kWebLoopbackHost) + ":" + pagePort + "/" +
           options.entry + "?mcp=" + bridgeEndpoint,
   };

   plan.push_back(std::move(pageServer));
   plan.push_back(std::move(bridge));
   plan.push_back(std::move(browser));
   return plan;
}

Result<std::string, TransportError> resolveBrowser() {
   const char* configured = std::getenv("BROWSER");
   if (configured != nullptr) {
      const std::string candidate(configured);
      const std::string name = baseName(candidate);
      std::string absolute;
      if ((name == "firefox" || name == "firefox-esr") &&
          isExecutable(candidate) && absolutePath(candidate, absolute)) {
         return absolute;
      }
   }

   for (const char* name : {"firefox", "firefox-esr"}) {
      std::string found;
      if (findOnPath(name, found)) {
         return found;
      }
   }

   return TransportError{
       "no isolated browser available: install firefox; "
       "no other browser is supported"};
}

}  // namespace sdl3mcp
